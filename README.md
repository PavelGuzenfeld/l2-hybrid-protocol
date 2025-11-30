# l2net

High-performance Layer 2 raw socket networking library for Linux.

## Overview

`l2net` allows user-space applications to bypass the Linux kernel's Transport (UDP/TCP) and Network (IP) layers, communicating directly via Ethernet frames. This provides deterministic latency, reduced packet overhead, and high throughput for industrial or embedded applications.

## Build

```bash
cmake --preset release
cmake --build --preset release
```

## Test

```bash
# unit tests (no root required)
./build/release/l2net_unit_tests

# integration tests (root required)
sudo ./build/release/l2net_integration_tests
```

## Benchmark

```bash
sudo ./build/release/l2net_benchmarks
```

## Performance Results

Comparison between `l2net` (Raw L2) and standard `SOCK_DGRAM` (UDP).

**Test Environment:**

  * **CPU:** 12th Gen Intel Core i7-12700H (20 vCPUs)
  * **OS:** Linux 6.6 (WSL2 / Microsoft Hypervisor)
  * **Memory:** 32 GB

### Throughput Comparison

| Payload Size | L2Net (Raw) | UDP (Standard) | Improvement |
|:---|:---:|:---:|:---:|
| **Small (50 B)** | **124.2 Mi/s** | 35.0 Mi/s | **3.55x** |
| **Large (1400 B)** | **2.19 Gi/s** | 0.95 Gi/s | **2.30x** |
| **Jumbo (8000 B)** | **8.01 Gi/s** | 4.54 Gi/s | **1.76x** |

### Latency & Overhead

| Metric | L2Net (Raw) | UDP (Standard) | Note |
|:---|:---:|:---:|:---|
| **Roundtrip Latency** | 990 ns | 936 ns | Comparable (WSL2 Loopback) |
| **Socket Creation** | **16,244,056 ns** | 1,237 ns | Raw sockets are expensive to open. |

### Key Findings

1.  **Massive Throughput Gains:** Bypassing the kernel stack yields a **3.5x speedup** for small packets where header processing dominates the CPU time.
2.  **Jumbo Frame Saturation:** With 8KB jumbo frames, the library achieves **8.0 Gi/s**, nearly saturating a theoretical 10Gb link in pure software packet generation.
3.  **Initialization Cost:** Creating a raw socket takes \~16ms (vs 1µs for UDP). **Design Tip:** Always initialize your `l2net` sockets at startup; never create them per-packet.

## Applications

### 1\. Local IPC (High Speed Loopback)

Uses the loopback interface (`lo`) as a message bus, isolating traffic from the physical network.

```bash
# Receiver (Terminal 1)
sudo ./build/release/ipc_l2 recv

# Sender (Terminal 2)
sudo ./build/release/ipc_l2 send
```

### 2\. Hybrid Chat (Industrial Protocol Sim)

Simulates a control plane (TCP) + data plane (Raw L2) architecture.

```bash
# Machine A (Server)
sudo ./build/release/hybrid_chat eth0 server

# Machine B (Client)
sudo ./build/release/hybrid_chat eth0 client <SERVER_IP>
```

## Protocol Constants

| Protocol | EtherType |
|----------|-----------|
| Hybrid Data | `0x88B5` |
| IPC | `0xAAAA` |
| VLAN Tag | `0x8100` |

Default VLAN: ID `10`, Priority `7`
