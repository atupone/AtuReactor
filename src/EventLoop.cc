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
#include <iostream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/timerfd.h>

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
    addSource(m_timer_fd, EPOLLIN, [this](uint32_t) {
        this->handleTimerRead();
    });
}

/**
 * @brief Destructor
 * Must be defined here because the compiler needs the full definition
 * of EpollInternal to delete m_impl.
 */
EventLoop::~EventLoop() = default;

void EventLoop::addSource(int fd, uint32_t eventMask, EventCallback cb) {
    struct epoll_event ev{};
    ev.events = eventMask;
    ev.data.fd = fd; // Store the FD in data so we can find it in the callback map later

    // Tell the kernel to start monitoring this FD
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        throw std::runtime_error("EventLoop: epoll_ctl ADD failed for FD " + std::to_string(fd));
    }

    // Store the callback in our local map
    m_callbacks[fd] = std::move(cb);
}

void EventLoop::removeSource(int fd) {
    // Tell the kernel to stop monitoring this FD.
    // Note: Some older kernels require non-null event even for DEL.
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        // We only report if the error isn't "Not Found" (which happens if already closed)
        if (errno != ENOENT) {
            perror("EventLoop: epoll_ctl DEL failed");
        }
    }

    // Clean up local resources
    m_callbacks.erase(fd);
}

void EventLoop::runOnce(int timeoutMs) {
    // Block until at least one event occurs or the timeout expires
    int ready = epoll_wait(m_epoll_fd, m_impl->events.data(), MAX_EVENTS, timeoutMs);

    if (ready < 0) {
        // EINTR means a system signal (like Ctrl+C) woke us up; this is not a failure.
        if (errno == EINTR) return;
        perror("EventLoop: epoll_wait");
        return;
    }

    // Iterate only through the number of file descriptors that actually have events
    for (int i = 0; i < ready; ++i) {
        int fd = m_impl->events[i].data.fd;
        uint32_t triggeredEvents = m_impl->events[i].events;

        auto it = m_callbacks.find(fd);
        if (it != m_callbacks.end()) {
            try {
                // Execute callback safely
                it->second(triggeredEvents);
            } catch (const std::exception& e) {
                // Log error but keep loop alive
                std::cerr << "EventLoop: Exception in callback for FD " << fd
                    << ": " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "EventLoop: Unknown exception in callback for FD " << fd << std::endl;
            }
        }
    }
}

TimerId EventLoop::runAfter(Duration delay, TimerCallback cb) {
    Timestamp when = Clock::now() + delay;
    Timer t{when, Duration(0), std::move(cb), m_nextTimerId++, false};
    insertTimer(std::move(t));
    return t.id;
}

TimerId EventLoop::runEvery(Duration interval, TimerCallback cb) {
    Timestamp when = Clock::now() + interval;
    Timer t{when, interval, std::move(cb), m_nextTimerId++, true};
    insertTimer(std::move(t));
    return t.id;
}

void EventLoop::cancelTimer(TimerId id) {
    auto it = m_activeTimers.find(id);
    if (it != m_activeTimers.end()) {
        m_timers.erase(it->second); // Remove from the set
        m_activeTimers.erase(it);   // Remove from the map

        // If we removed the earliest timer, we must update the hardware timer
        // to reflect the new earliest time.
        resetTimerFd();
    }
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
    struct itimerspec newValue{};
    struct itimerspec oldValue{};

    if (!m_timers.empty()) {
        auto nextExpire = m_timers.begin()->expiration;
        auto now = Clock::now();

        // Calculate delay from now until next expiration
        auto delay = std::chrono::duration_cast<std::chrono::microseconds>(nextExpire - now);
        if (delay.count() < 100) delay = std::chrono::microseconds(100); // Minimum 100us safety

        newValue.it_value.tv_sec = delay.count() / 1000000;
        newValue.it_value.tv_nsec = (delay.count() % 1000000) * 1000;
    } else {
        // No timers: disarm the timerfd
        newValue.it_value.tv_sec = 0;
        newValue.it_value.tv_nsec = 0;
    }

    timerfd_settime(m_timer_fd, 0, &newValue, &oldValue);
}

void EventLoop::handleTimerRead() {
    uint64_t expiredCount;
    // We must read the timerfd to clear the event, otherwise epoll triggers immediately again
    ssize_t n = ::read(m_timer_fd, &expiredCount, sizeof(expiredCount));
    if (n != sizeof(expiredCount)) {
        return; // Should not happen
    }

    Timestamp now = Clock::now();

    // Process all expired timers
    // We must be careful: the callback might add new timers or cancel existing ones.
    // Iterating while modifying requires care.

    // 1. Extract all expired timers from the set
    std::vector<Timer> expired;
    while (!m_timers.empty() && m_timers.begin()->expiration <= now) {
        expired.push_back(*m_timers.begin());
        m_activeTimers.erase(m_timers.begin()->id);
        m_timers.erase(m_timers.begin());
    }

    // 2. Execute callbacks
    for (const auto& t : expired) {
        if (t.callback) {
            try {
                t.callback();
            } catch(...) { /* Log error */ }
        }

        // 3. Re-schedule if repeating
        if (t.repeat) {
            Timer nextTimer = t;
            nextTimer.expiration = now + t.interval;
            insertTimer(std::move(nextTimer));
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
