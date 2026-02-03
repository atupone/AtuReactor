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
#include <netinet/in.h>
#include <vector>
#include <sys/socket.h>

// Library headers
#include <atu_reactor/Export.h>

namespace atu_reactor {

/**
 * @class UDPReceiver
 * @brief Manages multiple UDP sockets using an EventLoop.
 * * @note This class is THREAD-HOSTILE. It uses internal shared buffers
 * (m_flatBuffer) for high-performance batch reading. All methods,
 * including subscription and the eventual EventLoop::runOnce() dispatch,
 * MUST be executed on the same thread.
 */
class ATU_API UDPReceiver : public PacketReceiver {
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
        ~UDPReceiver() override = default;

        /**
         * @brief Creates a UDP socket, binds it to localPort, and registers it with the EventLoop.
         * @param localPort Port to listen on.
         * @param handler Callback object for processed packets.
         * @return true if socket was successfully created and registered.
         */
        [[nodiscard]] Result<int> subscribe(uint16_t localPort, void* context, PacketHandlerFn handler);

        // Disable copy/move to strictly manage resource identity
        UDPReceiver(const UDPReceiver&) = delete;
        UDPReceiver& operator=(const UDPReceiver&) = delete;
        UDPReceiver(UDPReceiver&&) = delete;
        UDPReceiver& operator=(UDPReceiver&&) = delete;

    protected:
        /**
         * @brief Internal callback triggered by EventLoop when a socket has data.
         */
        void handleRead(int fd, void* context, PacketHandlerFn handler);

    private:
        /**
         * Memory structures for recvmmsg.
         * Pre-allocated based on m_config to avoid heap allocation during the hot path.
         */
        std::vector<struct mmsghdr> m_msgHeaders;          // Kernel-to-user metadata

        // Store source addresses for the entire batch to avoid stack allocation in handleRead
        std::vector<struct sockaddr_storage> m_senderAddrs;

        // Add a member to hold control buffers for the batch
        std::vector<std::array<uint8_t, CMSG_SPACE(sizeof(struct timespec))>> m_controlBuffers;
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
