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
#include <atu_reactor/UDPReceiver.h>
#include <atu_reactor/EventLoop.h>

#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace atu_reactor;

// --- Mock Handler for Verification ---
class MockPacketHandler {
public:
    struct ReceivedPacket {
        std::vector<uint8_t> data;
        size_t size;
    };

    std::vector<ReceivedPacket> receivedPackets;

    void handlePacket(const uint8_t data[], size_t size) {
        receivedPackets.push_back({std::vector<uint8_t>(data, data + size), size});
    }

    // Static bridge function required by the new UDPReceiver API
    static void onPacket(void* context, const uint8_t* data, size_t len) {
        static_cast<MockPacketHandler*>(context)->handlePacket(data, len);
    }

    void clear() { receivedPackets.clear(); }
};

// --- Test Fixture ---
class UDPReceiverTest : public ::testing::Test {
protected:
    EventLoop loop;
    MockPacketHandler handler;
    const uint16_t TEST_PORT = 12345;

    // Helper to send raw UDP packets
    void sendUdpPacket(const std::vector<uint8_t>& data, uint16_t port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(sock, 0);

        struct sockaddr_in destAddr{};
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &destAddr.sin_addr);

        sendto(sock, data.data(), data.size(), 0,
               (struct sockaddr*)&destAddr, sizeof(destAddr));
        close(sock);
    }
};

// --- Test Cases ---

// 1. Verify Packet Reception
TEST_F(UDPReceiverTest, ReceivesLargePacketCorrectly) {
    UDPReceiver receiver(loop);
    auto result = receiver.subscribe(TEST_PORT, &handler, &MockPacketHandler::onPacket);
    ASSERT_TRUE(result.has_value()) << "Subscribe failed: " << result.error().message();

    // Create a packet larger than the batch size (64) but smaller than buffer (2048)
    // If the bug exists (iov_len = batchSize), this packet will be truncated to 64 bytes.
    std::string largePayload(1000, 'A'); // 1000 bytes of 'A'
    std::vector<uint8_t> packetData(largePayload.begin(), largePayload.end());

    sendUdpPacket(packetData, TEST_PORT);

    // Run loop briefly to process the packet
    loop.runOnce(100);

    ASSERT_EQ(handler.receivedPackets.size(), 1);
    EXPECT_EQ(handler.receivedPackets[0].size, 1000) << "Packet was truncated!";
    EXPECT_EQ(handler.receivedPackets[0].data, packetData);
}

// 2. Verify Batch Processing
TEST_F(UDPReceiverTest, HandlesBurstOfPackets) {
    ReceiverConfig config;
    config.batchSize = 10; // Configure small batch for testing
    UDPReceiver receiver(loop, config);
    auto result = receiver.subscribe(TEST_PORT, &handler, &MockPacketHandler::onPacket);
    ASSERT_TRUE(result.has_value());

    int packetCount = 5;
    for(int i=0; i < packetCount; ++i) {
        std::string msg = "Packet " + std::to_string(i);
        sendUdpPacket({msg.begin(), msg.end()}, TEST_PORT);
    }

    // Allow time for OS to buffer and loop to read
    // We might need a loop here in a real scenario, but runOnce usually clears the buffer
    loop.runOnce(100);

    EXPECT_EQ(handler.receivedPackets.size(), packetCount);
}

// New Test: Verify Error Handling for Duplicate Ports
TEST_F(UDPReceiverTest, ReturnsErrorOnDuplicatePort) {
    UDPReceiver receiver(loop);

    // First one succeeds
    auto res1 = receiver.subscribe(TEST_PORT, &handler, &MockPacketHandler::onPacket);
    ASSERT_TRUE(res1.has_value());

    // Second one should fail with EADDRINUSE
    auto res2 = receiver.subscribe(TEST_PORT, &handler, &MockPacketHandler::onPacket);
    ASSERT_FALSE(res2.has_value());
    EXPECT_EQ(res2.error().value(), EADDRINUSE);
}

// New Test: Verify Unsubscribe Result
TEST_F(UDPReceiverTest, UnsubscribeWorksCorrectly) {
    UDPReceiver receiver(loop);

    // Subscribe to a dynamic port (0)
    auto subRes = receiver.subscribe(0, &handler, &MockPacketHandler::onPacket);
    ASSERT_TRUE(subRes.has_value());
    uint16_t actualPort = static_cast<uint16_t>(subRes.value());

    // Unsubscribe and check Result<void>
    auto unsubRes = receiver.unsubscribe(actualPort);
    EXPECT_TRUE(unsubRes.has_value()) << "Unsubscribe failed: " << unsubRes.error().message();

    // Verify it's actually gone
    auto unsubRes2 = receiver.unsubscribe(actualPort);
    EXPECT_FALSE(unsubRes2.has_value());
    EXPECT_EQ(unsubRes2.error().value(), ENOENT);
}

// 3. Verify Lifecycle Safety (No crashes on scope exit)
TEST_F(UDPReceiverTest, SafeDestruction) {
    {
        UDPReceiver receiver(loop);
        auto result = receiver.subscribe(TEST_PORT, &handler, &MockPacketHandler::onPacket);
        ASSERT_TRUE(result.has_value());
    }

    // The loop should be clean. If the destructor didn't remove the source,
    // running the loop might crash or trigger use-after-free if events occur.
    loop.runOnce(10);
    SUCCEED(); // If we reached here without segfault, pass.
}
