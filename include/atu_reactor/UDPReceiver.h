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
#include <vector>
#include <map>
#include <sys/socket.h>

// Library headers
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/Result.h>
#include <atu_reactor/ScopedFd.h>

using PacketHandlerFn = void(*)(void* context, const uint8_t* data, size_t len);

namespace atu_reactor {

/**
 * @brief Configuration for UDPReceiver performance tuning.
 */
struct ReceiverConfig {
    int maxFds = 128;         // Limit to prevent FD exhaustion
    int batchSize = 64;       // Number of packets to pull via recvmmsg
    int bufferSize = 2048;    // Sufficient for standard MTU + headers
};

/**
 * @class UDPReceiver
 * @brief Manages multiple UDP sockets using an EventLoop for asynchronous reception.
 */
class UDPReceiver {
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
        void unsubscribe(int localPort);

        // Disable copy/move to strictly manage resource identity
        UDPReceiver(const UDPReceiver&) = delete;
        UDPReceiver& operator=(const UDPReceiver&) = delete;
        UDPReceiver(UDPReceiver&&) = delete;
        UDPReceiver& operator=(UDPReceiver&&) = delete;

    private:
        struct PortContext {
            UDPReceiver* parent;
            int fd;              // The raw FD for this port
            void* userContext;   // The 'this' pointer from the app
            PacketHandlerFn handler; // The static callback function
        };

        // Static bridge for the EventLoop
        static void onFdReady(void* ctx, uint32_t events);

        /**
         * @brief Internal callback triggered by EventLoop when a socket has data.
         */
        void handleRead(int fd, void* context, PacketHandlerFn handler);

        EventLoop& m_loop;
        ReceiverConfig m_config;

        // Maps port -> ScopedFd. RAII ensures sockets close on removal.
        std::map<int, ScopedFd> m_port_to_fd_map;
        // Storage for PortContext to ensure pointers passed to EventLoop remain valid
        std::map<int, PortContext> m_contexts;

        // Single contiguous buffer for all packets to improve cache locality
        std::vector<uint8_t> m_flatBuffer;

        /**
         * Memory structures for recvmmsg.
         * Pre-allocated based on m_config to avoid heap allocation during the hot path.
         */
        std::vector<struct iovec> m_ioVectors;             // Points to packetBuffers
        std::vector<struct mmsghdr> m_msgHeaders;          // Kernel-to-user metadata
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
