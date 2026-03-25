#include <gtest/gtest.h>
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/TcpMessaging.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

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
