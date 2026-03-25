/*
 * Copyright (C) 2026 Alfredo Tupone
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This example is part of the AtuReactor project.
 */

// System headers
#include <iostream>
#include <memory>
#include <string>

// Library headers
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/TcpMessaging.h>
#include <atu_reactor/Types.h>
#include <atu_reactor/UDPReceiver.h>

using namespace atu_reactor;

void onRadarPacket(void* context, const uint8_t* , size_t len, uint32_t status, struct timespec ts) {
    auto* uiBridge = static_cast<TcpMessaging*>(context);

    if (status & PacketStatus::TRUNCATED) {
        std::cerr << "[Radar] Warning: Packet truncated!" << std::endl;
    }

    // Forward a summary to the TCP UI Bridge
    std::string update = "{\"status\":\"target_detected\", \"size\":" + std::to_string(len) +
        ", \"ts_sec\":" + std::to_string(ts.tv_sec) + "}\n";

    uiBridge->send(update);
}

int main() {
    EventLoop loop;

    // Setup the TCP Messaging (The Bridge to your UI)
    auto uiBridge = std::make_shared<TcpMessaging>(loop);

    // Connect to a local server (e.g., a Python script or 'nc -l 9000')
    uiBridge->connect("127.0.0.1", 9000, [](const uint8_t* data, size_t len) {
        std::string cmd(reinterpret_cast<const char*>(data), len);
        std::cout << "[UI -> Bridge] Received command: " << cmd << std::endl;
    });

    // Setup UDP Receiver (The Radar Input)
    ReceiverConfig config;
    config.batchSize = 10;
    UDPReceiver radar(loop, config);

    // Listening on port 10001
    auto res = radar.subscribe(10001, uiBridge.get(), onRadarPacket);

    if (!res) {
        std::cerr << "Failed to subscribe to  UDP: " << res.error().message() << std::endl;
        return 1;
    }

    std::cout << "Reactor started. Listening UDP:10001, Forwarding -> TCP:9000" << std::endl;

    // Run the loop with a timeout (e.g., 100ms)
    while (true) {
        // Your EventLoop::runOnce(int timeoutMs) requires an argument
        auto result = loop.runOnce(100);
        if (!result) {
            std::cerr << "Loop error: " << result.error().message() << std::endl;
            break;
        }
    }

    return 0;
}


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
