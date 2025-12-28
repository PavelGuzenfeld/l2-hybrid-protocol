# l2net Roadmap

This document outlines the development plan for `l2net`, moving from performance optimizations to industrial feature expansion, and culminating in a custom ROS 2 Middleware (RMW) implementation.

## Phase 1: Performance & Zero-Copy Architecture
*Goal: Saturation of 10Gbps+ links with minimal CPU usage by bypassing standard syscall overhead.*

- [ ] **Implement `AF_PACKET` Ring Buffers (`PACKET_MMAP`)**
    - Replace `send()`/`recv()` with `PACKET_RX_RING` and `PACKET_TX_RING`.
    - Achieve zero-copy reception via shared kernel/user memory mapping.
    - Reference: `src/raw_socket.cpp`.

- [ ] **`io_uring` Backend**
    - Implement an asynchronous I/O backend for Linux 5.10+.
    - Batch submission of frame operations to reduce syscall context switching.

- [ ] **Zero-Allocation Frame Builder**
    - Refactor `l2net::frame_builder` to write directly into `PACKET_TX_RING` slots or pre-allocated arenas.
    - Eliminate `std::vector` allocations in the hot path.

- [ ] **Batch Processing**
    - Implement `recvmmsg` and `sendmmsg` for batched packet processing as a fallback for systems without `io_uring`.

## Phase 2: Industrial & Real-Time Features
*Goal: Support strict timing and reliability requirements for OT (Operational Technology) environments.*

- [ ] **Hardware Timestamping**
    - Enable `SO_TIMESTAMPING` to retrieve NIC hardware timestamps.
    - Crucial for precise latency measurements and PTP implementations.

- [ ] **Kernel-Side Filtering (eBPF)**
    - Allow attaching BPF filters to `raw_socket` (`SO_ATTACH_FILTER`).
    - Discard irrelevant traffic in kernel space before it reaches the application.

- [ ] **Link State Monitoring**
    - Implement Netlink socket listener to detect cable unplug/plug events asynchronously.

- [ ] **VLAN Offloading**
    - Utilize NIC hardware offloading for VLAN tag insertion/stripping instead of software manipulation.

## Phase 3: Portability & Ecosystem
*Goal: Expand platform support and ease of use.*

- [ ] **BSD/macOS Support**
    - Abstract `raw_socket` to support `/dev/bpf*` devices.
    - Allow development on macOS with deployment to Linux targets.

- [ ] **Fuzz Testing**
    - Integrate LLVM `libFuzzer` to harden `frame_parser` against malformed inputs.

- [ ] **Package Manager Support**
    - Add `conanfile.py` and `vcpkg.json` for easy consumption by other projects.

---

## Phase 4: ROS 2 RMW Implementation (`rmw_l2net`)
*Goal: Create a high-performance, brokerless ROS 2 Middleware interface using `l2net` primitives.*

This is a significant undertaking that maps ROS 2 concepts (Nodes, Pub/Sub) to Raw L2 frames. The architecture leverages the `l2net::hybrid` pattern (TCP Control + L2 Data).

### 4.1. Architecture Design
- **Package Name:** `rmw_l2net`
- **Transport Strategy:**
    - **Discovery (Graph):** Uses `l2net::hybrid` TCP logic (reliable) or L2 Multicast (fast).
    - **Topics (Data):** Raw Ethernet frames (EtherType `0x88B5`). Topics map to specific MAC multicast groups or VLAN tags.
    - **Services (RPC):** TCP or Reliable-L2 implementation.

### 4.2. Implementation Steps

#### Step 1: Minimum Viable RMW
- [ ] **RMW Skeleton:** Implement the C interface (`rmw_init`, `rmw_create_node`, `rmw_destroy_node`).
- [ ] **Identifier:** Define the RMW identifier `rmw_l2net_cpp`.
- [ ] **Serialization:** Integrate with `rosidl_typesupport` to serialize ROS messages into `l2net` payload buffers (CDR format).

#### Step 2: Discovery System
*Using `l2net::hybrid_chat` concepts:*
- [ ] **Participant Discovery:** Broadcast "Hello" frames on a dedicated L2 Multicast address.
- [ ] **Graph Cache:** Maintain a local map of `Node <-> MAC Address` and `Topic <-> VLAN/Multicast ID`.
- [ ] **Liveliness:** Implement periodic heartbeats using lightweight L2 frames.

#### Step 3: Publish / Subscribe
- [ ] **Publisher:**
    - Map ROS Topics to specific L2 destinations (Multicast MACs).
    - Use `l2net::raw_socket` for "Best Effort" QoS.
- [ ] **Subscriber:**
    - Use BPF filters to only wake up for subscribed Topics (filtering by Dest MAC or custom header).
    - Zero-copy deserialization from ring buffer to ROS message.

#### Step 4: Quality of Service (QoS)
- [ ] **Best Effort:** Direct mapping to Raw L2 (fire and forget).
- [ ] **Reliable:** Implement a lightweight ACK/NACK protocol over L2 (or fallback to TCP for strict reliability).
- [ ] **History:** Implement simple ring-buffer history for "Keep Last" semantics.

### 4.3. Benchmarking `rmw_l2net`
- [ ] Compare `rmw_l2net` latency vs `rmw_cyclonedds` and `rmw_fastrtps` on loopback and physical networks.
- [ ] Validate behavior under high load (10Gbps saturation).

## Legend
- [ ] Planned
- [x] Completed
- [-] Deprecated / On Hold