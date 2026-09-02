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
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// Library headers
#include <atu_reactor/Export.h>
#include <atu_reactor/Result.h>
#include <atu_reactor/ScopedFd.h>
#include <atu_reactor/Types.h>

namespace atu_reactor {

// Raw function pointer types for zero-overhead execution
using TaskCallbackFn = void(*)(void* context);
using StreamCallbackFn = void(*)(uint32_t events, void* context);

// Lightweight structure replacing std::function/MoveOnlyTask
struct Task {
    TaskCallbackFn fn{nullptr};
    void* context{nullptr};

    void operator()() const noexcept {
        if (fn) fn(context);
    }
};

// Forward declarations
class EventLoop;
class UDPReceiver;

// Define the tags.
// We use pointers here because they are "Incomplete Types"
// which std::variant handles fine as long as they are pointers.
struct TimerTag { EventLoop* loop; };
struct UDPReceiverTag {
    UDPReceiver* receiver;
    int fd;
    void* userContext;
    PacketHandlerFn handler;
};
struct StreamTag {
    StreamCallbackFn callback;
    int fd;
    void* context; // Pointer to the Streaming instance
};

// The dispatch variant
using InternalHandler = std::variant<
    std::monostate,
    TimerTag,
    UDPReceiverTag,
    StreamTag
>;

using Clock = std::chrono::steady_clock;
using Timestamp = std::chrono::time_point<Clock>;
using Duration = std::chrono::milliseconds;

// ID to track and cancel timers
using TimerId = uint64_t;

using EventCallbackFn = void(*)(void* context, uint32_t events);

/**
 * @class EventLoop
 * @brief A lightweight wrapper around Linux epoll for asynchronous I/O multiplexing.
 * This class manages a set of file descriptors and triggers registered callbacks
 * when specific events (like data being available to read) occur.
 */
class ATU_API EventLoop {
    public:
        /**
         * @brief Initializes the epoll instance.
         * @throws std::runtime_error if epoll_create1 fails.
         */
        EventLoop();

        // Destructor is default because ScopedFd automatically closes epoll_fd.
        ~EventLoop();

        /**
         * @brief Registers a file descriptor and its event handler with the event loop.
         *
         * Configures the underlying epoll instance to monitor @p fd for the requested
         * events and maps it to the provided @p handler.
         *
         * @param fd The file descriptor to monitor. Must be valid, open, and not already registered.
         * @param eventMask Bitmask of epoll events to watch for (e.g., `EPOLLIN`, `EPOLLOUT`, `EPOLLET`).
         * @param handler The callback function to execute when a matching event occurs.
         *
         * @return Result<void> Success status (`Result::ok()`), or an error payload detailing
         *         the registration failure (e.g., `EEXIST` if @p fd is already registered, `EBADF` if invalid).
         */
        Result<void> addSource(int fd, uint32_t eventMask, InternalHandler handler);

        /**
         * @brief Removes a file descriptor and its callback from the loop.
         * @param fd The descriptor to stop monitoring.
         */
        Result<void> removeSource(int fd);

        /**
         * @brief Registers a stream-based file descriptor (e.g., TCP socket, pipe) with the event loop.
         *
         * Adds @p fd to the underlying epoll interest list for read events (`EPOLLIN`). When the descriptor
         * becomes readable, @p callback is invoked on the event loop thread.
         *
         * @param fd The file descriptor to monitor. Must be valid, open, and configured as non-blocking.
         * @param callback The function to execute when data is available. Receives event flags and @p context.
         * @param context User data pointer passed directly to @p callback on invocation. May be `nullptr`.
         *
         * @return Result<void> Success (`Result::ok()`), or an error payload if registration fails
         *         (e.g., duplicate registration or `epoll_ctl` failure).
         *
         * @note FDs with values lower than 1024 use direct array indexing for low-latency dispatch;
         *       larger FDs fall back to hash map storage.
         * @warning @p callback runs directly on the event loop thread. Do not perform blocking operations
         *          inside the callback.
         */
        Result<void> addStreamSource(int fd, StreamCallbackFn callback, void* context);

        /**
         * @brief Modifies the epoll event mask for an already-registered stream source.
         *
         * Adjusts the monitored events for @p fd by appending or combining @p extraEvents
         * with the existing event configuration.
         *
         * @param fd The file descriptor to modify. Must already be registered via addStreamSource().
         * @param extraEvents Additional epoll event flags to watch for (e.g., `EPOLLOUT`, `EPOLLET`).
         *
         * @return Result<void> Success (`Result::ok()`), or an error if @p fd is not found or `epoll_ctl` fails.
         */
        Result<void> modifyStreamSource(int fd, uint32_t extraEvents);

        /**
         * @brief Unregisters a stream file descriptor from the event loop.
         *
         * Removes @p fd from the epoll interest list and cleans up its associated handler storage.
         *
         * @param fd The file descriptor to remove.
         *
         * @return Result<void> Success (`Result::ok()`), or an error if @p fd was not registered.
         */
        Result<void> removeStreamSource(int fd);

        /**
         * @brief Waits for and dispatches pending events.
         * @param timeoutMs Max time to wait. -1 = infinite, 0 = non-blocking poll.
         */
        Result<void> runOnce(int timeoutMs);

        /**
         * @brief Schedules a function to be executed in the next iteration of the loop.
         * Use this instead of runAfter(0, ...) to avoid system call overhead.
         */
        void runInLoop(TaskCallbackFn fn, void* context);

        /**
         * @brief Run a callback once after a delay.
         * @return Unique ID to allow cancellation.
         */
        Result<TimerId> runAfter(Duration delay, TaskCallbackFn fn, void* context);

        /**
         * @brief Run a callback periodically.
         * @return Unique ID to allow cancellation.
         */
        Result<TimerId> runEvery(Duration interval, TaskCallbackFn fn, void* context);

        /**
         * @brief Cancel a specific timer if it hasn't run yet.
         */
        Result<void> cancelTimer(TimerId id);

        // Prevent copying a reactor instance
        EventLoop(const EventLoop&) = delete;
        EventLoop& operator=(const EventLoop&) = delete;

    private:
        // Internal struct to manage a scheduled task
        struct Timer {
            Timestamp expiration;
            Duration interval; // Zero if one-shot
            Task task;
            TimerId id;
            bool repeat{false};

            // Comparator for min-heap (earliest expiration on top)
            bool operator>(const Timer& other) const noexcept {
                if (expiration != other.expiration) {
                    return expiration > other.expiration; // Min-heap: lower time gets priority
                }
                return id > other.id;
            }
        };

        void handleTimerRead();
        void resetTimerFd();
        void insertTimer(const Timer& t);

        static constexpr int MAX_EVENTS = 128; // Buffer size for events returned per wait

        // RAII wrapper for the epoll instance file descriptor
        ScopedFd m_epoll_fd;
        ScopedFd m_timer_fd;

        struct EpollInternal;
        std::unique_ptr<EpollInternal> m_impl;

        struct Source {
            InternalHandler handler;
        };

        // Queue for deferred execution
        std::vector<Task> m_pendingTasks;

        // Hybrid storage to prevent massive allocations on high FD numbers
        static constexpr int MAX_FAST_FDS = 1024; // Limit for direct indexing
        Source m_fastSources[MAX_FAST_FDS];
        std::unordered_map<int, Source> m_slowSources;

        // Min-heap container over std::vector
        std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> m_timers;

        // Set of active Timer IDs for O(1) existence checks & lazy deletion
        std::unordered_set<TimerId> m_activeTimers;

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
