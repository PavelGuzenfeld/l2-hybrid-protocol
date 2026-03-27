# l2net

High-performance Layer 2 raw socket networking library for Linux.

## Overview

`l2net` enables user-space applications to bypass the Linux kernel's Transport (UDP/TCP) and Network (IP) layers, communicating directly via Ethernet frames. This provides deterministic latency, reduced packet overhead, and high throughput for industrial, embedded, and real-time applications.

### Features

- **Raw L2 Sockets** — Direct Ethernet frame transmission/reception via `AF_PACKET`
- **802.1Q VLAN Support** — Priority tagging (PCP) and VLAN segmentation
- **Zero-Copy Frame Building** — Builder pattern with `build_into()` for pre-allocated buffers
- **High-Performance IPC** — Local messaging over loopback with custom EtherType isolation
- **Hybrid Protocol** — TCP control plane + raw L2 data plane architecture
- **Remote Benchmarking** — SSH-based deployment and cross-network testing
- **Static Builds** — Portable binaries via musl/Alpine or static glibc

### Design Principles

- **No exceptions** — All error handling via `std::expected<T, error_code>`
- **RAII everywhere** — Sockets, SSH sessions, channels automatically cleaned up
- **Compile-time safety** — `constexpr`/`consteval` validation, `static_assert` on struct layout
- **Strict warnings** — `-Wall -Wextra -Wpedantic -Werror` with conversion and shadow warnings
- **Sanitizer-clean** — All tests pass under AddressSanitizer + UndefinedBehaviorSanitizer

## Requirements

- Linux kernel 4.x+
- C++23 compatible compiler (GCC 13+ or Clang 16+)
- CMake 3.21+
- Ninja (recommended)
- Root privileges for raw socket operations

Optional:
- libssl-dev (for SSH support — libssh is fetched automatically)
- Docker (for containerized builds and musl static builds)

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

### Docker Build

Build and test in an isolated environment with all dependencies:

```bash
# Build the dev image
docker build -t l2net-dev -f- . <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    g++-14 cmake ninja-build pkg-config \
    libssh-dev libssl-dev ca-certificates git \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
WORKDIR /src
COPY . .
RUN cmake --preset debug && cmake --build --preset debug
DOCKERFILE

# Run unit tests
docker run --rm l2net-dev ./build/debug/bin/l2net_unit_tests

# Run integration tests (requires --privileged for raw sockets)
docker run --rm --privileged l2net-dev ./build/debug/bin/l2net_integration_tests

# Run benchmarks
docker run --rm --privileged l2net-dev ./build/debug/bin/l2net_benchmarks
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
./build/debug/bin/l2net_unit_tests

# Integration tests (root required for raw sockets)
sudo ./build/debug/bin/l2net_integration_tests
```

Tests run with ASan + UBSan enabled by default in Debug builds.

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

### Loopback Performance

Comparison between `l2net` (Raw L2) and standard `SOCK_DGRAM` (UDP) on loopback interface.

**Test Environment:**
- **CPU:** 12th Gen Intel Core i7-12700H (20 vCPUs)
- **OS:** Linux 6.6 (WSL2 / Microsoft Hypervisor)
- **Memory:** 32 GB

| Payload Size | L2Net (Raw) | UDP (Standard) | Improvement |
|:-------------|:-----------:|:--------------:|:-----------:|
| Small (50 B) | **124.2 Mi/s** | 35.0 Mi/s | **3.55x** |
| Large (1400 B) | **2.19 Gi/s** | 0.95 Gi/s | **2.30x** |
| Jumbo (8000 B) | **8.01 Gi/s** | 4.54 Gi/s | **1.76x** |

| Metric | L2Net (Raw) | UDP (Standard) | Note |
|:-------|:-----------:|:--------------:|:-----|
| Roundtrip Latency | 990 ns | 936 ns | Comparable (Loopback) |
| Socket Creation | **16,244 us** | 1.2 us | Raw sockets are expensive to create |

