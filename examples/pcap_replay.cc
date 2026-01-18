#include <atu_reactor/EventLoop.h>
#include <atu_reactor/PcapReceiver.h>
#include <iostream>

using namespace atu_reactor;

void onPacket(void*, const uint8_t*, size_t size, uint32_t, struct timespec ts) {
    std::cout << "[Replay] Got " << size << " bytes at PCAP time " << ts.tv_sec << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./pcap_replay <file.pcap>" << std::endl;
        return 1;
    }

    EventLoop loop;

    PcapConfig config;
    config.mode = ReplayMode::TIMED; // Replay at original speed
    // config.speedMultiplier = 2.0; // Uncomment for 2x speed

    PcapReceiver player(loop, config);

    if (auto res = player.open(argv[1]); !res) {
        std::cerr << "Failed to open PCAP: " << res.error().message() << std::endl;
        return 1;
    }

    // Subscribe to port 5001 (assuming traffic in PCAP is on 5001)
    auto res = player.subscribe(5001, nullptr, onPacket);
    if (!res) {
        std::cerr << "Failed to subscribe: " << res.error().message() << std::endl;
        return 1;
    }

    std::cout << "Starting Replay..." << std::endl;
    player.start();

    // Run loop
    while (true) {
        loop.runOnce(1000).value();
    }

    return 0;
}
