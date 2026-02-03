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

#include <atu_reactor/EventLoop.h>
#include <atu_reactor/PcapReceiver.h>
#include <iostream>
#include <unistd.h>

using namespace atu_reactor;

void onPacket(void*, const uint8_t*, size_t size, uint32_t, struct timespec ts) {
    std::cout << "[Replay] Got " << size << " bytes at PCAP time " << ts.tv_sec << std::endl;
}

int main(int argc, char** argv) {
    bool floodMode = false;
    int iterations = 1;
    int opt;

    // Parse options: -f for flood, -n for number of iterations
    while ((opt = getopt(argc, argv, "fn:")) != -1) {
        switch (opt) {
            case 'f':
                floodMode = true;
                break;
            case 'n':
                iterations = std::atoi(optarg);
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-f] [-n iterations] <file.pcap>" << std::endl;
                return 1;
        }
    }

    if (optind >= argc) {
        std::cerr << "Expected PCAP file path after options." << std::endl;
        return 1;
    }

    const char* pcapFile = argv[optind];
    EventLoop loop;

    PcapConfig config;
    config.mode = floodMode ? ReplayMode::FLOOD : ReplayMode::TIMED;
    // config.speedMultiplier = 2.0; // Uncomment for 2x speed

    PcapReceiver player(loop, config);

    std::cout << "Starting Replay..." << std::endl;
    std::cout << "Mode: " << (floodMode ? "FLOOD" : "TIMED") << std::endl;
    std::cout << "Iterations: " << iterations << std::endl;

    if (auto res = player.open(pcapFile); !res) {
        std::cerr << "Failed to open PCAP: " << res.error().message() << std::endl;
        return 1;
    }

    // Subscribe to port 5001 (assuming traffic in PCAP is on 5001)
    auto res = player.subscribe(5001, nullptr, onPacket);
    if (!res) {
        std::cerr << "Failed to subscribe: " << res.error().message() << std::endl;
        return 1;
    }

    for (int i = 0; i < iterations; ++i) {
        player.rewind();
        player.start();

        // Run loop
        while (!player.isFinished()) {
            loop.runOnce(floodMode ? 0 : 1).value();
        }

        if (iterations > 1 && (i + 1) % 100 == 0) {
            std::cout << "Completed " << (i + 1) << " iterations..." << std::endl;
        }
    }

    std::cout << "Finished all iterations." << std::endl;
    return 0;
}


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
