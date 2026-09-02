# AtuReactor

**AtuReactor** is a lightweight, high-performance Linux-native C++20 library designed for low-latency networking and packet processing. It implements the **Reactor Pattern** to provide an efficient, event-driven architecture for live UDP/TCP streams and PCAP replay processing.

---

## 🚀 Key Features

* **C++17 Native**: Modern codebase utilizing `[[likely]]` / `[[unlikely]]` branching hints.
* **Epoll-based Reactor**: High-efficiency asynchronous I/O multiplexing with O(1) scalability for stream and packet sources.
* **Batch UDP Reception**: Utilizes `recvmmsg` to pull multiple packets from the kernel in a single system call.
* **TCP Stream Support**: Integrated `TcpMessaging` class with non-blocking async connect, graceful buffer draining, and zero-copy view dispatch.
* **Hugepage Support**: Supports `MAP_HUGETLB` via `mmap` to reduce TLB misses and improve deterministic performance under high load.
* **Precision Kernel Timestamps**: Native support for nanosecond-precision timestamps via `SO_TIMESTAMPNS` and `SO_TIMESTAMPING`.
* **PCAP / PCAPNG Engine**: Native support for replaying legacy `.pcap` and modern `.pcapng` files with interface resolution and TIMED/FLOOD mode execution.
* **Dual-Stack IPv6 Support**: Automatically handles both IPv4 and IPv6 traffic on the same port using a single subscription.
* **Safety & Robustness**: Reports kernel-level events like packet truncation (`MSG_TRUNC`) via a status bitmask.
* **Cache-Aligned Buffering**: Uses a single contiguous flat buffer with 64-byte alignment to match CPU cache lines.
* **Resource Safety**: Full RAII implementation using `ScopedFd` to ensure descriptors are never leaked.
* **Precision Timers**: Native support for high-resolution timers via Linux `timerfd`.

---

## ⚡ Advanced Optimization: Hugepages (`MAP_HUGETLB`)

AtuReactor supports **Hugepages** for its internal packet buffers. By using 2MB pages instead of the standard 4KB pages, the CPU's Translation Lookaside Buffer (TLB) can cover a much larger memory area with fewer entries.

### 1. Enable Hugepages in the Linux Kernel
```bash
# Reserve 512 pages (1GB of RAM for 2MB pages)
sudo sysctl -w vm.nr_hugepages=512
```

### 2. How it works in AtuReactor
When `UDPReceiver` is initialized, it calculates the required memory for your `batchSize` and `bufferSize` and attempts to map it using `MAP_HUGETLB`.
* **Success**: The packet buffer is backed by 2MB physical pages.
* **Fallback**: If hugepages are unavailable, it falls back to standard 4KB pages, ensuring functionality.

---

## ⏱️ Kernel-Level Timestamps

AtuReactor provides robust support for nanosecond-precision timestamps to eliminate user-space jitter.

### Implementation Details
* **Dual API Support**: The library parses both `SCM_TIMESTAMPNS` and `SCM_TIMESTAMPING` ancillary data.
* **Metadata Persistence**: AtuReactor explicitly resets `msg_controllen` and `iov_len` before every batch read. This prevents the kernel from shrinking the metadata buffer, ensuring stable timestamp delivery across packet bursts.
* **Control Message Buffering**: Uses pre-allocated, appropriately sized buffers (`CMSG_SPACE`) to store multiple `timespec` structures provided by the kernel.

---

## 💻 Quick Start

### 1. Live UDP Reception
```cpp
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/UDPReceiver.h>
#include <iostream>

void onPacketReceived(void* context, std::span<const uint8_t> payload, uint32_t status, struct timespec ts) {
    if (ts.tv_sec > 0) {
        std::cout << "Kernel Timestamp: " << ts.tv_sec << "." << ts.tv_nsec << std::endl;
    }
    std::cout << "Received " << payload.size() << " bytes" << std::endl;
}

int main() {
    atu_reactor::EventLoop loop;
    atu_reactor::UDPReceiver receiver(loop);

    auto result = receiver.subscribe(12345, nullptr, onPacketReceived);
    if (result) {
        while (true) {
            loop.runOnce(1000); 
        }
    }
    return 0;
}
```

### 2. Asynchronous TCP Messaging
```cpp
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/TcpMessaging.h>
#include <iostream>

int main() {
    atu_reactor::EventLoop loop;
    atu_reactor::TcpMessaging client(loop);

    client.connect("127.0.0.1", 8080, [](std::span<const uint8_t> data) {
        std::cout << "Received " << data.size() << " bytes from server\n";
    });

    // Queue string_view or std::span data safely
    client.send("Hello, Reactor!");

    while (true) {
        loop.runOnce(1000);
    }
    return 0;
}
```

### 3. PCAP / PCAPNG Replay Usage
The `PcapReceiver` processes `.pcap` and `.pcapng` files using the same callback logic as live receivers.

```cpp
#include <atu_reactor/PcapReceiver.h>
#include <atu_reactor/EventLoop.h>

int main() {
    atu_reactor::EventLoop loop;
    
    atu_reactor::PcapConfig config{
        .mode = atu_reactor::ReplayMode::TIMED,
        .speedMultiplier = 1.0,
        .batchSize = 1000
    };
    
    atu_reactor::PcapReceiver reader(loop, config);
    if (!reader.open("capture.pcapng")) {
        return 1;
    }

    reader.subscribe(12345, nullptr, onPacketReceived);
    reader.start();
    
    while (!reader.isFinished()) {
        loop.runOnce(100);
    }
    return 0;
}
```

---

## 🛡️ Performance & Threading

To achieve maximum deterministic performance, AtuReactor components are **Thread-Hostile**:

* **No Internal Locking**: Eliminates mutex contention overhead to maintain a low-latency hot path.
* **Thread Safety**: All methods must be executed on the same thread that constructed the receiver or event loop.
* **Safety Assertions**: The library uses thread ID tracking to assert that I/O operations occur on the correct owner thread.

---

## ⚙️ Building

Requires a C++20 compliant compiler (GCC 10+, Clang 11+).

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## ⚖️ License

**AtuReactor** is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License v3.0**.
