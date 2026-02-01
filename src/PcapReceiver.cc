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

namespace atu_reactor {

// PCAP File Global Header
struct pcap_file_header {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network; // LinkType
};

// PCAP Packet Header (Disk Format)
struct pcap_sf_pkthdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t caplen;
    uint32_t len;
};

PcapReceiver::PcapReceiver(EventLoop& loopRef, PcapConfig config)
        : PacketReceiver(loopRef, config), m_pcapConfig(config),
        m_portTable(std::make_unique<Subscription[]>(65536)),
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
    void* mapped = mmap(nullptr, m_fileSize, PROT_READ, MAP_PRIVATE, m_fd, 0);
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

    // Detect format based on Magic Number
    if (g_hdr->magic_number == 0xd4c3b2a1) {
        m_swapped = true;
        m_isNanosecond = false;
    } else if (g_hdr->magic_number == 0x4d3c2b1a) {
        m_swapped = true;
        m_isNanosecond = true;
    } else if (g_hdr->magic_number == 0xa1b23c4d) {
        m_swapped = false;
        m_isNanosecond = true;
    } else {
        m_swapped = false;
        m_isNanosecond = false;
    }
    // Check magic number for byte swapping
    if (m_swapped) {
        m_linkType = __builtin_bswap32(g_hdr->network);
    } else {
        m_linkType = g_hdr->network;
    }

    // Set cursor to start of first packet
    m_currentPtr = m_mappedData + sizeof(pcap_file_header);

    return Result<void>::success();
}

Result<int> PcapReceiver::subscribe(uint16_t port,
                                    void* context,
                                    PacketHandlerFn handler) {
    // Call base class for checkThread and standard bookkeeping
    auto baseRes = PacketReceiver::subscribe(port, context, handler);
    if (!baseRes) {
        return baseRes;
    }

    // Perform PcapReceiver specific registration.
    m_portTable[port] = {context, handler};

    // Return the port as the ID.
    // This allows the caller to treat the port as the 'handle' for this subscription.
    return static_cast<int>(port);
}

Result<void> PcapReceiver::unsubscribe(uint16_t port) {
    auto baseResult = PacketReceiver::unsubscribe(port);
    if (!baseResult) {
        return baseResult;
    }

    m_portTable[port] = {};
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
    m_loop.runAfter(std::chrono::milliseconds(0), [this]() {
        processBatch();
    });
}

// The core logic: Reads one packet from memory
bool PcapReceiver::step() {
    checkThread();

    // EOF Check
    if (m_currentPtr + sizeof(pcap_sf_pkthdr) > m_mappedData + m_fileSize) {
        m_finished = true;
        printf("\nPCAP EOF reached.\n");
        return false;
    }

    // Pointer to Packet Header on Disk
    auto* disk_hdr = reinterpret_cast<const pcap_sf_pkthdr*>(m_currentPtr);

    // Read and potentially swap fields
    uint32_t sec
        = m_swapped
        ? __builtin_bswap32(disk_hdr->ts_sec)
        : disk_hdr->ts_sec;
    uint32_t fraction
        = m_swapped
        ? __builtin_bswap32(disk_hdr->ts_usec)
        : disk_hdr->ts_usec;
    uint32_t caplen
        = m_swapped
        ? __builtin_bswap32(disk_hdr->caplen)
        : disk_hdr->caplen;
    uint32_t len
        = m_swapped
        ? __builtin_bswap32(disk_hdr->len)
        : disk_hdr->len;

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
            m_loop.runAfter(delay, [this]() {
                this->processBatch();
            });
            return false;
        }
    }

    // Packet Data starts immediately after header
    const uint8_t* packet_data = m_currentPtr + sizeof(pcap_sf_pkthdr);

    // Dispatch with precision
    // Passing caplen and len explicitly since we've already handled their endianness
    parseAndDispatch(ts, caplen, len, packet_data);

    // Advance Cursor
    m_currentPtr = packet_data + caplen;
    return true;
}

