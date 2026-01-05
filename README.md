# AtuReactor

**AtuReactor** is a lightweight, high-performance Linux-native C++17 library designed for low-latency UDP packet processing. It implements the **Reactor Pattern** to provide an efficient, event-driven architecture.

---

## 🚀 Key Features

* **Epoll-based Reactor**: High-efficiency asynchronous I/O multiplexing with $O(1)$ scalability.
* **Batch UDP Reception**: Utilizes `recvmmsg` to pull multiple packets from the kernel in a single system call.
* **Dual-Stack IPv6 Support**: Automatically handles both IPv4 and IPv6 traffic on the same port using a single subscription.
* **Cache-Aligned Buffering**: Uses a single contiguous flat buffer with 64-byte alignment to improve cache locality.
* **Resource Safety**: Full RAII implementation using `ScopedFd` to ensure descriptors are never leaked.
* **Precision Timers**: Native support for high-resolution timers via Linux `timerfd`.

---

## 💻 Quick Start

### 1. Define a Callback
AtuReactor uses a C-style function pointer for packet handling to minimize abstraction overhead.

```cpp
#include <atu_reactor/UDPReceiver.h>
#include <iostream>

// Callback signature: void(*)(void* context, const uint8_t* data, size_t len)
void onPacketReceived(void* context, const uint8_t* data, size_t size) {
    std::cout << "Received " << size << " bytes" << std::endl;
}
```
### 2. Run the Event Loop
Register your handler. The `subscribe` method now supports Dual-Stack, meaning it listens on both IPv4 (`0.0.0.0`) and IPv6 (`::`) by default.

```cpp
#include <atu_reactor/EventLoop.h>
#include <atu_reactor/UDPReceiver.h>

int main() {
    atu_reactor::EventLoop loop;
    atu_reactor::UDPReceiver receiver(loop);

    // Subscribe to port 8080. Returns Result<int>
    // This will accept traffic from both IPv4 (127.0.0.1) and IPv6 (::1)
    auto result = receiver.subscribe(8080, nullptr, onPacketReceived);
    
    if (!result) {
        std::cerr << "Error: " << result.error().message() << std::endl;
        return 1;
    }

    while (true) {
        loop.runOnce(-1);
    }

    return 0;
}
```
### 3. Using Timers
The reactor supports scheduling tasks directly within the event loop without managing separate threads. All timers are managed using Linux `timerfd` for high precision.

```cpp
// Run a callback once after a 500ms delay
loop.runAfter(std::chrono::milliseconds(500), []() {
    std::cout << "One-shot timer expired" << std::endl;
});

// Run a callback periodically every 1 second
loop.runEvery(std::chrono::seconds(1), []() {
    std::cout << "Periodic heartbeat" << std::endl;
});
```

---

## 🧪 Testing Dual-Stack
You can verify the dual-stack functionality using standard Linux tools:

```bash
# Send a packet via IPv4
echo "test" | nc -u 127.0.0.1 8080

# Send a packet via IPv6
echo "test" | nc -u ::1 8080
```

---

## ⚠️ Threading Model

**AtuReactor is Thread-Hostile by design**.

To achieve maximum performance, the library avoids internal locking and utilizes shared memory structures.

* **Single-Threaded Execution**: All interactions with an `EventLoop` or `UDPReceiver` instance must occur on the same thread that constructed the object.
* **Safety Checks**: Debug builds use `assert` statements to prevent cross-thread access.
* **Scalability**: For multi-core utilization, instantiate one `EventLoop` and one `UDPReceiver` per thread.

> **Note**: Sharing a `UDPReceiver` across multiple `EventLoop` instances will cause immediate data corruption in the packet buffers.

---



---

## ⚙️ Building and Installing

To build the library, ensure you have **CMake 3.10+** and a **C++17** compatible compiler (like GCC 8+ or Clang 7+) installed on a Linux system.

```bash
# 1. Clone the repository
git clone [https://github.com/your-repo/AtuReactor.git](https://github.com/your-repo/AtuReactor.git)
cd AtuReactor

# 2. Create a build directory
mkdir build && cd build

# 3. Configure and build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 4. Install headers and library (optional)
sudo make install
```
---

## ⚖️ License

**AtuReactor** is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License** as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the implied warranty of **MERCHANTABILITY** or **FITNESS FOR A PARTICULAR PURPOSE**. See the [GNU General Public License](https://www.gnu.org/licenses/) for more details.

Copyright (C) 2026 Alfredo Tupone.

---

