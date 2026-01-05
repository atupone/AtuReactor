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

#include <gtest/gtest.h>
#include <atu_reactor/EventLoop.h>
#include <chrono>
#include <thread>
#include <atomic>

using namespace atu_reactor;
using namespace std::chrono_literals;

class TimerTest : public ::testing::Test {
protected:
    EventLoop loop;
};

// Test 1: Verify One-Shot Timing
TEST_F(TimerTest, OneShotExecutesAfterDelay) {
    std::atomic<bool> fired{false};
    auto start = std::chrono::steady_clock::now();

    // Schedule for 100ms
    auto res = loop.runAfter(std::chrono::milliseconds(100), [&]() {
        fired = true;
    });
    ASSERT_TRUE(res.has_value()) << "runAfter failed: " << res.error().message();

    // Run once immediately: should not fire
    loop.runOnce(0);
    EXPECT_FALSE(fired);

    // Wait until 150ms total have passed
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    loop.runOnce(0);

    EXPECT_TRUE(fired);
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(elapsed.count(), 100);
}

// Test 2: Verify Periodic Repetition
TEST_F(TimerTest, PeriodicTimerRepeats) {
    int fireCount = 0;

    // Fire every 50ms
    loop.runEvery(std::chrono::milliseconds(50), [&]() {
        fireCount++;
    });

    // Run loop for 170ms
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(170)) {
        loop.runOnce(10); // Check frequently
    }

    // Should have fired at 50ms, 100ms, and 150ms
    EXPECT_EQ(fireCount, 3);
}

// Test 3: Verify Cancellation
TEST_F(TimerTest, CancelledTimerNeverFires) {
    bool fired = false;

    // Capture the Result object
    auto res = loop.runAfter(std::chrono::milliseconds(50), [&]() {
        fired = true;
    });

    // Verify success
    ASSERT_TRUE(res.has_value()) << "runAfter failed: " << res.error().message();

    // Unwrap the actual TimerId
    TimerId id = res.value();

    // Now you can cancel it
    loop.cancelTimer(id).value();

    // Wait and run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loop.runOnce(0);

    EXPECT_FALSE(fired);
}

// Test 4: Multiple Timers (Min-Heap Verification)
// This ensures that even if we add timers out of order, the earliest one wakes the loop
TEST_F(TimerTest, OutOfOrderTimers) {
    std::vector<int> executionOrder;

    loop.runAfter(
        std::chrono::milliseconds(200),
        [&]() { executionOrder.push_back(200); }).value();
    loop.runAfter(
        std::chrono::milliseconds(50),
        [&]() { executionOrder.push_back(50);  }).value();
    loop.runAfter(
        std::chrono::milliseconds(100),
        [&]() { executionOrder.push_back(100); }).value();

    // Run until all 3 fire
    auto start = std::chrono::steady_clock::now();
    while (executionOrder.size() < 3 &&
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
        auto res = loop.runOnce(10);
        ASSERT_TRUE(res.has_value()) << "Loop execution failed: " << res.error().message();
    }

    ASSERT_EQ(executionOrder.size(), 3);
    EXPECT_EQ(executionOrder[0], 50);
    EXPECT_EQ(executionOrder[1], 100);
    EXPECT_EQ(executionOrder[2], 200);
}
