// ipc_l2_app.cpp - L2 IPC application entry point
// local messaging that actually demonstrates proper c++

#include "l2net/ipc_channel.hpp"

#include <fmt/format.h>
#include <csignal>
#include <atomic>
#include <algorithm>

namespace {

std::atomic<bool> g_running{true};

auto signal_handler(int /*signal*/) -> void {
    g_running.store(false);
}

auto print_usage(char const* program_name) -> void {
    fmt::print(stderr, "Usage: sudo {} <send|recv>\n", program_name);
}

auto run_receiver() -> int {
    fmt::print("IPC Receiver: Creating channel on loopback...\n");

    auto channel_result = l2net::ipc_channel::create();
    if (!channel_result.has_value()) {
        fmt::print(stderr, "Error creating channel: {}\n", channel_result.error());
        return 1;
    }

    auto& channel = *channel_result;
    fmt::print("IPC Receiver: Listening on Proto 0x{:04X}...\n",
        l2net::constants::eth_p_ipc);

    while (g_running.load()) {
        auto msg_result = channel.receive_with_timeout(std::chrono::milliseconds{100});

        if (!msg_result.has_value()) {
            if (msg_result.error() == l2net::error_code::timeout) {
                continue;
            }
            fmt::print(stderr, "Receive error: {}\n", msg_result.error());
            continue;
        }

        auto const& msg = *msg_result;
        if (!msg.empty()) {
            auto const preview_len = std::min(msg.size(), std::size_t{50});
            fmt::print("Got {} bytes: {}...\n",
                msg.size(),
                std::string_view{reinterpret_cast<char const*>(msg.data()), preview_len});
        }
    }

    return 0;
}

auto run_sender() -> int {
    fmt::print("IPC Sender: Creating channel on loopback...\n");

    auto channel_result = l2net::ipc_channel::create();
    if (!channel_result.has_value()) {
        fmt::print(stderr, "Error creating channel: {}\n", channel_result.error());
        return 1;
    }

    auto& channel = *channel_result;

    std::string const message = "High performance L2 IPC message";

    auto send_result = channel.send(message);
    if (!send_result.has_value()) {
        fmt::print(stderr, "Send error: {}\n", send_result.error());
        return 1;
    }

    fmt::print("Message sent via Loopback L2 ({} bytes).\n", *send_result);
    return 0;
}

} // anonymous namespace

auto main(int const argc, char const* argv[]) -> int {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string_view const mode{argv[1]};

    if (mode == "recv") {
        return run_receiver();
    }

    if (mode == "send") {
        return run_sender();
    }

    fmt::print(stderr, "Error: unknown mode '{}'\n", mode);
    return 1;
}
