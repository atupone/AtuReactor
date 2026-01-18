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
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <iostream>

// Fallback for non-standard Linux headers
#ifndef ETHERTYPE_VLAN
#define ETHERTYPE_VLAN 0x8100
#endif

namespace atu_reactor {

PcapReceiver::PcapReceiver(EventLoop& loopRef, PcapConfig config)
        : PacketReceiver(loopRef, config), m_pcapConfig(config)
{
}

PcapReceiver::~PcapReceiver() {
    if (m_pcapHandle) {
        pcap_close(m_pcapHandle);
    }
}

Result<void> PcapReceiver::open(const std::string& path) {
    checkThread();
    m_pcapHandle = pcap_open_offline(path.c_str(), m_errbuf);
    if (!m_pcapHandle) {
        // We simulate an IO error if pcap fails
        return std::error_code(EIO, std::system_category());
    }
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
    // We store the handler in our map so the parser can find it.
    m_subscriptions[port] = {context, handler};

    // Return the port as the ID.
    // This allows the caller to treat the port as the 'handle' for this subscription.
    return static_cast<int>(port);
}

Result<void> PcapReceiver::unsubscribe(uint16_t port) {
    auto baseResult = PacketReceiver::unsubscribe(port);
    if (!baseResult) {
        return baseResult;
    }

    if (m_subscriptions.erase(port) == 0) {
        return std::error_code(ENOENT, std::system_category());
    }
    return Result<void>::success();
}

void PcapReceiver::start() {
    checkThread();
    if (!m_pcapHandle) {
        return;
    }

    m_firstPacket = true;

    // In STEP mode, we do nothing. User must call step().
    if (m_pcapConfig.mode == ReplayMode::STEP) {
        return;
    }

    // For TIMED or FLOOD, schedule the first batch immediately
    scheduleNext();
}

void PcapReceiver::step() {
    checkThread();
    if (m_pcapConfig.mode != ReplayMode::STEP) {
        return;
    }

    // Process exactly one packet
    struct pcap_pkthdr* header;
    const uint8_t* packetData;
    int res = pcap_next_ex(m_pcapHandle, &header, &packetData);

    if (res == 1) {
        parseAndDispatch(header, packetData);
    }
}

void PcapReceiver::scheduleNext() {
    // We yield to the reactor loop to process other IO/Timers
    m_loop.runAfter(std::chrono::milliseconds(0), [this]() {
        processBatch();
    });
}

void PcapReceiver::processBatch() {
    if (!m_pcapHandle) {
        return;
    }

    struct pcap_pkthdr* header;
    const uint8_t* packetData;
    int count = 0;

    // In FLOOD mode, we process a full batch (e.g., 64 packets) then yield.
    // In TIMED mode, we process packets until we hit a packet that is "in the future".

    while (true) {
        // Check batch limit (prevent starving other IO)
        if (count >= m_config.batchSize) {
            scheduleNext(); // Continue in next loop cycle
            return;
        }

        // Read Packet
        int res = pcap_next_ex(m_pcapHandle, &header, &packetData);
        if (res == -2) {
            /* EOF */
            return;
        }
        if (res != 1) {
            continue; // Error or timeout, retry
        }

        // Timing Logic (TIMED Mode only)
        if (m_pcapConfig.mode == ReplayMode::TIMED) {
            if (m_firstPacket) {
                m_pcapStartTs.tv_sec = header->ts.tv_sec;
                m_pcapStartTs.tv_nsec = static_cast<long>(header->ts.tv_usec) * 1000;
                m_wallStartTs = std::chrono::steady_clock::now();
                m_firstPacket = false;
            } else {
                // Calculate Offset from start of PCAP
                long diff_sec = header->ts.tv_sec - m_pcapStartTs.tv_sec;
                long diff_ns = (static_cast<long>(header->ts.tv_usec) * 1000)
                    - m_pcapStartTs.tv_nsec;

                // Adjust for speed multiplier
                if (m_pcapConfig.speedMultiplier != 1.0) {
                    double total_ns = (double)diff_sec * 1e9 + (double)diff_ns;
                    total_ns /= m_pcapConfig.speedMultiplier;
                    diff_sec = (long)(total_ns / 1e9);
                    diff_ns = (long)((long long)total_ns % 1000000000L);
                }

                auto targetTime = m_wallStartTs
                    + std::chrono::seconds(diff_sec)
                    + std::chrono::nanoseconds(diff_ns);
                auto now = std::chrono::steady_clock::now();

                if (targetTime > now) {
                    // This packet is in the future.
                    // We cannot process it yet. We must sleep (schedule timer) and retry.
                    // Note: pcap_next_ex ALREADY advanced the pointer. This is tricky.
                    // We must process THIS packet when the timer fires.
                    // Since we can't "put back" the packet easily, we execute the dispatch logic
                    // inside a delayed lambda.

                    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(targetTime - now);

                    // Capture the necessary data for THIS packet to run later
                    // We must copy the data because pcap internal buffer might change?
                    // Actually, pcap ring buffer is usually valid until next call, but let's be safe.
                    std::vector<uint8_t> safeBuf(packetData, packetData + header->caplen);
                    struct pcap_pkthdr safeHdr = *header;

                    m_loop.runAfter(delay, [this, safeBuf = std::move(safeBuf), safeHdr]() {
                        // Dispatch the delayed packet
                        parseAndDispatch(&safeHdr, safeBuf.data());
                        // Resume normal processing loop
                        processBatch();
                    });
                    return; // Return to event loop, waiting for timer
                }
            }
        }

        // Dispatch
        parseAndDispatch(header, packetData);
        count++;
    }
}

void PcapReceiver::parseAndDispatch(const struct pcap_pkthdr* header, const uint8_t* packet) {
#if 0
    if (header->caplen != header->len) {
        return; // Ignore truncated in capture
    }
#endif

    const uint8_t* ptr = packet;
    uint32_t remaining = header->caplen;

    // --- Layer 2: Ethernet ---
    if (remaining < sizeof(struct ether_header)) return;
    auto* eth = reinterpret_cast<const struct ether_header*>(ptr);
    uint16_t proto = ntohs(eth->ether_type);
    ptr += sizeof(struct ether_header);
    remaining -= static_cast<uint32_t>(sizeof(struct ether_header));

    if (proto == ETHERTYPE_VLAN) {
        if (remaining < 4) return; // VLAN tag size
        // Skip VLAN (simplified, assuming single tag)
        proto = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 2));
        ptr += 4;
        remaining -= 4;
    }

    if (proto != ETHERTYPE_IP) return;

    // --- Layer 3: IPv4 ---
    if (remaining < sizeof(struct ip)) return;
    auto* ip = reinterpret_cast<const struct ip*>(ptr);
    if (ip->ip_v != 4) return;

    uint32_t ipLen = ip->ip_hl * 4;
    if (remaining < ipLen) return;

    if (ip->ip_p != IPPROTO_UDP) return;

    ptr += ipLen;
    remaining -= ipLen;

    // --- Layer 4: UDP ---
    if (remaining < sizeof(struct udphdr)) return;
    auto* udp = reinterpret_cast<const struct udphdr*>(ptr);

    uint16_t dstPort = ntohs(udp->uh_dport);
    uint16_t udpLen = ntohs(udp->uh_ulen); // Includes header
    if (udpLen < sizeof(struct udphdr)) return;

    size_t dataLen = udpLen - sizeof(struct udphdr);

    ptr += sizeof(struct udphdr);
    remaining -= static_cast<uint32_t>(sizeof(struct udphdr));

    if (remaining < dataLen) return;

    // --- Dispatch using m_hugeBuffer ---
    // Check if anyone subscribed to this port
    auto it = m_subscriptions.find(dstPort);
    if (it != m_subscriptions.end()) {
        // Determine destination in our pre-allocated hugepage memory
        // We use m_currentBatchIdx to simulate the same buffer layout as UDPReceiver
        uint8_t* destBuf = m_cachedBasePtr + (m_currentBatchIdx * m_alignedBufferSize);

        // Safety check: Ensure the packet fits in the allocated buffer slot
        size_t copyLen = std::min(dataLen, (size_t)m_config.bufferSize);

        // Perform the copy (This provides the user with a mutable pointer)
        std::memcpy(destBuf, ptr, copyLen);

        struct timespec ts;
        ts.tv_sec = header->ts.tv_sec;
        ts.tv_nsec = header->ts.tv_usec * 1000;

        // Dispatch using the managed buffer
        // Call the user handler directly
        // Note: PacketStatus is OK because we dropped truncated frames earlier
        it->second.handler(it->second.context, destBuf, copyLen, PacketStatus::OK, ts);

        // Increment and wrap the buffer index
        m_currentBatchIdx = (m_currentBatchIdx + 1) % m_config.batchSize;
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
