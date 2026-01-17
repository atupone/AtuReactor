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
#include <atu_reactor/UDPReceiver.h>
#include <iostream>

using namespace atu_reactor;

class GenericHandler {
    std::string m_label;
public:
    explicit GenericHandler(std::string label) : m_label(std::move(label)) {}

    void process(const uint8_t*, size_t size, uint32_t status) {
        std::cout << "[Channel: " << m_label << "] Received " << size << " bytes.";
        if (status & PacketStatus::TRUNCATED) {
            std::cout << " (TRUNCATED)";
        }
        std::cout << std::endl;
    }

    // Static bridge function
    static void bridge(void* context, const uint8_t* data, size_t len, uint32_t status, struct timespec) {
        static_cast<GenericHandler*>(context)->process(data, len, status);
    }
};

int main() {
    try {
        EventLoop loop;
        UDPReceiver receiver(loop);

        // Use generic names for different data streams
        GenericHandler streamA("PRIMARY_RADAR");
        GenericHandler streamB("SECONDARY_DATA");

        auto resA = receiver.subscribe(5001, &streamA, &GenericHandler::bridge);
        if (!resA) {
            std::cerr << "Stream A failed: " << resA.error().message() << std::endl;
        }

        auto resB = receiver.subscribe(5002, &streamB, &GenericHandler::bridge);
        if (!resB) {
            std::cerr << "Stream B failed: " << resB.error().message() << std::endl;
        }

        if (resA || resB) {
            std::cout << "Monitoring streams on IPv4/IPv6 dual-stack." << std::endl;
            while (true) {
                loop.runOnce(-1).value();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