void PcapReceiver::processBatch() {
    if (!m_mappedData || m_finished) return;

    int totalProcessed = 0;
    const int stopLimit = (m_pcapConfig.mode == ReplayMode::FLOOD)
                          ? 10000
                          : m_pcapConfig.batchSize;

    while (totalProcessed < stopLimit) {
        // step() returns false if EOF or if we are waiting for time
        if (!step()) {
            return;
        }
        totalProcessed++;
    }

    // Yield to event loop if we are just flooding (avoid freezing the app)
    if (m_pcapConfig.mode == ReplayMode::FLOOD && !m_finished) {
        m_loop.runAfter(Duration(0), [this]() {
            this->processBatch();
        });
    }
    // Note: In TIMED mode, step() handles the rescheduling when it hits a future packet.
    // If the batch finished but next packet is valid (catch-up scenario), schedule immediate continuation.
    else if (m_pcapConfig.mode == ReplayMode::TIMED && !m_finished) {
         m_loop.runAfter(Duration(0), [this]() {
            this->processBatch();
        });
    }
}

std::chrono::steady_clock::time_point PcapReceiver::calculateTargetTimeHighRes(
        const struct timespec& ts)
{
    if (m_firstPacket) {
        m_pcapStartTs = ts;
        m_wallStartTs = std::chrono::steady_clock::now();
        m_firstPacket = false;
        return m_wallStartTs;
    }

    long diff_sec = ts.tv_sec  - m_pcapStartTs.tv_sec;
    long diff_ns  = ts.tv_nsec - m_pcapStartTs.tv_nsec;

    // Normalize nanoseconds if negative (e.g., ts.tv_nsec < start.tv_nsec)
    if (diff_ns < 0) {
        diff_sec -= 1;
        diff_ns += 1000000000L;
    }

    if (m_pcapConfig.speedMultiplier != 1.0) {
        double total_ns = (double)diff_sec * 1e9 + (double)diff_ns;
        total_ns /= m_pcapConfig.speedMultiplier;
        diff_sec = (long)(total_ns / 1e9);
        diff_ns = (long)((long long)total_ns % 1000000000L);
    }

    return m_wallStartTs + std::chrono::seconds(diff_sec) + std::chrono::nanoseconds(diff_ns);
}

void PcapReceiver::parseAndDispatch(
        const struct timespec& ts,
        uint32_t caplen,
        uint32_t len,
        const uint8_t* packet)
{
    if (caplen != len) return; // Ignore truncated in capture

    const uint8_t* ptr = packet;
    uint32_t remaining = caplen;
    uint16_t proto = 0;

    // --- Layer 2 ---
    if (m_linkType == DLT_LINUX_SLL) {
        if (remaining < 16) return;
        // Protocol is at offset 14 (big endian)
        proto = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 14));
        ptr += 16;
        remaining -= 16;
    }
    else if (m_linkType == DLT_EN10MB) { // Standard Ethernet
        // --- Layer 2: Ethernet ---
        if (remaining < sizeof(struct ether_header)) return;
        auto* eth = reinterpret_cast<const struct ether_header*>(ptr);
        proto = ntohs(eth->ether_type);
        ptr += sizeof(struct ether_header);
        remaining -= static_cast<uint32_t>(sizeof(struct ether_header));

        // Handle 802.1Q VLAN Tagging
        if (proto == ETHERTYPE_VLAN) {
            if (remaining < 4) return; // VLAN tag size
            // Skip VLAN (simplified, assuming single tag)
            proto = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 2));
            ptr += 4;
            remaining -= 4;
        }
    }
    else {
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

    uint16_t dstPort = ntohs(udp->uh_dport);
    uint16_t udpLen = ntohs(udp->uh_ulen); // Includes header
    if (udpLen < sizeof(struct udphdr)) [[unlikely]] return;

    size_t dataLen = udpLen - sizeof(struct udphdr);

    ptr += sizeof(struct udphdr);
    remaining -= static_cast<uint32_t>(sizeof(struct udphdr));

    if (remaining < dataLen) [[unlikely]] return;

    // --- Dispatch ---
    auto& sub = m_portTable[dstPort];
    if (sub.handler) {
        sub.handler(
                sub.context,
                const_cast<uint8_t*>(ptr),
                dataLen,
                PacketStatus::OK,
                ts);
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
