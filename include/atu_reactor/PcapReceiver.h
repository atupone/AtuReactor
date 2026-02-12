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

#pragma once

// Inherits from
#include <atu_reactor/PacketReceiver.h>

// System headers
#include <string>
#include <unordered_map>
#include <sys/types.h>

// Library headers
#include <atu_reactor/Export.h>

namespace atu_reactor {

enum class ReplayMode {
    TIMED,      // Respect PCAP timestamps relative to wall clock
    FLOOD,      // Replay as fast as CPU allows (in batches)
    STEP        // Wait for manual trigger (step())
};

struct PcapConfig : public ReceiverConfig {
    ReplayMode mode = ReplayMode::TIMED;
    double speedMultiplier = 1.0; // 1.0 = normal speed, 2.0 = 2x speed
};

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

enum PcapMagic : uint32_t {
    // Legacy PCAP
    MAGIC_MICRO_BE = 0xa1b2c3d4,
    MAGIC_MICRO_LE = 0xd4c3b2a1,
    MAGIC_NANO_BE  = 0xa1b23c4d,
    MAGIC_NANO_LE  = 0x4d3c2b1a,

    // PCAPNG
    MAGIC_PCAPNG_SHB = 0x0A0D0D0A,
    PCAPNG_BOM       = 0x1A2B3C4D,
    PCAPNG_BOM_SWAP  = 0x4D3C2B1A,
    PCAPNG_IDB       = 0x00000001,
    PCAPNG_EPB       = 0x00000006
};

// PCAP Packet Header (Disk Format)
struct pcap_sf_pkthdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t caplen;
    uint32_t len;
};

struct PcapNgBlockHeader {
    uint32_t type;
    uint32_t totalLength;
};

struct PcapNgIDBBody {
    uint16_t linkType;
    uint16_t reserved;
    uint32_t snapLen;
};

// Body of an Enhanced Packet Block (Type 6)
struct PcapNgEPBBody {
    uint32_t interfaceId;
    uint32_t timestampHigh;
    uint32_t timestampLow;
    uint32_t capLen;
    uint32_t origLen;
    // Packet Data follows immediately...
    // Padding to 32-bit boundary...
    // Total Length (repeated)
};

/**
 * @class PcapReceiver
 */
class ATU_API PcapReceiver : public PacketReceiver {
    public:
        /**
         * @brief Constructor
         * @param loop Reference to the external event loop.
         * @param config Optional tuning parameters for buffers and limits.
         */
        explicit PcapReceiver(EventLoop& loop, PcapConfig config = {});

        // Destructor ensures munmap and close are called
        ~PcapReceiver() override;

        /**
         * @brief Opens a PCAP file for replay.
         * @param path Path to the .pcap file.
         */
        [[nodiscard]] Result<void> open(const std::string& path);

        void rewind();

        /**
         * @brief "Subscribes" to a specific destination port found in the PCAP.
         * When a packet in the PCAP matches this destination port, the handler is called.
         */
        [[nodiscard]] Result<int> subscribe(uint16_t localPort, void* context, PacketHandlerFn handler);

        [[nodiscard]] Result<void> unsubscribe(uint16_t port) override;

        /**
         * @brief Starts the replay loop (for TIMED and FLOOD modes).
         */
        void start();

        /**
         * @brief Manually triggers the next packet processing.
         * @return true if a packet was processed, false if EOF or waiting (TIMED).
         */
        bool step();

        bool isFinished() const { return m_finished; }

        // Disable copy/move to strictly manage resource identity
        PcapReceiver(const PcapReceiver&) = delete;
        PcapReceiver& operator=(const PcapReceiver&) = delete;
        PcapReceiver(PcapReceiver&&) = delete;
        PcapReceiver& operator=(PcapReceiver&&) = delete;

    protected:
        /**
         * @brief Internal callback triggered by EventLoop when a socket has data.
         */
        void handleRead(int, void*, PacketHandlerFn) override {}

    private:
        void processBatch();
        void processBatchFlood();
        inline bool internalStep() noexcept;
        inline void parseAndDispatch(
                const struct timespec & header,
                uint32_t caplen,
                uint32_t len,
                const uint8_t* packet,
                uint32_t linkType);
        void slowPathParse(const struct timespec& ts, uint32_t caplen, uint32_t len,
                const uint8_t* packet, uint32_t linkType);

        // Helper to determine when a packet should be played in TIMED mode
        std::chrono::steady_clock::time_point calculateTargetTimeHighRes(const struct timespec& header);

        // Helper to read NG blocks
        inline bool stepPcapNg() noexcept;

        struct InterfaceInfo {
            uint16_t linkType;
            uint64_t tsResolutionDivisor;
        };

        PcapConfig m_pcapConfig;

        // --- MMAP State ---
        int m_fd = -1;
        uint8_t* m_mappedData = nullptr;
        size_t m_fileSize = 0;
        const uint8_t* m_currentPtr = nullptr;
        uint32_t m_linkType = 0;

        // Subscriptions: Map Port -> Handler info
        struct Subscription {
            void* context = nullptr;
            PacketHandlerFn handler = nullptr;
        };
        std::unique_ptr<Subscription[]> m_portTable;

        // Timing state
        struct timespec m_pcapStartTs = {0, 0}; // TS of first packet in file
        std::chrono::steady_clock::time_point m_wallStartTs; // Wall time when replay started
        bool m_firstPacket = true;

        bool m_finished = false;

        bool m_swapped = false;
        bool m_isNanosecond = false;

        // Maps Interface ID (index in file) to its metadata
        std::unordered_map<uint32_t, InterfaceInfo> m_interfaces;
        uint32_t m_interfaceCount = 0;

        bool m_isPcapNg = true;

        uint16_t m_hotPort = 0; // In Network Byte Order
        PacketHandlerFn m_hotHandler = nullptr;
        void* m_hotContext = nullptr;
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
