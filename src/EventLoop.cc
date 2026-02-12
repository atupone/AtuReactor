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

// Interface
#include <atu_reactor/EventLoop.h>

// System headers
#include <errno.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/timerfd.h>

// Library headers
#include <atu_reactor/UDPReceiver.h>

namespace atu_reactor {

struct EventLoop::EpollInternal {
    std::vector<struct epoll_event> events;

    explicit EpollInternal(int size) : events(size) {}
};

/**
 * Constructor: Creates the epoll interest list.
 * epoll_create1(0) is used to avoid the legacy size parameter.
 */
EventLoop::EventLoop()
    : m_epoll_fd(epoll_create1(0)),
    m_timer_fd(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
    m_impl(std::make_unique<EpollInternal>(MAX_EVENTS))
{
    if (m_epoll_fd < 0) throw std::runtime_error("Failed to create epoll");
    if (m_timer_fd < 0) throw std::runtime_error("Failed to create timerfd");

    // Register the timer FD into the epoll loop immediately
    addSource(m_timer_fd, EPOLLIN, TimerTag{this}).value();
}

/**
 * @brief Destructor
 * Must be defined here because the compiler needs the full definition
 * of EpollInternal to delete m_impl.
 */
EventLoop::~EventLoop() = default;

Result<void> EventLoop::addSource(int fd, uint32_t eventMask, InternalHandler handler) {
    if (fd < 0) {
        return std::error_code(EBADF, std::generic_category());
    }

    struct epoll_event ev{};
    ev.events = eventMask;

    // Store the callback in our local map
    // Point epoll directly to the memory address of this source
    if (fd < MAX_FAST_FDS) {
        m_fastSources[fd] = {handler};
        ev.data.ptr = &m_fastSources[fd];
    } else {
        m_slowSources[fd] = {handler};
        ev.data.ptr = &m_slowSources[fd];
    }

    // Tell the kernel to start monitoring this FD
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) [[unlikely]] {
        // Return the system error code instead of throwing or ignoring
        return std::error_code(errno, std::generic_category());
    }

    // Return success
    return Result<void>::success();
}

Result<void> EventLoop::removeSource(int fd) {
    if (fd < MAX_FAST_FDS) {
        m_fastSources[fd].handler = std::monostate{};
    } else {
        m_slowSources.erase(fd);
    }

    // Remove from epoll
    // Note: We do this before clearing our internal vector.
    // In Linux < 2.6.9, epoll_ctl(DEL) required a valid pointer to event,
    // but modern kernels allow NULL.
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) [[unlikely]] {
        // If the FD wasn't registered, epoll_ctl returns ENOENT.
        // We can choose to return this error or ignore it.
        // Returning it is better for library debugging.
        return std::error_code(errno, std::system_category());
    }

    return Result<void>::success();
}

Result<void> EventLoop::runOnce(int timeoutMs) {
    // Optimization: If we have deferred tasks (e.g., PCAP flood),
    // do not block the CPU. Force non-blocking poll.
    if (!m_pendingTasks.empty()) {
        timeoutMs = 0;
    }

    // Block until at least one event occurs or the timeout expires
    int ready = epoll_wait(m_epoll_fd, m_impl->events.data(), MAX_EVENTS, timeoutMs);

    if (ready < 0) [[unlikely]] {
        // EINTR means a system signal (like Ctrl+C) woke us up; this is not a failure.
        if (errno == EINTR) {
            return Result<void>::success();
        }

        // Return the actual system error wrapped in our Result type
        return std::error_code(errno, std::system_category());
    }

    // Iterate only through the number of file descriptors that actually have events
    for (int i = 0; i < ready; ++i) {
        // Retrieve the pointer we saved earlier
        Source* source = static_cast<Source*>(m_impl->events[i].data.ptr);

        if (source) {
            // 2. Dispatch using the pointer
            std::visit([this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, TimerTag>) {
                    // Timers generally only trigger on EPOLLIN,
                    // but we call it normally.
                    handleTimerRead();
                } else if constexpr (std::is_same_v<T, UDPReceiverTag>) {
                    // Pass the event to the receiver
                    arg.receiver->handleRead(arg.fd, arg.userContext, arg.handler);
                }
            }, source->handler);
        }
    }

    // 3. Execute Pending Tasks
    // We swap to a local vector to handle re-entrant calls safely.
    if (!m_pendingTasks.empty()) {
        std::vector<std::function<void()>> tasks;
        m_pendingTasks.swap(tasks);

        for (const auto& task : tasks) {
            task();
        }
    }


    // Final success return to satisfy the Result<void> return type
    return Result<void>::success();
}

