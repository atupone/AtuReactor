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

// System headers
#include <map>
#include <sys/socket.h>
#include <thread>
#include <vector>

// Library headers
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/PacketMetadata.h>
#include <atu_reactor/Result.h>
#include <atu_reactor/ScopedFd.h>
#include <atu_reactor/Types.h>

namespace atu_reactor {

/**
 * @brief Configuration for PacketReceiver performance tuning.
 */
struct ReceiverConfig {
    int maxFds = 128;         // Limit to prevent FD exhaustion
    int batchSize = 64;       // Number of packets to pull via recvmmsg
    int bufferSize = 2048;    // Sufficient for standard MTU + headers
};

/**
 * @class PacketReceiver
 * @brief Abstract base managing shared memory and thread-hostile state.
 */
class PacketReceiver {
    // Grant EventLoop access to private members like handleRead
    friend class EventLoop;

    public:
        /**
         * @brief Constructor
         * @param loop Reference to the external event loop.
         * @param config Optional tuning parameters for buffers and limits.
         */
        explicit PacketReceiver(EventLoop& loop, ReceiverConfig config = {});

        // Destructor ensures FDs are removed from the EventLoop before closing
        virtual ~PacketReceiver();

        /**
         * @brief Registration logic common to all implementations.
         */
        [[nodiscard]] virtual Result<int> subscribe(uint16_t localPort, void* context, PacketHandlerFn handler);

        /**
         * @brief Removes the port from the loop and closes the associated socket.
         * @param Logic is identical for UDP, TCP, or Pcap, so it lives here.
         */
        virtual Result<void> unsubscribe(uint16_t localPort);

        // Disable copy/move to strictly manage resource identity
        PacketReceiver(const PacketReceiver&) = delete;
        PacketReceiver& operator=(const PacketReceiver&) = delete;
        PacketReceiver(PacketReceiver&&) = delete;
        PacketReceiver& operator=(PacketReceiver&&) = delete;

    protected:
        /**
         * @brief Enforces the thread-hostile requirement.
         */
        void checkThread() const;

        /**
         * @brief Internal callback triggered by EventLoop when a socket has data.
         */
        virtual void handleRead(int fd, void* context, PacketHandlerFn handler) = 0;

        /**
         * @brief Shared dispatcher to call user handlers with processed data.
         */
        void dispatch(int n, const PacketMetadata* meta, PacketHandlerFn handler, void* context);

        EventLoop& m_loop;
        ReceiverConfig m_config;
        std::thread::id m_ownerThreadId; // Added for thread-safety asserts

        // Maps port -> ScopedFd. RAII ensures sockets close on removal.
        std::map<uint16_t, ScopedFd> m_port_to_fd_map;

        /**
         * Memory structures for recvmmsg.
         * Pre-allocated based on m_config to avoid heap allocation during the hot path.
         */
        size_t m_alignedBufferSize; // Added for cache-line alignment
        uint8_t* m_hugeBuffer = nullptr; // The pointer returned by mmap
        size_t m_mappedSize = 0;      // To store the total size for munmap
        uint8_t* m_cachedBasePtr = nullptr;

        std::vector<struct iovec> m_ioVectors;             // Points to packetBuffers
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
