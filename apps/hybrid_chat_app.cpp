// hybrid_chat_app.cpp - hybrid chat application entry point
// the part users actually run, now without the embarrassing code

#include "l2net/hybrid_chat.hpp"
#include "l2net/interface.hpp"

#include <fmt/format.h>
#include <csignal>
#include <atomic>

namespace {

std::atomic<bool> g_running{true};

auto signal_handler(int /*signal*/) -> void {
    g_running.store(false);
}

auto print_usage(char const* program_name) -> void {
    fmt::print(stderr,
        "Usage: sudo {} <interface> <mode> [server_ip]\n"
        "  Server: sudo {} eth0 server\n"
        "  Client: sudo {} eth0 client 192.168.1.50\n",
        program_name, program_name, program_name);
}

auto run_server(l2net::interface_info const& iface) -> int {
    fmt::print("[Control Plane] Starting server on interface {}...\n", iface.name());
    fmt::print("[Control Plane] Local MAC: {}\n", iface.mac());

    auto endpoint_result = l2net::hybrid_endpoint::create_server(iface);
    if (!endpoint_result.has_value()) {
        fmt::print(stderr, "Error creating server: {}\n", endpoint_result.error());
        return 1;
    }

    auto& endpoint = *endpoint_result;
    fmt::print("[Control Plane] Handshake complete. Client MAC: {}\n", endpoint.peer());
    fmt::print("[Data Plane] Listening for data...\n");

    // receive loop
    while (g_running.load()) {
        auto msg_result = endpoint.receive_data();
        if (!msg_result.has_value()) {
            if (msg_result.error() == l2net::error_code::timeout) {
                continue;
            }
            // silently ignore non-matching frames
            continue;
        }

        auto const& msg = *msg_result;
        if (msg.was_tagged) {
            fmt::print("Recv [VLAN {} Prio {}]: {}\n",
                msg.vlan_id, msg.priority,
                std::string_view{reinterpret_cast<char const*>(msg.payload.data()),
                    std::min(msg.payload.size(), std::size_t{50})});
        } else {
            fmt::print("Recv [Untagged]: {}\n",
                std::string_view{reinterpret_cast<char const*>(msg.payload.data()),
                    std::min(msg.payload.size(), std::size_t{50})});
        }
    }

    return 0;
}

auto run_client(l2net::interface_info const& iface, std::string_view server_ip) -> int {
    fmt::print("[Control Plane] Connecting to {}...\n", server_ip);
    fmt::print("[Control Plane] Local MAC: {}\n", iface.mac());

    auto endpoint_result = l2net::hybrid_endpoint::create_client(iface, server_ip);
    if (!endpoint_result.has_value()) {
        fmt::print(stderr, "Error creating client: {}\n", endpoint_result.error());
        return 1;
    }

    auto& endpoint = *endpoint_result;
    fmt::print("[Control Plane] Handshake complete. Server MAC: {}\n", endpoint.peer());
    fmt::print("[Data Plane] Sending VLAN tagged frames with Priority {}...\n",
        endpoint.config().vlan_priority);

    // send loop
    std::string const message = "HIGH PRIORITY DATA";
    while (g_running.load()) {
        auto result = endpoint.send_data(message);
        if (!result.has_value()) {
            fmt::print(stderr, "Send error: {}\n", result.error());
        }
        std::this_thread::sleep_for(endpoint.config().send_interval);
    }

    return 0;
}

} // anonymous namespace

auto main(int const argc, char const* argv[]) -> int {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    // setup signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // get interface info
    auto iface_result = l2net::interface_info::query(argv[1]);
    if (!iface_result.has_value()) {
        fmt::print(stderr, "Error: interface '{}' not found\n", argv[1]);
        return 1;
    }

    std::string_view const mode{argv[2]};

    if (mode == "server") {
        return run_server(*iface_result);
    }

    if (mode == "client") {
        if (argc < 4) {
            fmt::print(stderr, "Error: client mode requires server IP\n");
            return 1;
        }
        return run_client(*iface_result, argv[3]);
    }

    fmt::print(stderr, "Error: unknown mode '{}'\n", mode);
    return 1;
}