### Remote Network Performance

Real network benchmarks between NVIDIA Jetson Xavier and Raspberry Pi over Ethernet and WiFi.

**Test Environment:**
- **Local:** NVIDIA Jetson Xavier (ARM64)
- **Remote (LAN):** Raspberry Pi via 100Mbps Ethernet
- **Remote (WiFi):** Raspberry Pi via WiFi
- **Packets per test:** 10,000

#### Latency (Round-Trip)

| Payload | Pi (LAN) | Pi (WiFi) | Notes |
|:--------|:-----------:|:----:|:------|
| 64 B    | 377 us (p99: 419 us) | 377 us (p99: 428 us) | Near-identical |
| 256 B   | 382 us (p99: 426 us) | 385 us (p99: 430 us) | |
| 512 B   | 390 us (p99: 428 us) | 392 us (p99: 431 us) | |
| 1024 B  | 417 us (p99: 484 us) | 413 us (p99: 467 us) | |
| 1400 B  | 429 us (p99: 474 us) | 431 us (p99: 466 us) | |

#### Throughput

| Payload | Pi (LAN) | Pi (WiFi) |
|:--------|:-----------:|:----:|
| 64 B    | 49 Mbps (78k pps) | 48 Mbps (78k pps) |
| 256 B   | 117 Mbps (54k pps) | 181 Mbps (84k pps) |
| 512 B   | 350 Mbps (83k pps) | 328 Mbps (78k pps) |
| 1024 B  | 437 Mbps (52k pps) | 638 Mbps (76k pps) |
| 1400 B  | 604 Mbps (53k pps) | 919 Mbps (81k pps) |

### Key Findings

1. **Massive Loopback Throughput:** Bypassing the kernel stack yields **3.5x speedup** for small packets where header processing dominates CPU time.
2. **Jumbo Frame Saturation:** With 8KB jumbo frames, the library achieves **8.0 Gi/s**, nearly saturating a theoretical 10Gb link.
3. **Consistent Remote Latency:** ~380-430 us average RTT with tight p99 (~10-15% above average).
4. **Zero Packet Loss:** All remote tests completed with 0% loss.
5. **WiFi vs 100Mbps Ethernet:** Pi WiFi outperformed 100Mbps wired link at larger payloads (919 vs 604 Mbps).
6. **Initialization Cost:** Creating a raw socket takes ~16ms vs ~1us for UDP. **Design Tip:** Initialize sockets at startup, never per-packet.

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

### Error Handling

All operations return `std::expected<T, error_code>` — no exceptions thrown:

```cpp
auto result = l2net::raw_socket::create_bound(iface);
if (!result.has_value()) {
    fmt::print(stderr, "Error: {}\n", result.error());
    return 1;
}
auto& socket = *result;
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
├── include/l2net/      # Public headers
│   ├── common.hpp      # Error codes, mac_address, byte utilities
│   ├── frame.hpp       # Frame building and parsing
│   ├── vlan.hpp        # 802.1Q VLAN support
│   ├── raw_socket.hpp  # Raw + TCP socket abstraction
│   ├── interface.hpp   # Network interface queries (ioctl/getifaddrs)
│   ├── ipc_channel.hpp # Local IPC over loopback
│   ├── hybrid_chat.hpp # TCP+L2 hybrid protocol endpoint
│   └── ssh_session.hpp # SSH session/pool for remote ops
├── src/                # Implementation
├── apps/               # Example applications
├── tests/              # Unit and integration tests (doctest)
├── bench/              # Benchmarks (nanobench)
├── cmake/              # CMake modules (musl toolchain)
└── scripts/            # Build and utility scripts
```

## License

MIT License — see [LICENSE](LICENSE) for details.

---

> **[Documentation and design notes on pavelguzenfeld.com](https://pavelguzenfeld.com/projects/l2-hybrid-protocol/)**