void EventLoop::runInLoop(std::function<void()> cb) {
    m_pendingTasks.push_back(std::move(cb));
}

Result<TimerId> EventLoop::runAfter(Duration delay, TimerCallback cb) {
    if (delay.count() < 0) [[unlikely]] {
        return std::error_code(EINVAL, std::system_category());
    }

    TimerId id = m_nextTimerId++;
    Timestamp when = Clock::now() + delay;
    Timer t{when, Duration(0), std::move(cb), id, false};

    insertTimer(std::move(t));

    // Return the id; it will be wrapped in Result<TimerId> automatically
    return t.id;
}

Result<TimerId> EventLoop::runEvery(Duration interval, TimerCallback cb) {
    if (interval.count() <= 0) [[unlikely]] {
        return std::error_code(EINVAL, std::system_category());
    }

    TimerId id = m_nextTimerId++;
    Timestamp when = Clock::now() + interval;
    Timer t{when, interval, std::move(cb), id, true};

    insertTimer(std::move(t));

    return id;
}

Result<void> EventLoop::cancelTimer(TimerId id) {
    auto it = m_activeTimers.find(id);

    if (it == m_activeTimers.end()) [[unlikely]] {
        // Return an error if the TimerId was not found
        return std::error_code(ENOENT, std::system_category());
    }

    m_timers.erase(it->second); // Remove from the set
    m_activeTimers.erase(it);   // Remove from the map

    // If we removed the earliest timer, we must update the hardware timer
    // to reflect the new earliest time.
    resetTimerFd();

    return Result<void>::success();
}

void EventLoop::insertTimer(Timer t) {
    bool earliestChanged = false;
    if (m_timers.empty() || t.expiration < m_timers.begin()->expiration) {
        earliestChanged = true;
    }

    auto result = m_timers.insert(std::move(t));
    m_activeTimers[result.first->id] = result.first;

    if (earliestChanged) {
        resetTimerFd();
    }
}

void EventLoop::resetTimerFd() {
    if (m_timers.empty()) {
        // Disarm timer
        struct itimerspec newValue{};
        timerfd_settime(m_timer_fd, 0, &newValue, nullptr);
        return;
    }

    auto now = Clock::now();
    auto expiration = m_timers.begin()->expiration;

    struct itimerspec newValue{};

    if (expiration <= now) {
        // Timer already expired! Set to smallest non-zero value
        // to trigger the epoll loop immediately.
        newValue.it_value.tv_sec = 0;
        newValue.it_value.tv_nsec = 1;
    } else {
        // Calculate delay from now until next expiration
        auto dur = expiration - now;
        newValue.it_value.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
        newValue.it_value.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count() % 1'000'000'000;
    }

    // Using Absolute Time is safer against system clock drift
    timerfd_settime(m_timer_fd, TFD_TIMER_ABSTIME, &newValue, nullptr);
}

void EventLoop::handleTimerRead() {
    uint64_t expiredCount;
    // We must read the timerfd to clear the event, otherwise epoll triggers immediately again
    ssize_t n = ::read(m_timer_fd, &expiredCount, sizeof(expiredCount));
    if (n != sizeof(expiredCount)) [[unlikely]] {
        return; // Should not happen
    }

    Timestamp now = Clock::now();

    // Process all expired timers
    // We must be careful: the callback might add new timers or cancel existing ones.
    // Iterating while modifying requires care.

    // 1. Extract all expired timers from the set
    std::vector<Timer> expired;
    while (!m_timers.empty() && m_timers.begin()->expiration <= now) {
        auto it = m_timers.begin();
        expired.push_back(std::move(*it));
        m_activeTimers.erase(it->id);
        m_timers.erase(it);
    }

    // 2. Execute callbacks
    for (auto& t : expired) {
        if (t.callback) {
            t.callback();
        }

        // 3. Re-schedule if repeating
        if (t.repeat) {
            t.expiration += t.interval;
            insertTimer(t);
        }
    }

    // 4. Ensure hardware timer waits for the next valid event
    resetTimerFd();
}

} // namespace atu_reactor


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
