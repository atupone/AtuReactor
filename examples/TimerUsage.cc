/*
 * Copyright (C) 2026 Alfredo Tupone
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This example is part of the AtuReactor project.
 */

#include <atu_reactor/EventLoop.h>
#include <iostream>
#include <chrono>

using namespace atu_reactor;

int main() {
    EventLoop loop;

    std::cout << "Starting Timer Example..." << std::endl;

    // Example 1: Periodic Heartbeat (every 1 second)
    int counter = 0;
    auto res1 = loop.runEvery(std::chrono::seconds(1), [&counter]() {
        counter++;
        std::cout << "[Periodic] Heartbeat " << counter << "s elapsed" << std::endl;
    });
    if (!res1) {
        std::cerr << "Failed to schedule periodic timer" << std::endl;
    }

    // Example 2: One-shot delayed task (triggers once after 3.5 seconds)
    auto res2 = loop.runAfter(std::chrono::milliseconds(3500), []() {
        std::cout << "[One-Shot] 3.5 seconds have passed. Cleaning up resources..." << std::endl;
    });
    if (!res2) {
        std::cerr << "Failed to schedule one-shot timer" << std::endl;
    }

    // Example 3: Self-canceling logic
    // We schedule a task for 10s, but we will exit the loop before it fires
    auto res3 = loop.runAfter(std::chrono::seconds(10), []() {
        std::cout << "This should never print!" << std::endl;
    });
    if (!res3) {
        std::cerr << "Failed to schedule 10s timer: " << res3.error().message() << std::endl;
    }

    // Run the loop for 5 seconds total
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        // Use .value() to ensure the loop hasn't encountered a system error.
        // If epoll_wait fails, this will throw/abort with the system error message.
        loop.runOnce(100).value(); // Poll with 100ms timeout
    }

    std::cout << "Example finished. The 10s timer was safely discarded." << std::endl;
    return 0;
}
