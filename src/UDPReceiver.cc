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
#include <atu_reactor/UDPReceiver.h>

// System headers
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>

namespace atu_reactor {

UDPReceiver::UDPReceiver(EventLoop& loopRef, ReceiverConfig config)
        : m_loop(loopRef),
        m_config(config),
        // Allocate ONE big chunk of memory for all packets
        m_flatBuffer(config.batchSize * config.bufferSize),
        m_ioVectors(config.batchSize),
        m_msgHeaders(config.batchSize)
{
    // Initialize the mapping between kernel structures and our flat buffer.
    for (int i = 0; i < m_config.batchSize; ++i) {
        // Calculate offset into the flat buffer
        uint8_t* currentBufferPtr = &m_flatBuffer[i * m_config.bufferSize];

        // Map iovec to the specific row in our 2D vector
        m_ioVectors[i].iov_base = currentBufferPtr;
        m_ioVectors[i].iov_len = m_config.bufferSize;

        // Link the message header to the iovec
        memset(&m_msgHeaders[i], 0, sizeof(struct mmsghdr));
        m_msgHeaders[i].msg_hdr.msg_iov = &m_ioVectors[i];
        m_msgHeaders[i].msg_hdr.msg_iovlen = 1;
    }
}

UDPReceiver::~UDPReceiver() {
    // Safety: Explicitly remove all sources from EventLoop so it doesn't
    // try to call callbacks on this destroyed object.
    for (auto const& [port, fd] : m_port_to_fd_map) {
        m_loop.removeSource(fd);
    }
}

Result<int> UDPReceiver::subscribe(uint16_t localPort, IPacketHandler* handler) {
    if (handler == nullptr) {
        return {std::error_code(EINVAL, std::system_category())};
    }

    if (m_port_to_fd_map.count(localPort)) {
        return {std::error_code(EADDRINUSE, std::system_category())};
    }

    if (m_port_to_fd_map.size() >= static_cast<size_t>(m_config.maxFds)) {
        return {std::error_code(EMFILE, std::system_category())};
    }

    // Initialize IPv4 UDP socket
    ScopedFd udp_socket(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (udp_socket < 0) {
        return {std::error_code(errno, std::system_category())};
    }

    // Reuse Address: Allows immediate restart of the application
    int optval = 1;
    if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        return {std::error_code(errno, std::system_category())};
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    addr.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces

    if (::bind(udp_socket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        return {std::error_code(errno, std::system_category())};
    }

    int raw_fd = udp_socket;
    try {
        m_loop.addSource(raw_fd, EPOLLIN, [this, raw_fd, handler](uint32_t) {
            this->handleRead(raw_fd, handler);
        });
    } catch (const std::exception& e) {
        return {std::error_code(ECONNREFUSED, std::system_category())};
    }

    // Move ownership of ScopedFd to our map
    m_port_to_fd_map.emplace(localPort, std::move(udp_socket));

    return {raw_fd};
}

void UDPReceiver::unsubscribe(int localPort) {
    auto it = m_port_to_fd_map.find(localPort);
    if (it != m_port_to_fd_map.end()) {
        // Remove from epoll first to stop callbacks
        m_loop.removeSource(it->second);

        // Erasing from map triggers ScopedFd destructor, closing the socket
        m_port_to_fd_map.erase(it);
    }
}

void UDPReceiver::handleRead(int fd, IPacketHandler* handler) {
    // recvmmsg allows us to grab up to BATCH_SIZE packets in one go.
    // MSG_DONTWAIT ensures we don't block if the buffer was emptied by a race condition.
    int numPackets = recvmmsg(
            fd, m_msgHeaders.data(), m_config.batchSize,
            MSG_DONTWAIT, nullptr);
    if (numPackets < 0) return;

    // Iterate through only the number of packets actually received
    for (int k = 0; k < numPackets; ++k) {
        size_t len = m_msgHeaders[k].msg_len;
        if (len > 0) {
            // Data is at the specific offset in the flat buffer
            uint8_t* packetData = &m_flatBuffer[k * m_config.bufferSize];
            // Dispatch the packet to the user-defined handler
            handler->handlePacket(packetData, len);
        }
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
