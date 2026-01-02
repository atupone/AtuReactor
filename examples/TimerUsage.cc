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
    loop.runEvery(std::chrono::seconds(1), [&counter]() {
        counter++;
        std::cout << "[Periodic] Heartbeat " << counter << "s elapsed" << std::endl;
    });

    // Example 2: One-shot delayed task (triggers once after 3.5 seconds)
    loop.runAfter(std::chrono::milliseconds(3500), []() {
        std::cout << "[One-Shot] 3.5 seconds have passed. Cleaning up resources..." << std::endl;
    });

    // Example 3: Self-canceling logic
    // We schedule a task for 10s, but we will exit the loop before it fires
    loop.runAfter(std::chrono::seconds(10), []() {
        std::cout << "This should never print!" << std::endl;
    });

    // Run the loop for 5 seconds total
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        loop.runOnce(100); // Poll with 100ms timeout
    }

    std::cout << "Example finished." << std::endl;
    return 0;
}
