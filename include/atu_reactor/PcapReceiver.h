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
#include <pcap/pcap.h>
#include <string>
#include <unordered_map>

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

/**
 * @class PcapReceiver
 */
class PcapReceiver : public PacketReceiver {
    public:
        /**
         * @brief Constructor
         * @param loop Reference to the external event loop.
         * @param config Optional tuning parameters for buffers and limits.
         */
        explicit PcapReceiver(EventLoop& loop, PcapConfig config = {});

        // Destructor ensures FDs are removed from the EventLoop before closing
        ~PcapReceiver() override;

        /**
         * @brief Opens a PCAP file for replay.
         * @param path Path to the .pcap file.
         */
        [[nodiscard]] Result<void> open(const std::string& path);

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
         * @brief Manually triggers the next packet (Only for STEP mode).
         */
        void step();

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
        void scheduleNext();
        void processBatch();
        void parseAndDispatch(const struct pcap_pkthdr* header, const uint8_t* packet);

        PcapConfig m_pcapConfig;
        pcap_t* m_pcapHandle = nullptr;
        char m_errbuf[PCAP_ERRBUF_SIZE];

        // Subscriptions: Map Port -> Handler info
        struct Subscription {
            void* context;
            PacketHandlerFn handler;
        };
        std::unordered_map<uint16_t, Subscription> m_subscriptions;

        // Timing state
        struct timespec m_pcapStartTs = {0, 0}; // TS of first packet in file
        std::chrono::steady_clock::time_point m_wallStartTs; // Wall time when replay started
        bool m_firstPacket = true;

        uint32_t m_currentBatchIdx = 0; // Tracks which buffer slot to use
        int m_linkType = -1; // Added to store DLT from pcap_datalink()

        // Dedicated buffer for the "next" scheduled packet to avoid heap allocs
        std::vector<uint8_t> m_pendingPacketBuf;
        struct pcap_pkthdr m_pendingHeader;
        bool m_hasPending = false;
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
