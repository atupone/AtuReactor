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
#include <unistd.h>

namespace atu_reactor {

/**
 * @brief RAII Wrapper for File Descriptors.
 * Automatically closes the FD when it goes out of scope.
 */
struct ScopedFd {
    int fd = -1;

    ScopedFd(int f = -1) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }

    // Delete copy to prevent double-close
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    // Allow move
    ScopedFd(ScopedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.fd);
            other.fd = -1;
        }
        return *this;
    }

    void reset(int new_fd = -1) {
        if (fd >= 0) ::close(fd);
        fd = new_fd;
    }

    // Implicit conversion to int for easy usage with system calls
    operator int() const { return fd; }
};

} // namespace atu_reactor

// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
