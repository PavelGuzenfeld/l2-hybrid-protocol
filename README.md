
# Linux Layer 2 (Raw Socket) Network Demonstrations

This repository contains two C++ proof-of-concept applications demonstrating high-performance, low-level systems programming on Linux using **Raw Sockets (`AF_PACKET`)**.

These tools bypass the standard Transport (TCP/UDP) and Network (IP) layers to communicate directly via Ethernet frames, offering deterministic latency and reduced overhead.

## 📂 Project Structure

* **`src/hybrid_chat.cpp`**: A hybrid network tool that uses TCP for the initial handshake and then switches to Raw Ethernet (VLAN Tagged) for data streaming.
* **`src/ipc_l2.cpp`**: A local Inter-Process Communication (IPC) tool that uses the Loopback interface (`lo`) as a high-speed message bus, bypassing the kernel's IP stack.

---

## 🚀 Prerequisites

* **OS:** Linux (Raw sockets are a Linux-specific feature).
* **Compiler:** `g++` (GCC).
* **Privileges:** **Root (`sudo`)** is required for all execution. Standard users cannot open `SOCK_RAW`.

---

## 🛠️ Compilation

Run the following commands to build both tools:

```bash
# Compile the Hybrid Chat (Network) tool
g++ -O2 -o hybrid_chat src/hybrid_chat.cpp

# Compile the IPC (Localhost) tool
g++ -O2 -o ipc_l2 src/ipc_l2.cpp
````

-----

## 1\. Hybrid Chat (`hybrid_chat`)

This tool simulates a high-end industrial protocol setup. It establishes a distinct Control Plane and Data Plane.

1.  **Control Plane (TCP/9000):** Machines exchange MAC addresses reliably.
2.  **Data Plane (EtherType 0x88B5):** Machines switch to raw Ethernet frames tagged with **802.1Q VLAN 10** and **Priority 7 (Critical)**.

### Usage

**Machine A (Server/Receiver):**

```bash
# Syntax: sudo ./hybrid_chat <interface> server
sudo ./hybrid_chat eth0 server
```

**Machine B (Client/Sender):**

```bash
# Syntax: sudo ./hybrid_chat <interface> client <server_ip>
sudo ./hybrid_chat eth0 client 192.168.1.15
```

> **Note:** Replace `eth0` with your actual network interface (check using `ip link`).

-----

## 2\. L2 Local IPC (`ipc_l2`)

This tool demonstrates using the Loopback interface (`lo`) for local communication between processes. It binds to a custom Protocol ID (`0xAAAA`) to create an isolated communication channel that is invisible to standard IP-based applications.

### Usage

**Terminal 1 (Receiver):**

```bash
sudo ./ipc_l2 recv
```

**Terminal 2 (Sender):**

```bash
sudo ./ipc_l2 send
```

-----

## 🔍 Debugging & Internals

Since these tools use standard Ethernet frames, you can inspect the traffic using `tcpdump` or Wireshark.

**To sniff the Hybrid Chat traffic:**

```bash
# Filters for our custom EtherType 0x88B5
sudo tcpdump -i eth0 -e ether proto 0x88B5
```

**To sniff the IPC traffic on Localhost:**

```bash
# Filters for our custom IPC Protocol 0xAAAA
sudo tcpdump -i lo -X ether proto 0xAAAA
```

### Protocol Constants Used

  * **Hybrid Data Protocol:** `0x88B5`
  * **VLAN Tag:** ID `10`, Priority `7`
  * **IPC Protocol:** `0xAAAA`

<!-- end list -->

```