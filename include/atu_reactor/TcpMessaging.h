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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Library headers
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/Export.h>

namespace atu_reactor {

class ATU_API TcpMessaging {
    public:
        // Callback provides the raw bytes and the number of bytes received
        using DataCallback = std::function<void(std::string_view data)>;

        explicit TcpMessaging(EventLoop& loop);
        ~TcpMessaging();

        // Non-copyable, non-movable
        TcpMessaging(const TcpMessaging&) = delete;
        TcpMessaging& operator=(const TcpMessaging&) = delete;
        TcpMessaging(TcpMessaging&&) = delete;
        TcpMessaging& operator=(TcpMessaging&&) = delete;

        /**
         * @brief Connects to a server asynchronously.
         * @param ip Destination IP address.
         * @param port Destination Port.
         * @param onData Callback executed when data arrives.
         */
        void connect(const std::string& ip, uint16_t port, DataCallback onData);

        /**
         * @brief Sends a string/buffer over the established connection.
         */
        void send(std::string_view data);

        /**
         * @brief Closes the socket and removes it from the reactor loop.
         */
        void close();

        [[nodiscard]] bool isConnected() const noexcept { return m_connected; }

    private:
        // The static bridge that matches the EventLoop's StreamTag signature
        static void onEvent(uint32_t events, void* context);

        void handleRead();
        void handleWrite(); // logic to empty the buffer
        void handleConnect();
        void forceClose();

        EventLoop& m_loop;
        int m_fd{-1};
        DataCallback m_onData;
        std::vector<uint8_t> m_writeBuffer;

        bool m_connected{false};
        bool m_closed{true};
};

}  // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
