# l2net

High-performance Layer 2 raw socket networking library for Linux.

## Overview

`l2net` enables user-space applications to bypass the Linux kernel's Transport (UDP/TCP) and Network (IP) layers, communicating directly via Ethernet frames. This provides deterministic latency, reduced packet overhead, and high throughput for industrial, embedded, and real-time applications.

### Features

- **Raw L2 Sockets** - Direct Ethernet frame transmission/reception
- **802.1Q VLAN Support** - Priority tagging and VLAN segmentation
- **High-Performance IPC** - Local messaging over loopback interface
- **Hybrid Protocol Simulation** - TCP control plane + raw L2 data plane
- **Remote Benchmarking** - SSH-based deployment and testing across networks
- **Static Builds** - Portable binaries via musl/Alpine or static glibc

## Requirements

- Linux kernel 4.x+
- C++23 compatible compiler (GCC 13+ or Clang 16+)
- CMake 3.21+
- Ninja (recommended)
- Root privileges for raw socket operations

Optional:
- libssl-dev (for SSH support - libssh is fetched automatically)
- Docker (for musl static builds)

## Build

### Quick Start

```bash
# Debug build with sanitizers
cmake --preset debug
cmake --build --preset debug

# Release build
cmake --preset release
cmake --build --preset release
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `L2NET_BUILD_TESTS` | ON | Build test suite |
| `L2NET_BUILD_BENCHMARKS` | ON | Build benchmarks |
| `L2NET_BUILD_APPS` | ON | Build example applications |
| `L2NET_ENABLE_SANITIZERS` | ON | Enable ASan/UBSan for tests |
| `L2NET_FETCH_LIBSSH` | ON | Fetch and build libssh (requires libssl-dev) |
| `L2NET_USE_MUSL` | OFF | Build with musl libc (requires musl system) |
| `L2NET_STATIC` | OFF | Build fully static binaries with glibc |

### SSH Support

SSH support (for remote benchmarking) requires OpenSSL development headers. libssh is fetched and built automatically.

```bash
# Debian/Ubuntu
apt install libssl-dev

# Alpine
apk add openssl-dev

# Or use system libssh instead of fetching
apt install libssh-dev
cmake -B build -DL2NET_FETCH_LIBSSH=OFF
```

### Static Builds

For portable static binaries, use Docker with Alpine:

```bash
./scripts/build_musl.sh
# Output: build/musl-alpine/bin/
```

Or build static with glibc:

```bash
cmake -B build/static \
    -DL2NET_STATIC=ON \
    -DL2NET_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/static
```

## Test

```bash
# Unit tests (no root required)
./build/release/bin/l2net_unit_tests

# Integration tests (root required)
sudo ./build/release/bin/l2net_integration_tests
```

## Benchmark

```bash
# Local benchmarks
sudo ./build/release/bin/l2net_benchmarks

# Remote benchmarks (requires SSH access to remote host)
# The script automatically configures sudoers on the remote host
sudo ./scripts/run_remote_benchmark.sh \
    -h 192.168.1.100 \
    -i eth0 \
    -u admin \
    -p
```

## Performance Results

Comparison between `l2net` (Raw L2) and standard `SOCK_DGRAM` (UDP).

**Test Environment:**
- **CPU:** 12th Gen Intel Core i7-12700H (20 vCPUs)
- **OS:** Linux 6.6 (WSL2 / Microsoft Hypervisor)
- **Memory:** 32 GB

### Throughput Comparison

| Payload Size | L2Net (Raw) | UDP (Standard) | Improvement |
|:-------------|:-----------:|:--------------:|:-----------:|
| Small (50 B) | **124.2 Mi/s** | 35.0 Mi/s | **3.55x** |
| Large (1400 B) | **2.19 Gi/s** | 0.95 Gi/s | **2.30x** |
| Jumbo (8000 B) | **8.01 Gi/s** | 4.54 Gi/s | **1.76x** |

### Latency & Overhead

| Metric | L2Net (Raw) | UDP (Standard) | Note |
|:-------|:-----------:|:--------------:|:-----|
| Roundtrip Latency | 990 ns | 936 ns | Comparable (Loopback) |
| Socket Creation | **16,244 µs** | 1.2 µs | Raw sockets are expensive to create |

### Key Findings

1. **Massive Throughput Gains:** Bypassing the kernel stack yields a **3.5x speedup** for small packets where header processing dominates CPU time.
2. **Jumbo Frame Saturation:** With 8KB jumbo frames, the library achieves **8.0 Gi/s**, nearly saturating a theoretical 10Gb link.
3. **Initialization Cost:** Creating a raw socket takes ~16ms vs ~1µs for UDP. **Design Tip:** Initialize sockets at startup, never per-packet.

## Applications

### 1. Local IPC (High-Speed Loopback)

Uses the loopback interface (`lo`) as a message bus, isolating traffic from the physical network.

```bash
# Terminal 1 - Receiver
sudo ./build/release/bin/ipc_l2 recv

