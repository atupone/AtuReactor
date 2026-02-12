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
#include <iomanip>

using namespace atu_reactor;

struct ReplayContext {
    bool quiet = false;
    uint64_t totalPackets = 0;
    uint64_t totalBytes = 0;
};

void onPacket(void* context, const uint8_t*, size_t size, uint32_t, struct timespec ts) {
    auto* ctx = static_cast<ReplayContext*>(context);
    ctx->totalPackets++;
    ctx->totalBytes += size;
    if (!ctx->quiet) {
        std::cout << "[Replay] Got " << size << " bytes at PCAP time " << ts.tv_sec << std::endl;
    }
}

int main(int argc, char** argv) {
    bool floodMode = false;
    int iterations = 1;
    uint16_t targetPort = 5001; // Default port
    ReplayContext replayCtx;
    int opt;

    // Parse options: -f for flood, -n for number of iterations, -p for port, -q for quiet
    while ((opt = getopt(argc, argv, "fn:p:q")) != -1) {
        switch (opt) {
            case 'f':
                floodMode = true;
                break;
            case 'n':
                iterations = std::atoi(optarg);
                break;
            case 'p':
                targetPort = static_cast<uint16_t>(std::atoi(optarg));
                break;
            case 'q':
                replayCtx.quiet = true;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [-f] [-n iterations] [-p port] <file.pcap>" << std::endl;
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

    // Use the port provided by the user
    auto res = player.subscribe(targetPort, &replayCtx, onPacket);
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
    }

    // Print final summary stats
    std::cout << "\n--- Replay Complete ---" << std::endl;
    std::cout << "Total Packets Processed: " << replayCtx.totalPackets << std::endl;
    std::cout << "Total Bytes Processed:   " << replayCtx.totalBytes << " ("
        << (replayCtx.totalBytes / 1024 / 1024) << " MB)" << std::endl;

    return 0;
}


// Local Variables: ***
// mode: C++ ***
// tab-width: 4 ***
// c-basic-offset: 4 ***
// indent-tabs-mode: nil ***
// End: ***
// ex: shiftwidth=4 tabstop=4
