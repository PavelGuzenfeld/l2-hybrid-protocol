# l2net

High-performance Layer 2 raw socket networking library for Linux.

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

## Applications

```bash
# IPC (localhost)
sudo ./build/release/ipc_l2 recv  # terminal 1
sudo ./build/release/ipc_l2 send  # terminal 2

# Hybrid Chat (network)
sudo ./build/release/hybrid_chat eth0 server              # machine A
sudo ./build/release/hybrid_chat eth0 client 192.168.1.15 # machine B
```

## Sniff Traffic

```bash
# hybrid chat protocol
sudo tcpdump -i eth0 -e ether proto 0x88B5

# ipc protocol on loopback
sudo tcpdump -i lo -X ether proto 0xAAAA
```

## Library Usage

```cpp
#include <l2net/ipc_channel.hpp>
#include <l2net/hybrid_chat.hpp>

// IPC
auto channel = l2net::ipc_channel::create();
channel->send("message");
auto msg = channel->receive();

// Hybrid endpoint
auto endpoint = l2net::hybrid_endpoint::create_server(iface);
endpoint->send_data("data");
```

## Protocol Constants

| Protocol | EtherType |
|----------|-----------|
| Hybrid Data | `0x88B5` |
| IPC | `0xAAAA` |
| VLAN Tag | `0x8100` |

Default VLAN: ID `10`, Priority `7`
