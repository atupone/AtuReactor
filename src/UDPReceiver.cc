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
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>

namespace atu_reactor {

UDPReceiver::UDPReceiver(EventLoop& loopRef, ReceiverConfig config)
        : m_loop(loopRef),
        m_config(config),
        m_ownerThreadId(std::this_thread::get_id()),
        // Allocate ONE big chunk of memory for all packets
        m_ioVectors(config.batchSize),
        m_msgHeaders(config.batchSize),
        m_senderAddrs(config.batchSize)
{
    m_alignedBufferSize = (m_config.bufferSize + 63) & ~63; // Round up to multiple of 64

    // 2. Resize the buffer
    // We add an extra 64 bytes of padding at the start to allow manual alignment
    // if the vector doesn't start on a 64-byte boundary.
    m_flatBuffer.resize((config.batchSize * m_alignedBufferSize) + 64);

    // 3. Find the first 64-byte aligned address in our vector
    uintptr_t rawAddr = reinterpret_cast<uintptr_t>(m_flatBuffer.data());
    uintptr_t alignedAddr = (rawAddr + 63) & ~63;
    uint8_t* basePtr = reinterpret_cast<uint8_t*>(alignedAddr);
    m_cachedBasePtr = reinterpret_cast<uint8_t*>(alignedAddr);

    // Initialize iovecs using the aligned stride
    for (int i = 0; i < m_config.batchSize; ++i) {
        // Calculate offset into the flat buffer
        uint8_t* packetStart = basePtr + (i * m_alignedBufferSize);

        // Map iovec to the specific row in our 2D vector
        m_ioVectors[i].iov_base = packetStart;
        m_ioVectors[i].iov_len = m_config.bufferSize;

        std::memset(&m_msgHeaders[i], 0, sizeof(struct mmsghdr));

        // Protocol-agnostic address setup: done ONCE here
        m_msgHeaders[i].msg_hdr.msg_name = &m_senderAddrs[i];

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

Result<int> UDPReceiver::subscribe(uint16_t port, void* context, PacketHandlerFn handler) {
    if (handler == nullptr) {
        return std::error_code(EINVAL, std::system_category());
    }

    // Use the OS limit check only if you explicitly want to cap this instance
    if (m_config.maxFds > 0 && m_port_to_fd_map.size() >= static_cast<size_t>(m_config.maxFds)) {
        return std::error_code(EMFILE, std::system_category());
    }

    if (m_port_to_fd_map.find(port) != m_port_to_fd_map.end()) {
        return std::error_code(EADDRINUSE, std::system_category());
    }

    // Attempt IPv6 Dual-Stack Socket
    int raw_fd = ::socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    bool isV6 = true;

    if (raw_fd < 0 && errno == EAFNOSUPPORT) {
        // Fallback to IPv4 if IPv6 is disabled in the kernel
        isV6 = false;
        raw_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    }

    if (raw_fd < 0) {
        // Here the OS will naturally return EMFILE if the real limit is hit
        return std::error_code(errno, std::system_category());
    }

    // Immediately wrap in your ScopedFd for RAII safety
    ScopedFd udp_socket(raw_fd);

    // Reuse Address: Allows immediate restart of the application
    int optval = 1;
    if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        return std::error_code(errno, std::system_category());
    }

    int reusePort = 1;
    if (setsockopt(udp_socket, SOL_SOCKET, SO_REUSEPORT, &reusePort, sizeof(reusePort)) < 0) {
        return std::error_code(errno, std::system_category());
    }

    if (isV6) {
        // Allow IPv4 packets on this IPv6 socket
        int off = 0;
        setsockopt(udp_socket, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

        struct sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6addr_any;

        if (::bind(udp_socket, reinterpret_cast<struct sockaddr*>(&addr6), sizeof(addr6)) == -1) {
            return std::error_code(errno, std::system_category());
        }
    } else {
        struct sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        addr4.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces

        if (::bind(udp_socket, reinterpret_cast<struct sockaddr*>(&addr4), sizeof(addr4)) == -1) {
            return std::error_code(errno, std::system_category());
        }
    }

    // Resolve the actual port (Crucial for port 0)
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(udp_socket, reinterpret_cast<struct sockaddr*>(&ss), &len) == -1) {
        return std::error_code(errno, std::system_category());
    }
    uint16_t localPort = (ss.ss_family == AF_INET6)
        ? ntohs(reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_port)
        : ntohs(reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port);

    // Register with the EventLoop
    auto regResult = m_loop.addSource(udp_socket, EPOLLIN, UDPReceiverTag{
        this,
        (int)udp_socket,
        context,
        handler
    });

    if (!regResult) {
        // If epoll registration fails, ScopedFd will automatically close the socket
        // when we return the error here.
        return regResult.error();
    }

    // Move ownership of ScopedFd to our map only after success
    m_port_to_fd_map.emplace(localPort, std::move(udp_socket));

    return {static_cast<int>(localPort)};
}

Result<void> UDPReceiver::unsubscribe(uint16_t port) {
    // Find the port in our map
    auto it = m_port_to_fd_map.find(port);

    // If it doesn't exist, return an error instead of silently doing nothing
    if (it == m_port_to_fd_map.end()) {
        return std::error_code(ENOENT, std::system_category());
    }

    // Get the raw FD to remove it from the EventLoop
    int fd = it->second;

    // Remove from EventLoop (Internal epoll_ctl DEL)
    auto loopRes = m_loop.removeSource(fd);

    // Remove from our map
    // Because m_port_to_fd_map stores ScopedFd, the destructor
    // of ScopedFd will automatically call ::close(fd) here.
    m_port_to_fd_map.erase(it);

    // Return the loop result (or success if we don't care about epoll_ctl DEL errors)
    return loopRes;
}

// NOTE: handleRead assumes exclusive access to m_flatBuffer.
// If multiple threads trigger handleRead simultaneously via different
// EventLoops, data corruption will occur.
void UDPReceiver::handleRead(int fd, void* context, PacketHandlerFn handler) {
    assert(std::this_thread::get_id() == m_ownerThreadId && 
            "UDPReceiver handled on wrong thread!");

    // 1. Reset the lengths BEFORE the system call.
    // This ensures the kernel can write the full address for every packet slot.
    for (auto& header : m_msgHeaders) {
        header.msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
    }

    // recvmmsg allows us to grab up to BATCH_SIZE packets in one go.
    // MSG_DONTWAIT ensures we don't block if the buffer was emptied by a race condition.
    int numPackets = recvmmsg(
            fd, m_msgHeaders.data(), m_config.batchSize,
            MSG_DONTWAIT, nullptr);
    if (numPackets < 0) return;

    // Iterate through only the number of packets actually received
    for (int k = 0; k < numPackets; ++k) {
        uint32_t status = PacketStatus::OK;

        // Check if the MSG_TRUNC flag was set by the kernel
        if (m_msgHeaders[k].msg_hdr.msg_flags & MSG_TRUNC) {
            status |= PacketStatus::TRUNCATED;
        }

        size_t len = m_msgHeaders[k].msg_len;
        if (len > 0) {
            // Data is at the specific offset in the flat buffer
            uint8_t* packetData = m_cachedBasePtr + (k * m_alignedBufferSize);
            // Dispatch the packet to the user-defined handler
            handler(context, packetData, len, status);
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
