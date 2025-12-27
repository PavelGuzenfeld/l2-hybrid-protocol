// remote_node.cpp - remote benchmark node
// deployed via ssh to remote machines for latency/throughput testing
// because testing on localhost is for cowards

#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <thread>
#include <vector>

namespace
{
    [[nodiscard]] auto send_with_retry(
        l2net::raw_socket &sock,
        std::span<const std::uint8_t> frame,
        l2net::interface_info const &iface,
        std::uint32_t max_retries = 200,
        std::chrono::microseconds backoff = std::chrono::microseconds{10}) -> l2net::result<std::size_t>
    {
        for (std::uint32_t i = 0; i < max_retries; ++i)
        {
            auto send_result = sock.send_raw(frame, iface);
            if (send_result.has_value())
            {
                return send_result;
            }

            // NOTE:
            // send_raw() currently returns only l2net::error_code,
            // so we don't know whether this is ENOBUFS/EAGAIN.
            // In practice, send failures in flood mode are almost always transient,
            // so we treat *all* send failures as retryable here.
            //
            // If you expose errno later, make this conditional:
            // retry only on ENOBUFS/EAGAIN/EWOULDBLOCK, otherwise fail.
            std::this_thread::sleep_for(backoff);
        }

        // return the last failure
        return sock.send_raw(frame, iface);
    }

    std::atomic<bool> g_running{true};

    auto signal_handler(int /*signal*/) -> void
    {
        g_running.store(false);
    }

    // protocol for benchmark coordination
    namespace proto
    {
        // custom ethertype for benchmark traffic - won't collide with real protocols
        inline constexpr std::uint16_t eth_p_bench = 0xBEEF;

        // message types embedded in first byte of payload
        inline constexpr std::uint8_t msg_ping = 0x01;       // latency test request
        inline constexpr std::uint8_t msg_pong = 0x02;       // latency test response
        inline constexpr std::uint8_t msg_data = 0x03;       // throughput data
        inline constexpr std::uint8_t msg_ack = 0x04;        // throughput ack
        inline constexpr std::uint8_t msg_start = 0x10;      // start signal
        inline constexpr std::uint8_t msg_stop = 0x11;       // stop signal
        inline constexpr std::uint8_t msg_ready = 0x12;      // ready signal
        inline constexpr std::uint8_t msg_stats = 0x20;      // stats request
        inline constexpr std::uint8_t msg_stats_resp = 0x21; // stats response
    } // namespace proto

    struct benchmark_stats
    {
        std::uint64_t packets_sent{0};
        std::uint64_t packets_received{0};
        std::uint64_t bytes_sent{0};
        std::uint64_t bytes_received{0};
        std::chrono::nanoseconds total_latency{0};
        std::chrono::steady_clock::time_point start_time{};
        std::chrono::steady_clock::time_point end_time{};
    };

