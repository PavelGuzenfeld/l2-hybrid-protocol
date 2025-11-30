// bench/bench_network.cpp - network benchmarks
// L2 raw socket vs UDP over physical network interface
// WARNING: requires root privileges and a network interface

#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp"
#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

    [[nodiscard]] auto can_run_network_benchmarks() -> bool
    {
        if (::geteuid() != 0)
            return false;
        auto all = l2net::interface_info::list_all();
        if (!all.has_value())
            return false;
        for (auto const &iface : *all)
        {
            if (!iface.is_loopback() && iface.is_up() && !iface.mac().is_null())
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto get_network_interface() -> std::optional<l2net::interface_info>
    {
        auto all = l2net::interface_info::list_all();
        if (!all.has_value())
            return std::nullopt;
        for (auto const &iface : *all)
        {
            if (!iface.is_loopback() && iface.is_up() && !iface.mac().is_null())
            {
                return iface;
            }
        }
        return std::nullopt;
    }

    class udp_broadcast_socket
    {
        int fd_{-1};

    public:
        udp_broadcast_socket() : fd_{::socket(AF_INET, SOCK_DGRAM, 0)}
        {
            if (fd_ >= 0)
            {
                int opt = 1;
                ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
            }
        }
        ~udp_broadcast_socket()
        {
            if (fd_ >= 0)
                ::close(fd_);
        }
        udp_broadcast_socket(udp_broadcast_socket const &) = delete;
        udp_broadcast_socket &operator=(udp_broadcast_socket const &) = delete;

        [[nodiscard]] auto send_broadcast(void const *data, std::size_t len, std::uint16_t port) -> ssize_t
        {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            addr.sin_port = htons(port);
            return ::sendto(fd_, data, len, 0,
                            reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
        }
        [[nodiscard]] auto is_valid() const -> bool { return fd_ >= 0; }
    };

} // anonymous namespace

static void BM_L2_Network_Send_Small(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    auto sock = l2net::raw_socket::create_bound(*iface);
    if (!sock.has_value())
    {
        state.SkipWithError("sock fail");
        return;
    }
    auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(), l2net::constants::eth_p_custom, std::vector<std::uint8_t>(50, 0x42));
    if (!frame.has_value())
    {
        state.SkipWithError("frame fail");
        return;
    }
    for (auto _ : state)
    {
        auto result = sock->send_raw(*frame, *iface);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(frame->size()));
}
BENCHMARK(BM_L2_Network_Send_Small);

static void BM_L2_Network_Send_Large(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    auto sock = l2net::raw_socket::create_bound(*iface);
    if (!sock.has_value())
    {
        state.SkipWithError("sock fail");
        return;
    }
    auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(), l2net::constants::eth_p_custom, std::vector<std::uint8_t>(1400, 0x42));
    if (!frame.has_value())
    {
        state.SkipWithError("frame fail");
        return;
    }
    for (auto _ : state)
    {
        auto result = sock->send_raw(*frame, *iface);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(frame->size()));
}
BENCHMARK(BM_L2_Network_Send_Large);

static void BM_L2_Network_VlanSend_Small(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    auto sock = l2net::raw_socket::create_bound(*iface);
    if (!sock.has_value())
    {
        state.SkipWithError("sock fail");
        return;
    }
    l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
    auto frame = l2net::build_vlan_frame(l2net::mac_address::broadcast(), iface->mac(), tci, l2net::constants::eth_p_custom, std::vector<std::uint8_t>(50, 0x42));
    if (!frame.has_value())
    {
        state.SkipWithError("frame fail");
        return;
    }
    for (auto _ : state)
    {
        auto result = sock->send_raw(*frame, *iface);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(frame->size()));
}
BENCHMARK(BM_L2_Network_VlanSend_Small);

