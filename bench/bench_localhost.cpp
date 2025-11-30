// bench/bench_localhost.cpp

#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/ipc_channel.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp" // FIXED: Added missing include
#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

    [[nodiscard]] auto can_run_raw_benchmarks() -> bool
    {
        if (::geteuid() != 0)
            return false;
        auto lo = l2net::get_loopback_interface();
        return lo.has_value();
    }

    class udp_socket
    {
        int fd_{-1};

    public:
        udp_socket() : fd_{::socket(AF_INET, SOCK_DGRAM, 0)} {}
        ~udp_socket()
        {
            if (fd_ >= 0)
                ::close(fd_);
        }
        udp_socket(udp_socket const &) = delete;
        udp_socket &operator=(udp_socket const &) = delete;

        [[nodiscard]] auto bind(std::uint16_t port) -> bool
        {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            return ::bind(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0;
        }

        [[nodiscard]] auto send_to(void const *data, std::size_t len, std::uint16_t port) -> ssize_t
        {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            return ::sendto(fd_, data, len, 0, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
        }

        [[nodiscard]] auto recv(void *data, std::size_t len) -> ssize_t
        {
            return ::recv(fd_, data, len, 0);
        }

        [[nodiscard]] auto is_valid() const -> bool { return fd_ >= 0; }
        [[nodiscard]] auto fd() const -> int { return fd_; }
    };

    struct benchmark_state
    {
        std::vector<std::uint8_t> small_frame, medium_frame, large_frame, jumbo_frame;
        benchmark_state()
        {
            auto const build = [](std::size_t s)
            {
                std::vector<std::uint8_t> p(s, 0x42);
                auto r = l2net::build_simple_frame(l2net::mac_address::null(), l2net::mac_address::null(), l2net::constants::eth_p_ipc, p);
                return r.has_value() ? *r : std::vector<std::uint8_t>{};
            };
            small_frame = build(50);
            medium_frame = build(498);
            large_frame = build(1386);
            jumbo_frame = build(7986);
        }
    };
    benchmark_state const &get_state()
    {
        static benchmark_state state;
        return state;
    }

} // namespace

static void BM_FrameBuild_Small(benchmark::State &state)
{
    l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::array<std::uint8_t, 50> payload{};
    for (auto _ : state)
    {
        auto frame = l2net::build_simple_frame(dest, src, 0x88B5, payload);
        benchmark::DoNotOptimize(frame);
    }
    // FIXED: Sign conversion
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 64);
}
BENCHMARK(BM_FrameBuild_Small);

static void BM_FrameBuild_Large(benchmark::State &state)
{
    l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::vector<std::uint8_t> payload(1400, 0x42);
    for (auto _ : state)
    {
        auto frame = l2net::build_simple_frame(dest, src, 0x88B5, payload);
        benchmark::DoNotOptimize(frame);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1414);
}
BENCHMARK(BM_FrameBuild_Large);

static void BM_FrameBuild_IntoBuffer(benchmark::State &state)
{
    l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::vector<std::uint8_t> payload(1400, 0x42);
    std::vector<std::uint8_t> buffer(1500);
    auto builder = l2net::frame_builder{}.set_dest(dest).set_src(src).set_ether_type(0x88B5).set_payload(payload);
    for (auto _ : state)
    {
        auto result = builder.build_into(buffer);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1414);
}
BENCHMARK(BM_FrameBuild_IntoBuffer);

static void BM_FrameParse_Untagged(benchmark::State &state)
{
    auto const &frames = get_state();
    for (auto _ : state)
    {
        l2net::frame_parser parser{frames.large_frame};
        benchmark::DoNotOptimize(parser.is_valid());
    }
}
BENCHMARK(BM_FrameParse_Untagged);

static void BM_FrameParse_Tagged(benchmark::State &state)
{
    l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
    auto tagged = l2net::build_vlan_frame(l2net::mac_address::null(), l2net::mac_address::null(), tci, 0x88B5, std::vector<std::uint8_t>(1386, 0x42));
    if (!tagged.has_value())
    {
        state.SkipWithError("fail");
        return;
    }
    for (auto _ : state)
    {
        l2net::frame_parser parser{*tagged};
        benchmark::DoNotOptimize(parser.is_valid());
    }
}
BENCHMARK(BM_FrameParse_Tagged);

static void BM_L2_Send_Small(benchmark::State &state)
{
    if (!can_run_raw_benchmarks())
    {
        state.SkipWithError("root req");
        return;
    }
    auto channel = l2net::ipc_channel::create();
    if (!channel.has_value())
    {
        state.SkipWithError("create fail");
        return;
    }
    std::array<std::uint8_t, 50> payload{};
    for (auto _ : state)
    {
        // FIXED: nodiscard
        (void)channel->send(payload);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 64);
}
BENCHMARK(BM_L2_Send_Small);