    auto print_usage(char const *program_name) -> void
    {
        fmt::print(stderr, R"(
Usage: sudo {} <mode> <interface> [options]

Modes:
  echo        - Echo server: receives frames and sends them back (for latency tests)
  sink        - Sink server: receives frames silently (for throughput tests)
  ping        - Ping client: sends frames and waits for echo (latency measurement)
  flood       - Flood client: sends frames as fast as possible (throughput measurement)

Options:
  --peer-mac <mac>      Peer MAC address (required for client modes)
  --payload-size <n>    Payload size in bytes (default: 64)
  --count <n>           Number of packets to send (default: 1000, 0 = infinite)
  --interval <us>       Microseconds between sends (default: 0 for flood, 1000 for ping)
  --timeout <ms>        Receive timeout in milliseconds (default: 1000)
  --vlan <id>           Use VLAN tagging with specified ID
  --priority <n>        VLAN priority 0-7 (default: 0)
  --quiet               Suppress per-packet output

Examples:
  sudo {} echo eth0
  sudo {} ping eth0 --peer-mac aa:bb:cc:dd:ee:ff --payload-size 1400 --count 10000
  sudo {} flood eth0 --peer-mac aa:bb:cc:dd:ee:ff --payload-size 8000
  sudo {} sink eth0 --vlan 10 --priority 7

)",
                   program_name, program_name, program_name, program_name, program_name);
    }

    struct config
    {
        std::string mode{};
        std::string interface_name{};
        l2net::mac_address peer_mac{};
        std::size_t payload_size{64};
        std::uint64_t count{1000};
        std::uint64_t interval_us{0};
        std::uint32_t timeout_ms{1000};
        std::uint16_t vlan_id{0};
        std::uint8_t vlan_priority{0};
        bool use_vlan{false};
        bool quiet{false};
    };

    [[nodiscard]] auto parse_args(int argc, char const *argv[]) -> std::optional<config>
    {
        if (argc < 3)
        {
            return std::nullopt;
        }

        config cfg;
        cfg.mode = argv[1];
        cfg.interface_name = argv[2];

        // set default interval based on mode
        if (cfg.mode == "ping")
        {
            cfg.interval_us = 1000;
        }

        for (int i = 3; i < argc; ++i)
        {
            std::string_view const arg{argv[i]};

            if (arg == "--peer-mac" && i + 1 < argc)
            {
                auto result = l2net::mac_address::from_string(argv[++i]);
                if (!result.has_value())
                {
                    fmt::print(stderr, "Error: invalid MAC address\n");
                    return std::nullopt;
                }
                cfg.peer_mac = *result;
            }
            else if (arg == "--payload-size" && i + 1 < argc)
            {
                cfg.payload_size = static_cast<std::size_t>(std::stoull(argv[++i]));
                if (cfg.payload_size == 0)
                {
                    fmt::print(stderr, "Error: payload size must be at least 1\n");
                    return std::nullopt;
                }
            }
            else if (arg == "--count" && i + 1 < argc)
            {
                cfg.count = std::stoull(argv[++i]);
            }
            else if (arg == "--interval" && i + 1 < argc)
            {
                cfg.interval_us = std::stoull(argv[++i]);
            }
            else if (arg == "--timeout" && i + 1 < argc)
            {
                cfg.timeout_ms = static_cast<std::uint32_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--vlan" && i + 1 < argc)
            {
                cfg.vlan_id = static_cast<std::uint16_t>(std::stoul(argv[++i]));
                cfg.use_vlan = true;
            }
            else if (arg == "--priority" && i + 1 < argc)
            {
                cfg.vlan_priority = static_cast<std::uint8_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--quiet")
            {
                cfg.quiet = true;
            }
            else
            {
                fmt::print(stderr, "Error: unknown argument '{}'\n", arg);
                return std::nullopt;
            }
        }

        // final validation
        if (cfg.payload_size == 0)
        {
            cfg.payload_size = 1; // minimum payload size
        }

        return cfg;
    }

    [[nodiscard]] auto build_frame(l2net::mac_address const &dest, l2net::mac_address const &src,
                                   std::span<std::uint8_t const> payload, config const &cfg)
        -> l2net::result<std::vector<std::uint8_t>>
    {
        if (cfg.use_vlan)
        {
            l2net::vlan_tci const tci{.priority = cfg.vlan_priority, .dei = false, .vlan_id = cfg.vlan_id};
            return l2net::build_vlan_frame(dest, src, tci, proto::eth_p_bench, payload);
        }
        return l2net::build_simple_frame(dest, src, proto::eth_p_bench, payload);
    }

    // =============================================================================
    // echo server - reflects packets back for latency testing
    // =============================================================================
    auto run_echo_server(l2net::interface_info const &iface, config const &cfg) -> int
    {
        fmt::print("Echo server starting on {} (MAC: {})\n", iface.name(), iface.mac());
        if (cfg.use_vlan)
        {
            fmt::print("  VLAN ID: {}, Priority: {}\n", cfg.vlan_id, cfg.vlan_priority);
        }
        fmt::print("  Timeout: {} ms\n", cfg.timeout_ms);
        fmt::print("Waiting for packets...\n\n");

        auto sock_result = l2net::raw_socket::create_bound(iface, l2net::raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            fmt::print(stderr, "Error creating socket: {}\n", sock_result.error());
            return 1;
        }
        auto &sock = *sock_result;

        std::vector<std::uint8_t> buffer(65536);
        benchmark_stats stats{};
        stats.start_time = std::chrono::steady_clock::now();

        while (g_running.load())
        {
            auto recv_result = sock.receive_with_timeout(buffer, std::chrono::milliseconds{cfg.timeout_ms});

            if (!recv_result.has_value())
            {
                if (recv_result.error() == l2net::error_code::timeout)
                {
                    continue;
                }
                fmt::print(stderr, "Receive error: {}\n", recv_result.error());
                continue;
            }

            auto const received_size = *recv_result;
            l2net::frame_parser parser{std::span{buffer.data(), received_size}};

            if (!parser.is_valid())
            {
                continue;
            }

            // check if it's our protocol
            if (parser.ether_type() != proto::eth_p_bench)
            {
                continue;
            }

            auto const payload = parser.payload();
            if (payload.empty())
            {
                continue;
            }

            // only echo ping messages
            if (payload[0] != proto::msg_ping)
            {
                continue;
            }

            stats.packets_received++;
            stats.bytes_received += received_size;

            // build response - swap src/dest, change ping to pong
            std::vector<std::uint8_t> response_payload(payload.begin(), payload.end());
            response_payload[0] = proto::msg_pong;

            auto frame_result = build_frame(parser.src_mac(), iface.mac(), response_payload, cfg);

            if (!frame_result.has_value())
            {
                continue;
            }

            auto send_result = sock.send_raw(*frame_result, iface);
            if (send_result.has_value())
            {
                stats.packets_sent++;
                stats.bytes_sent += *send_result;

                if (!cfg.quiet)
                {
                    fmt::print("Echo: {} bytes from {}\n", payload.size(), parser.src_mac());
                }
            }
        }

        stats.end_time = std::chrono::steady_clock::now();
        auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);

        fmt::print("\n--- Echo Server Statistics ---\n");
        fmt::print("Packets: {} received, {} sent\n", stats.packets_received, stats.packets_sent);
        fmt::print("Bytes: {} received, {} sent\n", stats.bytes_received, stats.bytes_sent);
        fmt::print("Duration: {} ms\n", duration.count());

        return 0;
    }

    // =============================================================================
    // sink server - silently receives packets for throughput testing
    // =============================================================================
    auto run_sink_server(l2net::interface_info const &iface, config const &cfg) -> int
    {
        fmt::print("Sink server starting on {} (MAC: {})\n", iface.name(), iface.mac());
        if (cfg.use_vlan)
        {
            fmt::print("  VLAN ID: {}, Priority: {}\n", cfg.vlan_id, cfg.vlan_priority);
        }
        fmt::print("Waiting for packets...\n\n");

        auto sock_result = l2net::raw_socket::create_bound(iface, l2net::raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            fmt::print(stderr, "Error creating socket: {}\n", sock_result.error());
            return 1;
        }
        auto &sock = *sock_result;

        std::vector<std::uint8_t> buffer(65536);
        benchmark_stats stats{};
        stats.start_time = std::chrono::steady_clock::now();

        auto last_report = stats.start_time;
        std::uint64_t last_packets = 0;
        std::uint64_t last_bytes = 0;

        while (g_running.load())
        {
            auto recv_result = sock.receive_with_timeout(buffer, std::chrono::milliseconds{cfg.timeout_ms});

            if (!recv_result.has_value())
            {
                if (recv_result.error() == l2net::error_code::timeout)
                {
                    // periodic stats report
                    auto const now = std::chrono::steady_clock::now();
                    auto const interval = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report);

                    if (interval.count() >= 1000 && stats.packets_received > last_packets)
                    {
                        auto const pps = (stats.packets_received - last_packets) * 1000 /
                                         static_cast<std::uint64_t>(interval.count());
                        auto const mbps = (stats.bytes_received - last_bytes) * 8 /
                                          static_cast<std::uint64_t>(interval.count()) / 1000;
                        fmt::print("Rate: {} pps, {} Mbps\n", pps, mbps);

                        last_report = now;
                        last_packets = stats.packets_received;
                        last_bytes = stats.bytes_received;
                    }
                    continue;
                }
                continue;
            }

            auto const received_size = *recv_result;
            l2net::frame_parser parser{std::span{buffer.data(), received_size}};

            if (!parser.is_valid())
            {
                continue;
            }

            if (parser.ether_type() != proto::eth_p_bench)
            {
                continue;
            }

            stats.packets_received++;
            stats.bytes_received += received_size;
        }

        stats.end_time = std::chrono::steady_clock::now();
        auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);

        fmt::print("\n--- Sink Server Statistics ---\n");
        fmt::print("Packets received: {}\n", stats.packets_received);
        fmt::print("Bytes received: {}\n", stats.bytes_received);
        fmt::print("Duration: {} ms\n", duration.count());

        if (duration.count() > 0)
        {
            auto const pps = stats.packets_received * 1000 / static_cast<std::uint64_t>(duration.count());
            auto const mbps = stats.bytes_received * 8 / static_cast<std::uint64_t>(duration.count()) / 1000;
            fmt::print("Average: {} pps, {} Mbps\n", pps, mbps);
        }

        return 0;
    }

    // =============================================================================
    // ping client - sends packets and measures round-trip latency
    // =============================================================================
    auto run_ping_client(l2net::interface_info const &iface, config const &cfg) -> int
    {
        if (cfg.peer_mac.is_null())
        {
            fmt::print(stderr, "Error: --peer-mac required for ping mode\n");
            return 1;
        }

        fmt::print("Ping client starting on {} (MAC: {})\n", iface.name(), iface.mac());
        fmt::print("  Target: {}\n", cfg.peer_mac);
        fmt::print("  Payload size: {} bytes\n", cfg.payload_size);
        fmt::print("  Count: {}\n", cfg.count == 0 ? "infinite" : std::to_string(cfg.count));
        fmt::print("  Interval: {} us\n", cfg.interval_us);
        if (cfg.use_vlan)
        {
            fmt::print("  VLAN ID: {}, Priority: {}\n", cfg.vlan_id, cfg.vlan_priority);
        }
        fmt::print("\n");

        auto sock_result = l2net::raw_socket::create_bound(iface, l2net::raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            fmt::print(stderr, "Error creating socket: {}\n", sock_result.error());
            return 1;
        }
        auto &sock = *sock_result;

        // build payload with sequence number space
        // belt-and-suspenders: gcc's static analyzer needs local proof
        if (cfg.payload_size == 0) [[unlikely]]
        {
            fmt::print(stderr, "Error: payload size must be at least 1\n");
            return 1;
        }
        std::vector<std::uint8_t> payload(cfg.payload_size);
        payload[0] = proto::msg_ping;

        std::vector<std::uint8_t> recv_buffer(65536);
        benchmark_stats stats{};
        std::vector<std::chrono::nanoseconds> latencies;
        latencies.reserve(cfg.count > 0 ? cfg.count : 10000);

        stats.start_time = std::chrono::steady_clock::now();
        std::uint64_t seq = 0;

        while (g_running.load() && (cfg.count == 0 || seq < cfg.count))
        {
            // embed sequence number in payload
            if (payload.size() >= 9)
            {
                std::memcpy(payload.data() + 1, &seq, sizeof(seq));
            }

            auto frame_result = build_frame(cfg.peer_mac, iface.mac(), payload, cfg);
            if (!frame_result.has_value())
            {
                fmt::print(stderr, "Error building frame: {}\n", frame_result.error());
                continue;
            }

            auto const send_time = std::chrono::steady_clock::now();

            auto send_result = send_with_retry(sock, *frame_result, iface);
            if (!send_result.has_value())
            {
                fmt::print(stderr, "Send error: {}\n", send_result.error());
                continue;
            }

            stats.packets_sent++;
            stats.bytes_sent += *send_result;

            // wait for response
            bool got_response = false;
            auto const deadline = send_time + std::chrono::milliseconds{cfg.timeout_ms};

            while (std::chrono::steady_clock::now() < deadline)
            {
                auto const remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

                if (remaining.count() <= 0)
                {
                    break;
                }

                auto recv_result = sock.receive_with_timeout(recv_buffer, remaining);
                if (!recv_result.has_value())
                {
                    if (recv_result.error() == l2net::error_code::timeout)
                    {
                        break;
                    }
                    continue;
                }

                auto const recv_time = std::chrono::steady_clock::now();

                l2net::frame_parser parser{std::span{recv_buffer.data(), *recv_result}};
                if (!parser.is_valid())
                {
                    continue;
                }
                if (parser.ether_type() != proto::eth_p_bench)
                {
                    continue;
                }

                auto const resp_payload = parser.payload();
                if (resp_payload.empty() || resp_payload[0] != proto::msg_pong)
                {
                    continue;
                }

                // check sequence number
                if (resp_payload.size() >= 9)
                {
                    std::uint64_t resp_seq = 0;
                    std::memcpy(&resp_seq, resp_payload.data() + 1, sizeof(resp_seq));
                    if (resp_seq != seq)
                    {
                        continue;
                    }
                }

                auto const latency = recv_time - send_time;
                latencies.push_back(latency);
                stats.packets_received++;
                stats.bytes_received += *recv_result;
                stats.total_latency += latency;

                if (!cfg.quiet)
                {
                    auto const latency_us = std::chrono::duration_cast<std::chrono::microseconds>(latency);
                    fmt::print("{} bytes from {}: seq={} time={} us\n", resp_payload.size(), parser.src_mac(), seq,
                               latency_us.count());
                }

                got_response = true;
                break;
            }

            if (!got_response && !cfg.quiet)
            {
                fmt::print("Request timeout for seq={}\n", seq);
            }

            ++seq;

            if (cfg.interval_us > 0 && g_running.load())
            {
                std::this_thread::sleep_for(std::chrono::microseconds{cfg.interval_us});
            }
        }

        stats.end_time = std::chrono::steady_clock::now();

        // calculate statistics
        fmt::print("\n--- Ping Statistics ---\n");
        fmt::print("{} packets transmitted, {} received, {:.1f}% packet loss\n", stats.packets_sent,
                   stats.packets_received,
                   stats.packets_sent > 0 ? 100.0 * static_cast<double>(stats.packets_sent - stats.packets_received) /
                                                static_cast<double>(stats.packets_sent)
                                          : 0.0);

        if (!latencies.empty())
        {
            std::sort(latencies.begin(), latencies.end());

            auto const min_lat = std::chrono::duration_cast<std::chrono::microseconds>(latencies.front());
            auto const max_lat = std::chrono::duration_cast<std::chrono::microseconds>(latencies.back());
            auto const avg_lat = std::chrono::duration_cast<std::chrono::microseconds>(
                stats.total_latency / static_cast<long>(latencies.size()));
            auto const p50 = std::chrono::duration_cast<std::chrono::microseconds>(latencies[latencies.size() / 2]);
            auto const p99 =
                std::chrono::duration_cast<std::chrono::microseconds>(latencies[latencies.size() * 99 / 100]);

            fmt::print("rtt min/avg/max/p50/p99 = {}/{}/{}/{}/{} us\n", min_lat.count(), avg_lat.count(),
                       max_lat.count(), p50.count(), p99.count());
        }

        return stats.packets_received > 0 ? 0 : 1;
    }

    // =============================================================================
    // flood client - sends packets as fast as possible for throughput testing
    // =============================================================================
    auto run_flood_client(l2net::interface_info const &iface, config const &cfg) -> int
    {
        if (cfg.peer_mac.is_null())
        {
            fmt::print(stderr, "Error: --peer-mac required for flood mode\n");
            return 1;
        }

        fmt::print("Flood client starting on {} (MAC: {})\n", iface.name(), iface.mac());
        fmt::print("  Target: {}\n", cfg.peer_mac);
        fmt::print("  Payload size: {} bytes\n", cfg.payload_size);
        fmt::print("  Count: {}\n", cfg.count == 0 ? "infinite" : std::to_string(cfg.count));
        if (cfg.use_vlan)
        {
            fmt::print("  VLAN ID: {}, Priority: {}\n", cfg.vlan_id, cfg.vlan_priority);
        }
        fmt::print("\n");

        auto sock_result = l2net::raw_socket::create_bound(iface, l2net::raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            fmt::print(stderr, "Error creating socket: {}\n", sock_result.error());
            return 1;
        }
        auto &sock = *sock_result;

        // pre-build frame for maximum performance
        // pre-build frame for maximum performance
        // belt-and-suspenders: gcc's static analyzer needs local proof
        if (cfg.payload_size == 0) [[unlikely]]
        {
            fmt::print(stderr, "Error: payload size must be at least 1\n");
            return 1;
        }
        std::vector<std::uint8_t> payload(cfg.payload_size, 0x42);
        payload[0] = proto::msg_data;

        auto frame_result = build_frame(cfg.peer_mac, iface.mac(), payload, cfg);
        if (!frame_result.has_value())
        {
            fmt::print(stderr, "Error building frame: {}\n", frame_result.error());
            return 1;
        }
        auto const &frame = *frame_result;

        benchmark_stats stats{};
        stats.start_time = std::chrono::steady_clock::now();

        auto last_report = stats.start_time;
        std::uint64_t last_packets = 0;
        std::uint64_t last_bytes = 0;

        while (g_running.load() && (cfg.count == 0 || stats.packets_sent < cfg.count))
        {
            auto send_result = sock.send_raw(frame, iface);
            if (send_result.has_value())
            {
                stats.packets_sent++;
                stats.bytes_sent += *send_result;
            }

            // periodic stats report
            if ((stats.packets_sent % 10000) == 0)
            {
                auto const now = std::chrono::steady_clock::now();
                auto const interval = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report);

                if (interval.count() >= 1000)
                {
                    auto const pps =
                        (stats.packets_sent - last_packets) * 1000 / static_cast<std::uint64_t>(interval.count());
                    auto const mbps =
                        (stats.bytes_sent - last_bytes) * 8 / static_cast<std::uint64_t>(interval.count()) / 1000;
                    fmt::print("Sent {} packets ({} Mbps, {} pps)\n", stats.packets_sent, mbps, pps);

                    last_report = now;
                    last_packets = stats.packets_sent;
                    last_bytes = stats.bytes_sent;
                }
            }

            if (cfg.interval_us > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds{cfg.interval_us});
            }
        }

        stats.end_time = std::chrono::steady_clock::now();
        auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(stats.end_time - stats.start_time);

        fmt::print("\n--- Flood Statistics ---\n");
        fmt::print("Packets sent: {}\n", stats.packets_sent);
        fmt::print("Bytes sent: {}\n", stats.bytes_sent);
        fmt::print("Duration: {} ms\n", duration.count());

        if (duration.count() > 0)
        {
            auto const pps = stats.packets_sent * 1000 / static_cast<std::uint64_t>(duration.count());
            auto const mbps = stats.bytes_sent * 8 / static_cast<std::uint64_t>(duration.count()) / 1000;
            fmt::print("Average: {} pps, {} Mbps\n", pps, mbps);
        }

        return 0;
    }

} // anonymous namespace

auto main(int const argc, char const *argv[]) -> int
{
    auto const cfg_opt = parse_args(argc, argv);
    if (!cfg_opt.has_value())
    {
        print_usage(argv[0]);
        return 1;
    }
    auto const &cfg = *cfg_opt;

    // setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // get interface info
    auto iface_result = l2net::interface_info::query(cfg.interface_name);
    if (!iface_result.has_value())
    {
        fmt::print(stderr, "Error: interface '{}' not found\n", cfg.interface_name);
        return 1;
    }
    auto const &iface = *iface_result;

    if (!iface.is_up())
    {
        fmt::print(stderr, "Error: interface '{}' is not up\n", cfg.interface_name);
        return 1;
    }

    // dispatch to appropriate mode
    if (cfg.mode == "echo")
    {
        return run_echo_server(iface, cfg);
    }
    if (cfg.mode == "sink")
    {
        return run_sink_server(iface, cfg);
    }
    if (cfg.mode == "ping")
    {
        return run_ping_client(iface, cfg);
    }
    if (cfg.mode == "flood")
    {
        return run_flood_client(iface, cfg);
    }

    fmt::print(stderr, "Error: unknown mode '{}'\n", cfg.mode);
    print_usage(argv[0]);
    return 1;
}
