/*
 * Copyright (C) 2026 Alfredo Tupone
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This example is part of the AtuReactor project.
 */

#include <atu_reactor/EventLoop.h>    //
#include <atu_reactor/UDPReceiver.h>  //
#include <atu_reactor/IPacketHandler.h> //
#include <iostream>

using namespace atu_reactor;

// A simple handler that just prints packet info
class EchoHandler : public IPacketHandler {
public:
    void handlePacket(const uint8_t* data, size_t size) override {
        std::cout << "Received " << size << " bytes" << std::endl;
        // You could process data here
    }
};

int main() {
    try {
        EventLoop loop;               //
        UDPReceiver receiver(loop);   //
        EchoHandler myHandler;

        uint16_t port = 12345;

        // Register our handler for UDP port 12345
        auto result = receiver.subscribe(port, &myHandler);

        if (!result) {
            std::cerr << "Failed to start Echo Server: "
                      << result.error().message() << std::endl;
        }

        std::cout << "Starting Echo Server on port " << port << "..." << std::endl;

        // Run the reactor loop
        while (true) {
            loop.runOnce(1000); // 1 second timeout
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