static void BM_L2_Network_VlanSend_Large(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    auto sock = l2net::raw_socket::create_bound(*iface);
    if (!sock.has_value())
    {
        state.SkipWithError("sock fail");
        return;
    }
    l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
    auto frame = l2net::build_vlan_frame(l2net::mac_address::broadcast(), iface->mac(), tci, l2net::constants::eth_p_custom, std::vector<std::uint8_t>(1400, 0x42));
    if (!frame.has_value())
    {
        state.SkipWithError("frame fail");
        return;
    }
    for (auto _ : state)
    {
        auto result = sock->send_raw(*frame, *iface);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(frame->size()));
}
BENCHMARK(BM_L2_Network_VlanSend_Large);

static void BM_UDP_Network_Broadcast_Small(benchmark::State &state)
{
    udp_broadcast_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("sock fail");
        return;
    }
    std::array<std::uint8_t, 50> payload{};
    for (auto _ : state)
    {
        auto result = sock.send_broadcast(payload.data(), payload.size(), 19997);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 50);
}
BENCHMARK(BM_UDP_Network_Broadcast_Small);

static void BM_UDP_Network_Broadcast_Large(benchmark::State &state)
{
    udp_broadcast_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("sock fail");
        return;
    }
    std::vector<std::uint8_t> payload(1400, 0x42);
    for (auto _ : state)
    {
        auto result = sock.send_broadcast(payload.data(), payload.size(), 19997);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1400);
}
BENCHMARK(BM_UDP_Network_Broadcast_Large);

static void BM_L2_Socket_Create(benchmark::State &state)
{
    if (::geteuid() != 0)
    {
        state.SkipWithError("requires root");
        return;
    }
    for (auto _ : state)
    {
        auto sock = l2net::raw_socket::create();
        benchmark::DoNotOptimize(sock);
    }
}
BENCHMARK(BM_L2_Socket_Create);

static void BM_UDP_Socket_Create(benchmark::State &state)
{
    for (auto _ : state)
    {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        benchmark::DoNotOptimize(fd);
        if (fd >= 0)
            ::close(fd);
    }
}
BENCHMARK(BM_UDP_Socket_Create);

static void BM_L2_Socket_CreateAndBind(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    for (auto _ : state)
    {
        auto sock = l2net::raw_socket::create_bound(*iface);
        benchmark::DoNotOptimize(sock);
    }
}
BENCHMARK(BM_L2_Socket_CreateAndBind);

static void BM_Interface_Query(benchmark::State &state)
{
    for (auto _ : state)
    {
        auto result = l2net::interface_info::query("lo");
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_Interface_Query);

static void BM_Interface_ListAll(benchmark::State &state)
{
    for (auto _ : state)
    {
        auto result = l2net::interface_info::list_all();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_Interface_ListAll);

static void BM_L2_Network_PayloadSize(benchmark::State &state)
{
    if (!can_run_network_benchmarks())
    {
        state.SkipWithError("requires root");
        return;
    }
    auto iface = get_network_interface();
    if (!iface.has_value())
    {
        state.SkipWithError("no iface");
        return;
    }
    auto sock = l2net::raw_socket::create_bound(*iface);
    if (!sock.has_value())
    {
        state.SkipWithError("sock fail");
        return;
    }
    auto const payload_size = static_cast<std::size_t>(state.range(0));
    auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(), l2net::constants::eth_p_custom, std::vector<std::uint8_t>(payload_size, 0x42));
    if (!frame.has_value())
    {
        state.SkipWithError("frame fail");
        return;
    }
    for (auto _ : state)
    {
        auto result = sock->send_raw(*frame, *iface);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(frame->size()));
}
BENCHMARK(BM_L2_Network_PayloadSize)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024)->Arg(1400);

static void BM_UDP_Network_PayloadSize(benchmark::State &state)
{
    udp_broadcast_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("sock fail");
        return;
    }
    auto const payload_size = static_cast<std::size_t>(state.range(0));
    std::vector<std::uint8_t> payload(payload_size, 0x42);
    for (auto _ : state)
    {
        auto result = sock.send_broadcast(payload.data(), payload.size(), 19996);
        benchmark::DoNotOptimize(result);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payload_size));
}
BENCHMARK(BM_UDP_Network_PayloadSize)->Arg(32)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024)->Arg(1400);