static void BM_L2_Send_Large(benchmark::State &state)
{
    if (!can_run_raw_benchmarks())
    {
        state.SkipWithError("root req");
        return;
    }
    auto channel = l2net::ipc_channel::create();
    if (!channel.has_value())
    {
        state.SkipWithError("create fail");
        return;
    }
    std::vector<std::uint8_t> payload(1400, 0x42);
    for (auto _ : state)
    {
        (void)channel->send(payload);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1414);
}
BENCHMARK(BM_L2_Send_Large);

static void BM_L2_Send_Jumbo(benchmark::State &state)
{
    if (!can_run_raw_benchmarks())
    {
        state.SkipWithError("root req");
        return;
    }
    auto channel = l2net::ipc_channel::create();
    if (!channel.has_value())
    {
        state.SkipWithError("create fail");
        return;
    }
    std::vector<std::uint8_t> payload(8000, 0x42);
    for (auto _ : state)
    {
        (void)channel->send(payload);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 8014);
}
BENCHMARK(BM_L2_Send_Jumbo);

static void BM_UDP_Send_Small(benchmark::State &state)
{
    udp_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("udp fail");
        return;
    }
    std::array<std::uint8_t, 50> payload{};
    for (auto _ : state)
    {
        (void)sock.send_to(payload.data(), payload.size(), 19999);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 50);
}
BENCHMARK(BM_UDP_Send_Small);

static void BM_UDP_Send_Large(benchmark::State &state)
{
    udp_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("udp fail");
        return;
    }
    std::vector<std::uint8_t> payload(1400, 0x42);
    for (auto _ : state)
    {
        (void)sock.send_to(payload.data(), payload.size(), 19999);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1400);
}
BENCHMARK(BM_UDP_Send_Large);

static void BM_UDP_Send_Jumbo(benchmark::State &state)
{
    udp_socket sock;
    if (!sock.is_valid())
    {
        state.SkipWithError("udp fail");
        return;
    }
    std::vector<std::uint8_t> payload(8000, 0x42);
    for (auto _ : state)
    {
        (void)sock.send_to(payload.data(), payload.size(), 19999);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 8000);
}
BENCHMARK(BM_UDP_Send_Jumbo);

static void BM_L2_Roundtrip_Latency(benchmark::State &state)
{
    if (!can_run_raw_benchmarks())
    {
        state.SkipWithError("root req");
        return;
    }
    l2net::ipc_config config;
    config.recv_timeout = std::chrono::milliseconds{100};
    auto channel = l2net::ipc_channel::create(config);
    if (!channel.has_value())
    {
        state.SkipWithError("create fail");
        return;
    }
    std::array<std::uint8_t, 64> payload{};
    for (auto _ : state)
    {
        (void)channel->send(payload);
        auto result = channel->receive_with_timeout(std::chrono::milliseconds{10});
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_L2_Roundtrip_Latency);

static void BM_UDP_Roundtrip_Latency(benchmark::State &state)
{
    udp_socket sock;
    if (!sock.is_valid() || !sock.bind(19998))
    {
        state.SkipWithError("udp fail");
        return;
    }
    std::array<std::uint8_t, 64> payload{};
    std::array<std::uint8_t, 128> buf{};
    struct timeval tv{};
    tv.tv_usec = 10000;
    setsockopt(sock.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (auto _ : state)
    {
        (void)sock.send_to(payload.data(), payload.size(), 19998);
        auto result = sock.recv(buf.data(), buf.size());
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_UDP_Roundtrip_Latency);

static void BM_MacAddress_FromString(benchmark::State &state)
{
    for (auto _ : state)
    {
        auto result = l2net::mac_address::from_string("aa:bb:cc:dd:ee:ff");
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MacAddress_FromString);

static void BM_MacAddress_ToString(benchmark::State &state)
{
    l2net::mac_address const mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    for (auto _ : state)
    {
        auto result = mac.to_string();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_MacAddress_ToString);

static void BM_VlanTci_Encode(benchmark::State &state)
{
    l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 100};
    for (auto _ : state)
    {
        auto result = tci.encode();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_VlanTci_Encode);

static void BM_VlanTci_Decode(benchmark::State &state)
{
    std::uint16_t const encoded = 0xE064;
    for (auto _ : state)
    {
        auto result = l2net::vlan_tci::decode(encoded);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_VlanTci_Decode);

static void BM_VlanFrame_Build(benchmark::State &state)
{
    l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
    std::vector<std::uint8_t> payload(1386, 0x42);
    for (auto _ : state)
    {
        auto frame = l2net::build_vlan_frame(l2net::mac_address::broadcast(), l2net::mac_address::null(), tci, 0x88B5, payload);
        benchmark::DoNotOptimize(frame);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 1404);
}
BENCHMARK(BM_VlanFrame_Build);