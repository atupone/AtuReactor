# AtuReactor

**AtuReactor** is a lightweight, high-performance Linux-native C++17 library designed for low-latency UDP packet processing. It implements the **Reactor Pattern** to provide an efficient, event-driven architecture.

---

## 🚀 Key Features

* **Epoll-based Reactor**: High-efficiency asynchronous I/O multiplexing with $O(1)$ scalability.
* **Batch UDP Reception**: Utilizes `recvmmsg` to pull multiple packets from the kernel in a single system call.
* **Hugepage Support**: Supports `MAP_HUGETLB` to reduce TLB misses and improve deterministic performance under high load.
* **Dual-Stack IPv6 Support**: Automatically handles both IPv4 and IPv6 traffic on the same port using a single subscription.
* **Safety & Robustness**: Reports kernel-level events like packet truncation (`MSG_TRUNC`) via a status bitmask.
* **Cache-Aligned Buffering**: Uses a single contiguous flat buffer with 64-byte alignment to match CPU cache lines.
* **Resource Safety**: Full RAII implementation using `ScopedFd` to ensure descriptors are never leaked.
* **Precision Timers**: Native support for high-resolution timers via Linux `timerfd`.

---

## ⚡ Advanced Optimization: Hugepages (`MAP_HUGETLB`)

AtuReactor supports **Hugepages** for its internal packet buffers. By using 2MB pages instead of the standard 4KB pages, the CPU's Translation Lookaside Buffer (TLB) can cover a much larger memory area with fewer entries. This significantly reduces "TLB Misses" and jitter during high-throughput packet bursts.



### 1. Enable Hugepages in the Linux Kernel
Hugepages must be pre-allocated by the operating system because they require contiguous physical memory that cannot be swapped out.

**Check current status:**
```bash
grep Huge /proc/meminfo
```
*Look for `HugePages_Total` (usually 0) and `Hugepagesize` (usually 2048 kB).*

**Reserve 512 pages (1GB of RAM for 2MB pages):**
```bash
# Temporary (lost after reboot)
sudo sysctl -w vm.nr_hugepages=512

# Permanent
echo "vm.nr_hugepages = 512" | sudo tee -a /etc/sysctl.conf
```

### 2. How it works in AtuReactor
When `UDPReceiver` is initialized, it calculates the required memory for your `batchSize` and `bufferSize` and attempts to map it using the `MAP_HUGETLB` flag.



* **Success**: The packet buffer is backed by 2MB physical pages.
* **Fallback**: If no hugepages are reserved in the kernel or the allocation fails, the library will automatically fall back to standard 4KB pages via a regular `mmap` call, ensuring the application remains functional.

### 3. Verification
You can verify if AtuReactor is successfully using the reserved pages while the application is running by watching the free page count:
```bash
watch -n1 "grep HugePages_Free /proc/meminfo"
```
If the number of free pages drops when your application starts and returns when it stops, Hugepage integration is active.

---

## 💻 Quick Start

### 1. Define a Callback
AtuReactor uses a C-style function pointer for packet handling to minimize abstraction overhead. The callback receives a `status` bitmask to check for errors like truncation.

```cpp
#include <iostream>
#include <atu_reactor/UDPReceiver.h>
#include <atu_reactor/Types.h>

// Callback signature: void(*)(void* context, const uint8_t* data, size_t len, uint32_t status)
void onPacketReceived(void* context, const uint8_t* data, size_t size, uint32_t status) {
    if (status & atu_reactor::PacketStatus::TRUNCATED) {
        std::cerr << "[Warning] Packet truncated by kernel!" << std::endl;
    }
    std::cout << "Received " << size << " bytes" << std::endl;
}
```

### 2. Run the Event Loop
Register your handler. The `subscribe` method supports Dual-Stack, meaning it listens on both IPv4 and IPv6 simultaneously.

```cpp
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/UDPReceiver.h>

int main() {
    atu_reactor::EventLoop loop;
    atu_reactor::UDPReceiver receiver(loop);

    // Register handler for UDP port 12345
    auto result = receiver.subscribe(12345, nullptr, onPacketReceived);
    
    if (result) {
        while (true) {
            loop.runOnce(1000); // Poll with 1s timeout
        }
    }
    return 0;
}
```

---

## 🛡️ Robustness & Packet Integrity

AtuReactor is designed for mission-critical data. Starting with version **0.0.5**, the library provides kernel-level feedback for every packet via a status bitmask.

### Handling Truncation
If a physical UDP packet exceeds your configured `bufferSize`, the Linux kernel discards the overflowing bytes. AtuReactor captures the `MSG_TRUNC` flag and passes it to your callback as `PacketStatus::TRUNCATED`.



**Usage:**
```cpp
#include <atu_reactor/Types.h>

void onPacket(void* context, const uint8_t* data, size_t len, uint32_t status) {
    if (status & atu_reactor::PacketStatus::TRUNCATED) {
        std::cerr << "Received incomplete data (" << len << " bytes received)" << std::endl;
        return; 
    }
    process(data, len);
}
```

---

## ⏱️ Precision Timers

AtuReactor provides high-resolution timers using the Linux `timerfd` API, ensuring that timer expirations are handled with the same efficiency as network I/O.



### Periodic and One-Shot Timers
```cpp
atu_reactor::EventLoop loop;

// Example 1: Periodic Heartbeat (every 1 second)
loop.runEvery(std::chrono::seconds(1), []() {
    std::cout << "Heartbeat pulse..." << std::endl;
});

// Example 2: One-shot delayed task (triggers once after 500ms)
loop.runAfter(std::chrono::milliseconds(500), []() {
    std::cout << "Delayed task executed." << std::endl;
});
```

### Timer Cancellation
Every timer registration returns a `TimerId` which can be used to cancel the timer before it fires.
```cpp
auto res = loop.runAfter(std::chrono::seconds(10), []() { /* ... */ });
if (res) {
    atu_reactor::TimerId id = res.value();
    loop.cancelTimer(id); // Timer will no longer fire
}
```

---

## ⚡ Performance & Threading

To achieve maximum deterministic performance, AtuReactor follows a **single-threaded execution model**:

* **No Internal Locking**: Eliminates mutex contention overhead.
* **Thread Affinity**: Recommended to pin the `EventLoop` thread to a specific CPU core.
* **Thread Safety**: All methods must be called from the same thread that constructed the object.
* **Scalability**: For multi-core utilization, instantiate one `EventLoop` and one `UDPReceiver` per thread.

---

## ⚙️ Building and Installing

```bash
# 1. Clone the repository
git clone [https://github.com/your-repo/AtuReactor.git](https://github.com/your-repo/AtuReactor.git)
cd AtuReactor

# 2. Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 3. Install (optional)
sudo make install
```

---

## ⚖️ License

**AtuReactor** is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of **MERCHANTABILITY** or **FITNESS FOR A PARTICULAR PURPOSE**. See the [GNU General Public License](https://www.gnu.org/licenses/) for more details.
