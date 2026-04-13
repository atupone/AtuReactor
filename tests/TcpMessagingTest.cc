#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/TcpMessaging.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace atu_reactor;

TEST(TcpMessagingTest, HandlesCongestionWithBuffer) {
    EventLoop loop;
    TcpMessaging client(loop);

    // 1. Setup Server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);

    socklen_t addr_len = sizeof(addr);
    getsockname(server_fd, (struct sockaddr*)&addr, &addr_len);
    uint16_t port = ntohs(addr.sin_port);

    // 2. Start Connect
    client.connect("127.0.0.1", port, [](const uint8_t*, size_t) {});

    // 3. Accept the connection
    int accepted_fd = accept(server_fd, nullptr, nullptr);

    // Set non-blocking on the accepted side so our 'read' doesn't hang the test
    fcntl(accepted_fd, F_SETFL, fcntl(accepted_fd, F_GETFL, 0) | O_NONBLOCK);

    // 4. Force Congestion
    // We send a lot of data. Some goes to kernel, some to m_writeBuffer.
    std::string large_payload(100000, 'A');
    client.send(large_payload);

    // 5. Run the loop with a SMALL timeout and a limit
    // We run the loop a few times to allow the internal state to update
    for(int i = 0; i < 5; ++i) {
        loop.runOnce(10); // 10ms timeout
    }

    // 6. Consume data to "unclog" the pipe
    char sink[100000];
    ssize_t total_read = 0;
    ssize_t n;
    while ((n = read(accepted_fd, sink, sizeof(sink))) > 0) {
        total_read += n;
    }

    // 7. Run loop again so TcpMessaging can see the socket is writable again
    // and finish emptying m_writeBuffer
    for(int i = 0; i < 5; ++i) {
        loop.runOnce(10);
    }

    EXPECT_GT(total_read, 0);

    // Cleanup
    close(accepted_fd);
    close(server_fd);
    client.close();
}

TEST(TcpMessagingTest, EpollOutDoesNotSpinOnIdle) {
    EventLoop loop;
    TcpMessaging client(loop);

    // 1. Setup Local Server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);

    socklen_t addr_len = sizeof(addr);
    getsockname(server_fd, (struct sockaddr*)&addr, &addr_len);
    uint16_t port = ntohs(addr.sin_port);

    // 2. Connect the client
    client.connect("127.0.0.1", port, [](const uint8_t*, size_t) {});
    int accepted_fd = accept(server_fd, nullptr, nullptr);
    fcntl(accepted_fd, F_SETFL, fcntl(accepted_fd, F_GETFL, 0) | O_NONBLOCK);

    // Run the loop a couple of times to process the EPOLLOUT handshake
    loop.runOnce(10);
    loop.runOnce(10);

    // 3. Test Phase 1: Idle Connection
    // Now that we are connected, the loop should sleep because there is nothing to write.
    auto start = std::chrono::steady_clock::now();
    loop.runOnce(50); // Ask the loop to wait 50ms
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();

    // Allow a small margin of error for the OS scheduler (e.g., 40ms instead of 50ms)
    EXPECT_GE(elapsed, 40) << "Phase 1 Failed: EventLoop spun instantly! EPOLLOUT was not cleared after connect.";

    // 4. Test Phase 2: Drained Buffer
    // Send a tiny bit of data to re-trigger EPOLLOUT
    client.send("Ping");
    loop.runOnce(10); // Process the write and clear the buffer

    // Read the data on the server side to clear the pipe
    char buf[16];
    read(accepted_fd, buf, sizeof(buf));

    // Now the client buffer is empty again. Ensure it goes back to sleep.
    start = std::chrono::steady_clock::now();
    loop.runOnce(50);
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();

    EXPECT_GE(elapsed, 40) << "Phase 2 Failed: EventLoop spun instantly! EPOLLOUT was not cleared after draining buffer.";

    // Cleanup
    close(accepted_fd);
    close(server_fd);
    client.close();
}
