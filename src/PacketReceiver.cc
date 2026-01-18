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
#include <atu_reactor/PacketReceiver.h>

// System headers
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

namespace atu_reactor {

PacketReceiver::PacketReceiver(EventLoop& loopRef, ReceiverConfig config)
        : m_loop(loopRef),
        m_config(config),
        m_ownerThreadId(std::this_thread::get_id()),
        m_ioVectors(config.batchSize) // Pre-size directly in initializer
{
    // Calculate size and alignment
    // Round up the buffer size to a multiple of 64 for cache-line alignment
    m_alignedBufferSize = (m_config.bufferSize + 63) & ~63; // Round up to multiple of 64

    // Total memory needed for all packets
    const size_t totalRequested = m_config.batchSize * m_alignedBufferSize;

    // Hugepages are typically 2MB. We round up the total mapping size to a 2MB boundary.
    const size_t HUGEPAGE_SIZE = 2 * 1024 * 1024;
    m_mappedSize = (totalRequested + HUGEPAGE_SIZE - 1) & ~(HUGEPAGE_SIZE - 1);

    // Memory Allocation via mmap
    // Try to allocate memory using Hugepages first for better TLB performance
    m_hugeBuffer = static_cast<uint8_t*>(mmap(nullptr, m_mappedSize,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));

    // Fallback: If Hugepages are not available/reserved, use standard 4KB pages
    if (m_hugeBuffer == MAP_FAILED) {
        m_hugeBuffer = static_cast<uint8_t*>(mmap(nullptr, m_mappedSize,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

        if (m_hugeBuffer == MAP_FAILED) {
            m_mappedSize = 0;
            throw std::runtime_error("Failed to allocate packet buffer via mmap");
        }
    }

    // Set the base pointer for the recvmmsg logic
    m_cachedBasePtr = m_hugeBuffer;

    // Initialize iovecs using the aligned stride
    for (int i = 0; i < m_config.batchSize; ++i) {
        // Calculate offset into the flat buffer
        uint8_t* packetStart = m_cachedBasePtr + (i * m_alignedBufferSize);

        // Map iovec to the specific row in our 2D vector
        m_ioVectors[i].iov_base = packetStart;
        m_ioVectors[i].iov_len = m_config.bufferSize;
    }
}

PacketReceiver::~PacketReceiver() {
    // Safety: Explicitly remove all sources from EventLoop so it doesn't
    // try to call callbacks on this destroyed object.
    for (auto const& [port, fd] : m_port_to_fd_map) {
        m_loop.removeSource(fd);
    }

    // Manually unmap the buffer
    if (m_hugeBuffer != nullptr && m_hugeBuffer != MAP_FAILED) {
        ::munmap(m_hugeBuffer, m_mappedSize);
    }
}

Result<int> PacketReceiver::subscribe(uint16_t port,
                                      [[maybe_unused]] void* context,
                                      PacketHandlerFn handler) {
    checkThread();

    // Safety: Validate handler before doing any work
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

    return Result<int>(0); // Base says "OK to proceed"
}

Result<void> PacketReceiver::unsubscribe(uint16_t port) {
    checkThread();

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

// Enforce the thread-hostile requirement
void PacketReceiver::checkThread() const {
    assert(std::this_thread::get_id() == m_ownerThreadId &&
           "PacketReceiver accessed from wrong thread!");
}

void PacketReceiver::dispatch(int n, const PacketMetadata* meta, PacketHandlerFn handler, void* context) {
    for (int i = 0; i < n; ++i) {
        uint8_t* packetData = m_cachedBasePtr + (i * m_alignedBufferSize);
        handler(context, packetData, meta[i].len, PacketStatus::OK, meta[i].ts);
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