# Terminal 2 - Sender
sudo ./build/release/bin/ipc_l2 send
```

### 2. Hybrid Chat (Industrial Protocol Simulation)

Simulates a control plane (TCP) + data plane (Raw L2) architecture common in industrial protocols.

```bash
# Machine A (Server)
sudo ./build/release/bin/hybrid_chat eth0 server

# Machine B (Client)
sudo ./build/release/bin/hybrid_chat eth0 client <SERVER_IP>
```

### 3. Remote Benchmarking

Deploy and run benchmarks across physical networks:

```bash
# Deploy l2net_remote_node to remote host and run latency/throughput tests
sudo ./build/release/bin/l2net_remote_benchmark \
    --remote-host 192.168.1.100 \
    --ssh-user admin \
    --ssh-key ~/.ssh/id_rsa \
    --local-iface eth0 \
    --remote-iface eth0 \
    --binary ./build/release/bin/l2net_remote_node \
    --payload-sizes 64,256,1024,1400,4096,8192 \
    --packets 10000 \
    --output results
```

## Library Usage

### Basic Frame Construction

```cpp
#include <l2net/frame.hpp>
#include <l2net/raw_socket.hpp>

// Create and bind socket
auto sock = l2net::raw_socket::create_bound(iface);

// Build frame
auto frame = l2net::build_simple_frame(
    dest_mac,
    src_mac,
    0x88B5,  // EtherType
    payload
);

// Send
sock->send_raw(*frame, iface);
```

### VLAN Tagged Frames

```cpp
#include <l2net/vlan.hpp>

l2net::vlan_tci tci{
    .priority = 7,    // Highest priority
    .dei = false,
    .vlan_id = 10
};

auto frame = l2net::build_vlan_frame(
    dest_mac, src_mac, tci,
    0x88B5,  // Inner EtherType
    payload
);
```

### IPC Channel

```cpp
#include <l2net/ipc_channel.hpp>

auto channel = l2net::ipc_channel::create();

// Send
channel->send("message");

// Receive with timeout
auto msg = channel->receive_with_timeout(std::chrono::milliseconds{100});
```

## Protocol Constants

| Protocol | EtherType |
|----------|-----------|
| Hybrid Data | `0x88B5` |
| IPC | `0xAAAA` |
| Benchmark | `0xBEEF` |
| VLAN Tag | `0x8100` |

Default VLAN: ID `10`, Priority `7`

## Project Structure

```
l2net/
├── include/l2net/     # Public headers
│   ├── common.hpp     # Error codes, mac_address, utilities
│   ├── frame.hpp      # Frame building and parsing
│   ├── vlan.hpp       # 802.1Q VLAN support
│   ├── raw_socket.hpp # Socket abstraction
│   ├── interface.hpp  # Network interface queries
│   ├── ipc_channel.hpp# Local IPC
│   ├── hybrid_chat.hpp# TCP+L2 hybrid protocol
│   └── ssh_session.hpp# SSH for remote ops
├── src/               # Implementation
├── apps/              # Example applications
├── tests/             # Unit and integration tests
├── bench/             # Benchmarks
├── cmake/             # CMake modules
└── scripts/           # Build and utility scripts
```

## License

MIT License - see [LICENSE](LICENSE) for details.