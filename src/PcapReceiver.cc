/*
 * Copyright (C) 2026 Alfredo Tupone
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Own interface
#include <atu_reactor/PcapReceiver.h>

// System headers
#include <cstring>
#include <fcntl.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Fallback for non-standard Linux headers
#ifndef ETHERTYPE_VLAN
#define ETHERTYPE_VLAN 0x8100
#endif

// Link Types
#ifndef DLT_EN10MB
#define DLT_EN10MB 1
#endif
#ifndef DLT_LINUX_SLL
#define DLT_LINUX_SLL 113
#endif

template <typename T>
[[nodiscard]] inline T maybeSwap(T val, bool swapped) noexcept {
    if constexpr (sizeof(T) == 2) return swapped ? __builtin_bswap16(val) : val;
    if constexpr (sizeof(T) == 4) return swapped ? __builtin_bswap32(val) : val;
    if constexpr (sizeof(T) == 8) return swapped ? __builtin_bswap64(val) : val;
}

namespace atu_reactor {

PcapReceiver::PcapReceiver(EventLoop& loopRef, PcapConfig config)
        : PacketReceiver(loopRef, config), m_pcapConfig(config),
        m_finished(false)
{
}

PcapReceiver::~PcapReceiver() {
    if (m_mappedData && m_mappedData != MAP_FAILED) {
        munmap(m_mappedData, m_fileSize);
    }
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

Result<void> PcapReceiver::open(const std::string& path) {
    checkThread();

    // 1. Open File
    m_fd = ::open(path.c_str(), O_RDONLY);
    if (m_fd < 0) return std::error_code(errno, std::system_category());

    // 2. Get Size
    struct stat st;
    if (fstat(m_fd, &st) < 0) {
        ::close(m_fd);
        m_fd = -1;
        return std::error_code(errno, std::system_category());
    }
    m_fileSize = st.st_size;

    // 3. Map into Memory
    void* mapped = mmap(nullptr, m_fileSize, PROT_READ, MAP_PRIVATE | MAP_POPULATE, m_fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(m_fd);
        m_fd = -1;
        return std::error_code(errno, std::system_category());
    }

    // Convert to uint8_t* for arithmetic
    m_mappedData = static_cast<uint8_t*>(mapped);
    madvise(m_mappedData, m_fileSize, MADV_SEQUENTIAL | MADV_WILLNEED);

    // 4. Parse Global Header (24 bytes)
    if (m_fileSize < sizeof(pcap_file_header)) {
        return std::error_code(EINVAL, std::system_category());
    }

    auto* g_hdr = reinterpret_cast<const pcap_file_header*>(m_mappedData);
    m_currentPtr = m_mappedData;

    // Detect format based on Magic Number
    if (g_hdr->magic_number == MAGIC_PCAPNG_SHB) {
        m_isPcapNg = true;

        // In PcapNg, the Byte Order Magic is at offset 8 (inside the SHB body)
        const uint32_t* ptr32 = reinterpret_cast<const uint32_t*>(m_mappedData);
        uint32_t byteOrderMagic = ptr32[2];

        if (byteOrderMagic == PCAPNG_BOM) {
            m_swapped = false;
        } else if (byteOrderMagic == PCAPNG_BOM_SWAP) {
            m_swapped = true;
        } else {
            return std::error_code(EINVAL, std::system_category());
        }

        // Default PcapNg resolution is usually micro or nanoseconds.
        // Real implementation should parse the IDB (Interface Description Block) 'if_tsresol' option.
        // For now, we assume standard nanoseconds for simplicity.
        m_isNanosecond = true;

        // Start reading immediately after the pointer (will be handled by stepPcapNg)
    } else {
        m_isPcapNg = false;

        switch (g_hdr->magic_number) {
            case MAGIC_MICRO_LE: m_swapped = true;  m_isNanosecond = false; break;
            case MAGIC_NANO_LE:  m_swapped = true;  m_isNanosecond = true;  break;
            case MAGIC_NANO_BE:  m_swapped = false; m_isNanosecond = true;  break;
            case MAGIC_MICRO_BE: m_swapped = false; m_isNanosecond = false; break;
            default: return std::error_code(EINVAL, std::system_category());
        }
        // Check magic number for byte swapping
        m_linkType = maybeSwap(g_hdr->network, m_swapped);

        // Set cursor to start of first packet
        m_currentPtr += sizeof(pcap_file_header);
    }

    return Result<void>::success();
}

void PcapReceiver::rewind() {
    checkThread();
    m_currentPtr = m_mappedData;
    // Move cursor back to the start of the first packet
    if (!m_isPcapNg) {
        // right after global header
        m_currentPtr += sizeof(pcap_file_header);
    }
    m_finished = false;
    m_firstPacket = true;
}

Result<int> PcapReceiver::subscribe(uint16_t port,
                                    void* context,
                                    PacketHandlerFn handler) {
    // Call base class for checkThread and standard bookkeeping
    auto baseRes = PacketReceiver::subscribe(port, context, handler);
    if (!baseRes) {
        return baseRes;
    }

    // Convert to Network Byte Order ONCE
    uint16_t netPort = htons(port);

    // Perform PcapReceiver specific registration.
    m_portTable[netPort] = {context, handler};

    // Return the port as the ID.
    // This allows the caller to treat the port as the 'handle' for this subscription.
    return static_cast<int>(port);
}

Result<void> PcapReceiver::unsubscribe(uint16_t port) {
    auto baseResult = PacketReceiver::unsubscribe(port);
    if (!baseResult) {
        return baseResult;
    }

    // Convert to Network Byte Order ONCE
    uint16_t netPort = htons(port);

    m_portTable[netPort] = {};

    // Clear hot cache if this port was cached
    if (m_hotPort == netPort) {
        m_hotPort = 0;
        m_hotHandler = nullptr;
        m_hotContext = nullptr;
    }

    return Result<void>::success();
}

void PcapReceiver::start() {
    checkThread();
    if (!m_mappedData) return;

    m_firstPacket = true;

    // In STEP mode, we do nothing. User must call step().
    if (m_pcapConfig.mode == ReplayMode::STEP) {
        return;
    }

    // For TIMED or FLOOD, schedule the first batch immediately
    m_loop.runAfter(std::chrono::milliseconds(0), &PcapReceiver::processBatchProxy, this);
}

bool PcapReceiver::step() {
    checkThread(); // Safety check only happens here
    return internalStep();
}

// The core logic: Reads one packet from memory
bool PcapReceiver::internalStep() noexcept {
    // New Delegation
    if (m_isPcapNg) {
        return stepPcapNg();
    }

    // EOF Check
    if (m_currentPtr + sizeof(pcap_sf_pkthdr) > m_mappedData + m_fileSize) [[unlikely]] {
        m_finished = true;
        return false;
    }

    // Pointer to Packet Header on Disk
    auto* disk_hdr = reinterpret_cast<const pcap_sf_pkthdr*>(m_currentPtr);

    // Read and potentially swap fields
    uint32_t sec      = maybeSwap(disk_hdr->ts_sec,  m_swapped);
    uint32_t fraction = maybeSwap(disk_hdr->ts_usec, m_swapped);
    uint32_t caplen   = maybeSwap(disk_hdr->caplen,  m_swapped);
    uint32_t len      = maybeSwap(disk_hdr->len,     m_swapped);

    // Create a compatible in-memory header
    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = static_cast<long>(fraction);
    if (!m_isNanosecond) {
        ts.tv_nsec *= 1000;
    }

    // TIMED Mode Check: Is it too early?
    if (m_pcapConfig.mode == ReplayMode::TIMED) {
        auto targetTime = calculateTargetTimeHighRes(ts);
        auto now = std::chrono::steady_clock::now();

        if (targetTime > now) {
            // It's in the future.
            // We return FALSE so the loop stops, but we DO NOT advance m_currentPtr.
            // We reschedule the loop to wake up at targetTime.
            auto delay = std::chrono::duration_cast<Duration>(targetTime - now);
            m_loop.runAfter(delay, &PcapReceiver::processBatchProxy, this);
            return false;
        }
    }

    // Packet Data starts immediately after header
    const uint8_t* packet_data = m_currentPtr + sizeof(pcap_sf_pkthdr);

    // Dispatch with precision
    // Passing caplen and len explicitly since we've already handled their endianness
    parseAndDispatch(ts, caplen, len, packet_data, m_linkType);

    // Advance Cursor
    m_currentPtr = packet_data + caplen;
    return true;
}

bool PcapReceiver::stepPcapNg() noexcept {
    uint32_t len;

    while (true) {
        // EOF Check
        if (m_currentPtr + sizeof(PcapNgBlockHeader) > m_mappedData + m_fileSize) {
            m_finished = true;
            return false;
        }

        auto* bh = reinterpret_cast<const PcapNgBlockHeader*>(m_currentPtr);
        uint32_t type = maybeSwap(bh->type,        m_swapped);
        len           = maybeSwap(bh->totalLength, m_swapped);

        // Safety check
        if (len < sizeof(PcapNgBlockHeader) || m_currentPtr + len > m_mappedData + m_fileSize) [[unlikely]] {
            m_finished = true;
            return false;
        }

        // 1. Enhanced Packet Block (EPB) - Type 6
        if (type == PCAPNG_EPB) [[likely]] {
            break;
        }

        if (type == PCAPNG_IDB) {
            if (m_interfaceCount >= MAX_INTERFACES) [[unlikely]] {
                // Ignore interfaces beyond limit or handle accordingly
                m_currentPtr += len;
                continue;
            }

            auto* idb = reinterpret_cast<const PcapNgIDBBody*>(m_currentPtr + sizeof(PcapNgBlockHeader));
            InterfaceInfo info;
            info.linkType = maybeSwap(idb->linkType, m_swapped);
            info.tsResolutionDivisor = 1000000; // Default to micro (10^6)

            // Parse IDB Options for resolution (if_tsresol)
            const uint8_t* optPtr = m_currentPtr + sizeof(PcapNgBlockHeader) + sizeof(PcapNgIDBBody);
            const uint8_t* blockEnd = m_currentPtr + len - 4; // last 4 bytes is the length again

            while (optPtr + 4 <= blockEnd) {
                uint16_t code;
                std::memcpy(&code, optPtr, sizeof(code));
                code = maybeSwap(code, m_swapped);
                if (code == 0) break; // End of options

                uint16_t vlen;
                std::memcpy(&vlen, optPtr + 2, sizeof(code));
                vlen = maybeSwap(vlen, m_swapped);

                if (code == 9 && vlen == 1) { // if_tsresol
                    uint8_t res = *(optPtr + 4);
                    info.tsResolutionDivisor = (res & 0x80) ? (1ULL << (res & 0x7F)) : 1;
                    if (!(res & 0x80)) for(int i=0; i<res; ++i) info.tsResolutionDivisor *= 10;
                }
                optPtr += 4 + ((vlen + 3) & ~3); // Padding to 32-bit
            }
            m_interfaces[m_interfaceCount++] = info;
        }

        // 2. Skip other blocks (SHB, Statistics, etc.)
        m_currentPtr += len;

        // Loop continues until we find a packet or EOF
    }

    auto* epb = reinterpret_cast<const PcapNgEPBBody*>(m_currentPtr + sizeof(PcapNgBlockHeader));
    uint32_t ifId = maybeSwap(epb->interfaceId, m_swapped);

    if (ifId >= m_interfaceCount) [[unlikely]] {
        return false; // Invalid interface ID
    }

    auto& info = m_interfaces[ifId]; // Assuming IDB appeared before EPB
    uint64_t high  = maybeSwap(epb->timestampHigh, m_swapped);
    uint64_t low   = maybeSwap(epb->timestampLow,  m_swapped);
    uint64_t tsRaw = (high << 32) | low;

    // Convert to timespec (Assuming standard resolution of units per second)
    // Note: Robust implementations parse IDB options for resolution.
    // Simplification: Assuming 1 unit = 1 microsecond (common default) or 1ns
    // You may need to tune this divisor based on your capture source.
    struct timespec ts;
    ts.tv_sec  = tsRaw / info.tsResolutionDivisor;
    ts.tv_nsec = (tsRaw % info.tsResolutionDivisor) * 1000000000ULL / info.tsResolutionDivisor;

    // check TIMED mode
    if (m_pcapConfig.mode == ReplayMode::TIMED) {
        auto targetTime = calculateTargetTimeHighRes(ts);
        auto now = std::chrono::steady_clock::now();
        if (targetTime > now) {
            auto delay = std::chrono::duration_cast<Duration>(targetTime - now);
            m_loop.runAfter(delay, &PcapReceiver::processBatchProxy, this);
            return false; // Valid wait, do not advance pointer
        }
    }

    uint32_t capLen  = maybeSwap(epb->capLen,  m_swapped);
    uint32_t origLen = maybeSwap(epb->origLen, m_swapped);

    // Packet data starts after the EPB body
    const uint8_t* dataPtr = m_currentPtr + sizeof(PcapNgBlockHeader) + sizeof(PcapNgEPBBody);

    parseAndDispatch(ts, capLen, origLen, dataPtr, info.linkType);

    m_currentPtr += len; // Advance to next block
    return true;
}

void PcapReceiver::processBatch() {
    if (!m_mappedData || m_finished) return;

    // Dispatch to optimized loop for the flood case
    if (m_pcapConfig.mode == ReplayMode::FLOOD) {
        processBatchFlood();
        if (m_finished) return;
    }

    int totalProcessed = 0;
    const int stopLimit = m_pcapConfig.batchSize;

    while (totalProcessed < stopLimit) {
        // We prefetch ~128 bytes ahead of the current pointer.
        // This usually covers the next packet's Pcap header and Layer 2/3 headers.
        if (m_currentPtr + 128 < m_mappedData + m_fileSize) [[likely]] {
            __builtin_prefetch(m_currentPtr + 128, 0, 3);
        }

        // step() returns false if EOF or if we are waiting for time
        if (!internalStep()) {
            return;
        }
        totalProcessed++;
    }

    if (m_finished) [[unlikely]] {
        return;
    }

    // Yield to event loop if we are just flooding (avoid freezing the app)
    if (m_pcapConfig.mode == ReplayMode::FLOOD) {
        m_loop.runInLoop(&PcapReceiver::processBatchProxy, this);
    }
    // Note: In TIMED mode, step() handles the rescheduling when it hits a future packet.
    // If the batch finished but next packet is valid (catch-up scenario), schedule immediate continuation.
    else if (m_pcapConfig.mode == ReplayMode::TIMED) {
         m_loop.runAfter(Duration(0), &PcapReceiver::processBatchProxy, this);
    }
}

void PcapReceiver::processBatchFlood() {
    constexpr int stopLimit = 20000;
    constexpr int lookAhead = 512;

    for (int i = 0; i < stopLimit; i++) {
        // Calculate the next packet start if possible.
        // We look "ahead" of the current packet size.
        // Assuming a standard PCAP header (16 bytes) + typical packet
        __builtin_prefetch(m_currentPtr + lookAhead, 0, 3);

        // internalStep() returns false if EOF or if we are waiting for time
        if (!internalStep()) [[unlikely]] {
            return;
        }
    }
    if (m_finished) [[unlikely]] {
        return;
    }

    m_loop.runInLoop(&PcapReceiver::processBatchFloodProxy, this);
}



std::chrono::steady_clock::time_point PcapReceiver::calculateTargetTimeHighRes(
        const struct timespec& ts)
{
    if (m_firstPacket) [[unlikely]] {
        m_pcapStartTs = ts;
        m_wallStartTs = std::chrono::steady_clock::now();
        m_firstPacket = false;
        return m_wallStartTs;
    }

    int64_t diff_sec = static_cast<int64_t>(ts.tv_sec) - m_pcapStartTs.tv_sec;
    int64_t diff_ns  = static_cast<int64_t>(ts.tv_nsec) - m_pcapStartTs.tv_nsec;

    // Normalize nanoseconds if negative (e.g., ts.tv_nsec < start.tv_nsec)
    if (diff_ns < 0) {
        diff_sec -= 1;
        diff_ns += 1000000000L;
    }

    if (m_pcapConfig.speedMultiplier == 1.0) [[likely]] {
        return m_wallStartTs + std::chrono::seconds(diff_sec) + std::chrono::nanoseconds(diff_ns);
    }

    double total_ns = (static_cast<double>(diff_sec) * 1e9 + static_cast<double>(diff_ns))
                      / m_pcapConfig.speedMultiplier;

    return m_wallStartTs + std::chrono::nanoseconds(static_cast<int64_t>(total_ns));
}

// This is portable AND fast because the compiler optimizes the array access
inline bool isFastPathIPv4(const uint8_t* p) {
    // p+12 is EtherType, p+14 is IP Version
    return p[12] == 0x08 && p[13] == 0x00 && (p[14] & 0xF0) == 0x40;
}

void PcapReceiver::parseAndDispatch(
        const struct timespec& ts,
        uint32_t caplen,
        uint32_t len,
        const uint8_t* packet,
        uint32_t linkType)
{
    // Determine status: If captured length < original length, it's truncated
    PacketStatus status = (caplen < len) ? PacketStatus::TRUNCATED : PacketStatus::OK;

    // Validate Link Type (Done once during init, but used here)
    // If m_linkType != 1 (Mapping to DLT_EN10MB), we MUST use slow path.
    if (m_linkType == DLT_EN10MB && status == PacketStatus::OK && caplen >= 42) [[likely]] {
        // The FAST PATH
        if (isFastPathIPv4(packet) && packet[23] == IPPROTO_UDP) [[likely]] {
            // NOW we need IHL to find the start of the UDP header
            // We only do this math because we KNOW we want this packet.
            uint8_t ihl = packet[14] & 0x0F;
            uint32_t ipHeaderLen = ihl << 2;
            uint32_t totalHeaderLen = 14 + ipHeaderLen + 8; // Eth + IP + UDP

            if (caplen < totalHeaderLen) [[unlikely]] {
                // Cannot read the udp header
                return;
            }

            // Map the UDP header relative to the actual end of the IP header
            auto* udp = reinterpret_cast<const struct udphdr*>(packet + 14 + ipHeaderLen);

            // Optimized Network-Order Port Table lookup
            uint16_t dstPortNet = udp->uh_dport;

            // CHECK PORT FIRST
            if (dstPortNet != m_hotPort || m_hotHandler == nullptr) [[unlikely]] {
                // Cache Miss or First Packet: Look up the port in our table
                auto& sub = m_portTable[dstPortNet];

                if (!sub.handler) {
                    return; // No subscription for this port
                }

                // Update the hot cache for subsequent packets
                m_hotPort    = dstPortNet;
                m_hotHandler = sub.handler;
                m_hotContext = sub.context;
            }

            // ONLY COMPUTE LENGTHS IF WE HAVE A HANDLER
            uint16_t udpLen = ntohs(udp->uh_ulen);
            if (udpLen < 8) [[unlikely]] {
                // udpLen should be at least its header
                return;
            }

            // This protects against packets where the UDP header is bigger than PCAP.
            if (caplen < (14 + ipHeaderLen + udpLen)) [[unlikely]] {
                return; // Not enough data
            }

            const uint8_t* payload = packet + totalHeaderLen;
            int32_t payloadLen = udpLen - 8;

            m_hotHandler(m_hotContext, payload, payloadLen, status, ts);
            return; // Fast path successful
        }
    }

    // Fallback for VLANs, SLL, or truncated packets
    slowPathParse(ts, caplen, len, packet, linkType);
}

void PcapReceiver::slowPathParse(const struct timespec& ts, uint32_t caplen, uint32_t len,
        const uint8_t* packet, uint32_t linkType) {
    if (caplen != len) [[unlikely]] return; // Ignore truncated in capture

    const uint8_t* ptr = packet;
    uint32_t remaining = caplen;
    uint16_t proto = 0;

    // --- Layer 2 ---
    if (linkType == DLT_LINUX_SLL) {
        if (remaining < 16) return;
        // Protocol is at offset 14 (big endian)
        proto = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 14));
        ptr += 16;
        remaining -= 16;
    }
    else if (linkType == DLT_EN10MB) { // Standard Ethernet
                                       // --- Layer 2: Ethernet ---
        if (remaining < sizeof(struct ether_header)) [[unlikely]] return;
        auto* eth = reinterpret_cast<const struct ether_header*>(ptr);
        proto = ntohs(eth->ether_type);
        ptr += sizeof(struct ether_header);
        remaining -= static_cast<uint32_t>(sizeof(struct ether_header));

        // Handle 802.1Q VLAN Tagging
        if (proto == ETHERTYPE_VLAN) {
            if (remaining < 4) [[unlikely]] return; // VLAN tag size
                                                    // Skip VLAN (simplified, assuming single tag)
            proto = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 2));
            ptr += 4;
            remaining -= 4;
        }
    }
    else [[unlikely]] {
        // Unsupported link type (e.g. DLT_NULL/Loopback or DLT_RAW)
        return;
    }

    if (proto != ETHERTYPE_IP) return;

    // --- Layer 3: IPv4 ---
    if (remaining < sizeof(struct ip)) [[unlikely]] return;
    auto* ip = reinterpret_cast<const struct ip*>(ptr);
    if (ip->ip_v != 4) return;

    uint32_t ipLen = ip->ip_hl * 4;
    if (remaining < ipLen) [[unlikely]] return;

    if (ip->ip_p != IPPROTO_UDP) return;

    ptr += ipLen;
    remaining -= ipLen;

    // --- Layer 4: UDP ---
    if (remaining < sizeof(struct udphdr)) [[unlikely]] return;
    auto* udp = reinterpret_cast<const struct udphdr*>(ptr);

    uint16_t dstPortNet = udp->uh_dport;

    // CHECK PORT FIRST
    if (dstPortNet != m_hotPort || m_hotHandler == nullptr) [[unlikely]] {
        // Cache Miss or First Packet: Look up the port in our table
        auto& sub = m_portTable[dstPortNet];

        if (!sub.handler) {
            return; // No subscription for this port
        }

        // Update the hot cache for subsequent packets
        m_hotPort    = dstPortNet;
        m_hotHandler = sub.handler;
        m_hotContext = sub.context;
    }

    // --- Dispatch ---
    uint16_t udpLen = ntohs(udp->uh_ulen); // Includes header
    if (udpLen < sizeof(struct udphdr)) [[unlikely]] return;

    size_t dataLen = udpLen - sizeof(struct udphdr);

    ptr += sizeof(struct udphdr);
    remaining -= static_cast<uint32_t>(sizeof(struct udphdr));

    if (remaining < dataLen) [[unlikely]] return;

    m_hotHandler(m_hotContext, ptr, dataLen, PacketStatus::OK, ts);
}

void PcapReceiver::processBatchProxy(void* context) {
    if (context) {
        static_cast<PcapReceiver*>(context)->processBatch();
    }
}

void PcapReceiver::processBatchFloodProxy(void* context) {
    if (context) {
        static_cast<PcapReceiver*>(context)->processBatchFlood();
    }
}


} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
