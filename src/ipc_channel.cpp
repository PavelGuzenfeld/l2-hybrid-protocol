// ipc_channel.cpp - local IPC over L2 loopback
// high-performance local messaging without the shmem complexity

#include "l2net/ipc_channel.hpp"

#include <poll.h>
#include <algorithm>

namespace l2net {

// ============================================================================
// ipc_channel implementation
// ============================================================================

ipc_channel::ipc_channel(
    raw_socket socket,
    interface_info iface,
    ipc_config config
) noexcept
    : socket_{std::move(socket)}
    , interface_{std::move(iface)}
    , config_{config}
    , recv_buffer_(config.recv_buffer_size)
{}

auto ipc_channel::create(ipc_config const& config) noexcept -> result<ipc_channel> {
    // get interface info
    auto iface_result = interface_info::query(config.interface_name);
    if (!iface_result.has_value()) {
        // try loopback as fallback
        iface_result = get_loopback_interface();
        if (!iface_result.has_value()) {
            return tl::unexpected{iface_result.error()};
        }
    }

    // create socket with our protocol
    auto sock_result = raw_socket::create(
        static_cast<raw_socket::protocol>(config.protocol_id));
    if (!sock_result.has_value()) {
        return tl::unexpected{sock_result.error()};
    }

    // bind to interface
    auto bind_result = sock_result->bind(*iface_result);
    if (!bind_result.has_value()) {
        return tl::unexpected{bind_result.error()};
    }

    // set timeout if specified
    if (config.recv_timeout.has_value()) {
        socket_options opts;
        opts.recv_timeout = config.recv_timeout;
        auto opt_result = sock_result->set_options(opts);
        if (!opt_result.has_value()) {
            return tl::unexpected{opt_result.error()};
        }
    }

    return ipc_channel{
        std::move(*sock_result),
        std::move(*iface_result),
        config
    };
}

auto ipc_channel::send(std::span<std::uint8_t const> data) noexcept -> result<std::size_t> {
    // build frame: header + payload
    // for loopback, MACs don't matter but must be present
    auto frame_result = build_simple_frame(
        mac_address::null(),  // dest - ignored on loopback
        mac_address::null(),  // src - ignored on loopback
        config_.protocol_id,
        data
    );

    if (!frame_result.has_value()) {
        return tl::unexpected{frame_result.error()};
    }

    return socket_.send_raw(*frame_result, interface_);
}

auto ipc_channel::send(std::string_view const message) noexcept -> result<std::size_t> {
    return send(std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const*>(message.data()),
        message.size()
    });
}

auto ipc_channel::receive() noexcept -> result<std::vector<std::uint8_t>> {
    auto recv_result = socket_.receive(recv_buffer_);
    if (!recv_result.has_value()) {
        return tl::unexpected{recv_result.error()};
    }

    auto const received = *recv_result;

    // parse frame
    frame_parser parser{std::span{recv_buffer_.data(), received}};
    if (!parser.is_valid()) {
        return tl::unexpected{error_code::invalid_frame_size};
    }

    // check protocol
    if (parser.ether_type() != config_.protocol_id) {
        // not our protocol, could retry but for now return empty
        return std::vector<std::uint8_t>{};
    }

    // extract payload
    auto const payload = parser.payload();
    return std::vector<std::uint8_t>{payload.begin(), payload.end()};
}

auto ipc_channel::receive_with_timeout(std::chrono::milliseconds const timeout) noexcept
    -> result<std::vector<std::uint8_t>>
{
    auto recv_result = socket_.receive_with_timeout(recv_buffer_, timeout);
    if (!recv_result.has_value()) {
        return tl::unexpected{recv_result.error()};
    }

    auto const received = *recv_result;

    frame_parser parser{std::span{recv_buffer_.data(), received}};
    if (!parser.is_valid()) {
        return tl::unexpected{error_code::invalid_frame_size};
    }

    if (parser.ether_type() != config_.protocol_id) {
        return std::vector<std::uint8_t>{};
    }

    auto const payload = parser.payload();
    return std::vector<std::uint8_t>{payload.begin(), payload.end()};
}

auto ipc_channel::try_receive() noexcept
    -> result<std::optional<std::vector<std::uint8_t>>>
{
    auto result = receive_with_timeout(std::chrono::milliseconds{0});

    if (!result.has_value()) {
        if (result.error() == error_code::timeout) {
            return std::optional<std::vector<std::uint8_t>>{std::nullopt};
        }
        return tl::unexpected{result.error()};
    }

    return std::optional{std::move(*result)};
}

auto ipc_channel::receive_loop(message_callback const& callback) noexcept -> void_result {
    while (true) {
        auto result = receive();
        if (!result.has_value()) {
            return tl::unexpected{result.error()};
        }

        if (!result->empty()) {
            callback(*result);
        }
    }
}

// ============================================================================
// ipc pair creation
// ============================================================================

auto create_ipc_pair(ipc_config const& config) noexcept -> result<ipc_pair> {
    auto sender = ipc_channel::create(config);
    if (!sender.has_value()) {
        return tl::unexpected{sender.error()};
    }

    auto receiver = ipc_channel::create(config);
    if (!receiver.has_value()) {
        return tl::unexpected{receiver.error()};
    }

    return ipc_pair{
        std::move(*sender),
        std::move(*receiver)
    };
}

} // namespace l2net
