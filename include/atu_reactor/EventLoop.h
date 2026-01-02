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
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>

// Library headers
#include <atu_reactor/ScopedFd.h>

namespace atu_reactor {

using Clock = std::chrono::steady_clock;
using Timestamp = std::chrono::time_point<Clock>;
using Duration = std::chrono::milliseconds;

// ID to track and cancel timers
using TimerId = uint64_t;

/**
 * @class EventLoop
 * @brief A lightweight wrapper around Linux epoll for asynchronous I/O multiplexing.
 * This class manages a set of file descriptors and triggers registered callbacks
 * when specific events (like data being available to read) occur.
 */
class EventLoop {
    public:
        // Callback signature: takes a uint32_t representing the triggered epoll events.
        using EventCallback = std::function<void(uint32_t)>;
        using TimerCallback = std::function<void()>;

        /**
         * @brief Initializes the epoll instance.
         * @throws std::runtime_error if epoll_create1 fails.
         */
        EventLoop();

        // Destructor is default because ScopedFd automatically closes epoll_fd.
        ~EventLoop();

        /**
         * @brief Registers a file descriptor and a callback to the event loop.
         * @param fd The file descriptor to monitor (must be unique).
         * @param eventMask The epoll events to watch for (e.g., EPOLLIN, EPOLLET).
         * @param cb The function to execute when the event triggers.
         * @throws std::runtime_error if the OS fails to add the source.
         */
        void addSource(int fd, uint32_t eventMask, EventCallback cb);

        /**
         * @brief Removes a file descriptor and its callback from the loop.
         * @param fd The descriptor to stop monitoring.
         */
        void removeSource(int fd);

        /**
         * @brief Waits for and dispatches pending events.
         * @param timeoutMs Max time to wait. -1 = infinite, 0 = non-blocking poll.
         */
        void runOnce(int timeoutMs);

        /**
         * @brief Run a callback once after a delay.
         * @return Unique ID to allow cancellation.
         */
        TimerId runAfter(Duration delay, TimerCallback cb);

        /**
         * @brief Run a callback periodically.
         * @return Unique ID to allow cancellation.
         */
        TimerId runEvery(Duration interval, TimerCallback cb);

        /**
         * @brief Cancel a specific timer if it hasn't run yet.
         */
        void cancelTimer(TimerId id);

        // Prevent copying a reactor instance
        EventLoop(const EventLoop&) = delete;
        EventLoop& operator=(const EventLoop&) = delete;

    private:
        // Internal struct to manage a scheduled task
        struct Timer {
            Timestamp expiration;
            Duration interval; // Zero if one-shot
            TimerCallback callback;
            TimerId id;
            bool repeat;

            // Sorting for std::set (earliest time first)
            bool operator<(const Timer& other) const {
                if (expiration != other.expiration)
                    return expiration < other.expiration;
                return id < other.id;
            }
        };

        void handleTimerRead();
        void resetTimerFd();
        void insertTimer(Timer t);

        static constexpr int MAX_EVENTS = 128; // Buffer size for events returned per wait

        // RAII wrapper for the epoll instance file descriptor
        ScopedFd m_epoll_fd;
        ScopedFd m_timer_fd;

        struct EpollInternal;
        std::unique_ptr<EpollInternal> m_impl;

        // Maps FD -> User Callback for O(1) lookups during the event loop
        std::unordered_map<int, EventCallback> m_callbacks;

        // Ordered queue of pending timers
        std::set<Timer> m_timers;

        // Map ID -> Iterator (for fast cancellation O(log N))
        std::unordered_map<TimerId, std::set<Timer>::iterator> m_activeTimers;

        uint64_t m_nextTimerId = 1;
};

} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
