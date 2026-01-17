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
#include <atu_reactor/Result.h>
#include <atu_reactor/ScopedFd.h>
#include <atu_reactor/Types.h>

namespace atu_reactor {

/**
 * @brief Configuration for UDPReceiver performance tuning.
 */
struct ReceiverConfig {
    int maxFds = 128;         // Limit to prevent FD exhaustion
    int batchSize = 64;       // Number of packets to pull via recvmmsg
    int bufferSize = 2048;    // Sufficient for standard MTU + headers
};

// Define the function pointer type here


/**
 * @class UDPReceiver
 * @brief Manages multiple UDP sockets using an EventLoop.
 * * @note This class is THREAD-HOSTILE. It uses internal shared buffers
 * (m_flatBuffer) for high-performance batch reading. All methods,
 * including subscription and the eventual EventLoop::runOnce() dispatch,
 * MUST be executed on the same thread.
 */
class UDPReceiver {
    // Grant EventLoop access to private members like handleRead
    friend class EventLoop;

    public:
        /**
         * @brief Constructor
         * @param loop Reference to the external event loop.
         * @param config Optional tuning parameters for buffers and limits.
         */
        explicit UDPReceiver(EventLoop& loop, ReceiverConfig config = {});

        // Destructor ensures FDs are removed from the EventLoop before closing
        ~UDPReceiver();

        /**
         * @brief Creates a UDP socket, binds it to localPort, and registers it with the EventLoop.
         * @param localPort Port to listen on.
         * @param handler Callback object for processed packets.
         * @return true if socket was successfully created and registered.
         */
        [[nodiscard]] Result<int> subscribe(uint16_t localPort, void* context, PacketHandlerFn handler);

        /**
         * @brief Closes the socket for a port and removes it from the EventLoop.
         * @param localPort The port to stop listening on.
         */
        Result<void> unsubscribe(uint16_t localPort);

        // Disable copy/move to strictly manage resource identity
        UDPReceiver(const UDPReceiver&) = delete;
        UDPReceiver& operator=(const UDPReceiver&) = delete;
        UDPReceiver(UDPReceiver&&) = delete;
        UDPReceiver& operator=(UDPReceiver&&) = delete;

    private:
        /**
         * @brief Internal callback triggered by EventLoop when a socket has data.
         */
        void handleRead(int fd, void* context, PacketHandlerFn handler);

        EventLoop& m_loop;
        ReceiverConfig m_config;
        size_t m_alignedBufferSize; // Added for cache-line alignment
        std::thread::id m_ownerThreadId; // Added for thread-safety asserts

        // Maps port -> ScopedFd. RAII ensures sockets close on removal.
        std::map<int, ScopedFd> m_port_to_fd_map;

        /**
         * Memory structures for recvmmsg.
         * Pre-allocated based on m_config to avoid heap allocation during the hot path.
         */
        std::vector<struct iovec> m_ioVectors;             // Points to packetBuffers
        std::vector<struct mmsghdr> m_msgHeaders;          // Kernel-to-user metadata

        // Store source addresses for the entire batch to avoid stack allocation in handleRead
        std::vector<struct sockaddr_storage> m_senderAddrs;

        uint8_t* m_cachedBasePtr = nullptr;

        uint8_t* m_hugeBuffer = nullptr; // The pointer returned by mmap
        size_t m_mappedSize = 0;      // To store the total size for munmap

        // Add a member to hold control buffers for the batch
        std::vector<std::array<uint8_t, CMSG_SPACE(3 * sizeof(struct timespec))>> m_controlBuffers;
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
