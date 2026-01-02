# AtuReactor

**AtuReactor** is a lightweight, high-performance Linux-native C++17 library designed for low-latency UDP packet processing. It implements the **Reactor Pattern** to provide an efficient, event-driven architecture.



---

## 🚀 Key Features

* **Epoll-based Reactor**: High-efficiency asynchronous I/O multiplexing with O(1) scalability.
* **Batch UDP Reception**: Utilizes `recvmmsg` to pull multiple packets from the kernel in a single system call.
* **Resource Safety**: Full RAII implementation using `ScopedFd` to ensure system file descriptors are never leaked.
* **Precision Timers**: Native support for high-resolution timers via Linux `timerfd`.

---

## 💻 Quick Start

### 1. Implement a Handler
Inherit from `IPacketHandler` to define how your application processes incoming data.

    #include <atu_reactor/IPacketHandler.h>
    #include <iostream>

    class MyHandler : public atu_reactor::IPacketHandler {
    public:
        void handlePacket(const uint8_t data[], size_t size) override {
            std::cout << "Received packet of size: " << size << std::endl;
        }
    };

### 2. Run the Event Loop
This setup demonstrates the coordination between the Reactor (`EventLoop`) and the Handler.

    #include <atu_reactor/EventLoop.h>
    #include <atu_reactor/UDPReceiver.h>

    int main() {
        atu_reactor::EventLoop loop;
        atu_reactor::UDPReceiver receiver(loop);
        MyHandler handler;

        receiver.subscribe(8080, &handler);
        loop.run();

        return 0;
    }

---
## ⚙️ Building and Installing

To build the library, ensure you have **CMake** and a **C++17** compatible compiler installed.

    mkdir build && cd build
    cmake ..
    make
    sudo make install

---

## ⚖️ License

[cite_start]This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version[cite: 1].

---

