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
#include <atu_reactor/TcpMessaging.h>

// System headers
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace atu_reactor {

    TcpMessaging::TcpMessaging(EventLoop& loop) : m_loop(loop) {}

    TcpMessaging::~TcpMessaging() {
        close();
    }

    void TcpMessaging::connect(const std::string& ip, uint16_t port, DataCallback onData) {
        m_onData = std::move(onData);

        struct addrinfo hints{}, *resInfo;
        hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &resInfo) != 0) {
            return;
        }

        // Create socket based on what getaddrinfo found (AF_INET or AF_INET6)
        m_fd = socket(resInfo->ai_family, resInfo->ai_socktype, resInfo->ai_protocol);
        if (m_fd < 0) {
            freeaddrinfo(resInfo);
            return;
        }

        // Set non-blocking so the EventLoop never stalls
        fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL, 0) | O_NONBLOCK);

        // Start Async Connect
        int res = ::connect(m_fd, resInfo->ai_addr, resInfo->ai_addrlen);
        freeaddrinfo(resInfo);

        if ((res != 0) && (errno != EINPROGRESS)) {
            close();
            return;
        }

        m_loop.addStreamSource(m_fd, &TcpMessaging::onEvent, this);

        if (res == 0) {
            m_connected = true;
        } else if (errno == EINPROGRESS) {
            // Handshake in progress: MUST listen for EPOLLOUT to know when it finishes
            m_loop.modifyStreamSource(m_fd, EPOLLOUT);
        }
    }

    void TcpMessaging::onEvent(uint32_t events, void* context) {
        auto* self = static_cast<TcpMessaging*>(context);

        if (events & EPOLLOUT) {
            if (!self->m_connected) {
                self->handleConnect();
            } else {
                self->handleWrite();
            }
        }

        if (events & EPOLLIN) {
            self->handleRead();
        }
    }

    void TcpMessaging::handleConnect() {
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &len);

        if (error == 0) {
            m_connected = true;
            // Connection success!
        } else {
            close(); // Connection failed
        }
    }

    void TcpMessaging::handleRead() {
        uint8_t buffer[4096];
        ssize_t n = ::read(m_fd, buffer, sizeof(buffer));

        if (n > 0) {
            if (m_onData) m_onData(buffer, static_cast<size_t>(n));
        } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
            close(); // Connection lost or error
        }
    }

    void TcpMessaging::send(std::string_view data) {
        if (m_fd < 0) {
            return; // Socket must at least exist
        }

        // If we already have a buffer, append to it to preserve order
        if (!m_connected || !m_writeBuffer.empty()) {
            m_writeBuffer.insert(m_writeBuffer.end(), data.begin(), data.end());

            // Ensure we are registered for EPOLLOUT so we know when
            // the connection finishes or the buffer can be drained
            m_loop.modifyStreamSource(m_fd, EPOLLOUT);
            return;
        }

        // Try direct write only if connected and buffer is empty
        ssize_t n = ::write(m_fd, data.data(), data.length());

        if (n < (ssize_t)data.length()) {
            size_t written = (n < 0) ? 0 : (size_t)n;

            if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(); // Error occurred, shut down the connection
                return;
            }

            // Socket is full! Store the remainder and ask for EPOLLOUT
            m_writeBuffer.insert(m_writeBuffer.end(), data.begin() + written, data.end());
            m_loop.modifyStreamSource(m_fd, EPOLLOUT);
        }
    }

    void TcpMessaging::handleWrite() {
        if (m_writeBuffer.empty()) {
            return;
        }

        ssize_t n = ::write(m_fd, m_writeBuffer.data(), m_writeBuffer.size());

        if (n > 0) {
            m_writeBuffer.erase(m_writeBuffer.begin(), m_writeBuffer.begin() + n);
        }

        // If buffer is finally empty, stop listening for EPOLLOUT
        if (m_writeBuffer.empty()) {
            m_loop.modifyStreamSource(m_fd, 0); // Back to just EPOLLIN
        }
    }

    void TcpMessaging::close() {
        if (m_fd >= 0) {
            m_loop.removeStreamSource(m_fd);
            ::close(m_fd);
            m_fd = -1;
        }
        m_connected = false;
    }


} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
