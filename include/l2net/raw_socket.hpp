#pragma once

// raw_socket.hpp - RAII wrapper for raw sockets
// because your naked file descriptors give me nightmares

#include "common.hpp"
#include "interface.hpp"

#include <chrono>
#include <span>
#include <optional>

namespace l2net {

// ============================================================================
// socket options - because magic numbers in setsockopt are disgusting
// ============================================================================

struct socket_options {
    std::optional<std::chrono::milliseconds> recv_timeout{};
    std::optional<std::chrono::milliseconds> send_timeout{};
    bool reuse_addr{false};
    bool broadcast{false};
    std::optional<int> recv_buffer_size{};
    std::optional<int> send_buffer_size{};
};

// ============================================================================
// raw socket - the star of the show, now with proper resource management
// ============================================================================

class raw_socket {
public:
    // socket protocol types
    enum class protocol : std::uint16_t {
        all = 0x0003,      // ETH_P_ALL - receive everything
        custom = 0x88B5,   // our custom protocol
        ipc = 0xAAAA,      // ipc protocol
        vlan = 0x8100,     // 802.1Q
    };

private:
    int fd_{-1};
    protocol proto_{protocol::all};
    std::optional<interface_info> bound_interface_{};

public:
    // rule of 5 - because we own a resource
    raw_socket() noexcept = default;
    ~raw_socket() noexcept;

    // no copying - sockets are unique resources, unlike your copy-paste code
    raw_socket(raw_socket const&) = delete;
    auto operator=(raw_socket const&) -> raw_socket& = delete;

    // move semantics - the civilized way to transfer ownership
    raw_socket(raw_socket&& other) noexcept;
    auto operator=(raw_socket&& other) noexcept -> raw_socket&;

    // factory functions - because constructors that can fail are stupid
    [[nodiscard]] static auto create(protocol proto = protocol::all) noexcept
        -> result<raw_socket>;

    [[nodiscard]] static auto create_bound(
        interface_info const& iface,
        protocol proto = protocol::all
    ) noexcept -> result<raw_socket>;

    // socket operations
    [[nodiscard]] auto bind(interface_info const& iface) noexcept -> void_result;
    [[nodiscard]] auto set_options(socket_options const& opts) noexcept -> void_result;

    // send/receive - the whole point of this exercise
    [[nodiscard]] auto send_to(
        std::span<std::uint8_t const> data,
        interface_info const& iface,
        mac_address const& dest_mac
    ) noexcept -> result<std::size_t>;

    [[nodiscard]] auto send_raw(
        std::span<std::uint8_t const> data,
        interface_info const& iface
    ) noexcept -> result<std::size_t>;

    [[nodiscard]] auto receive(
        std::span<std::uint8_t> buffer
    ) noexcept -> result<std::size_t>;

    [[nodiscard]] auto receive_with_timeout(
        std::span<std::uint8_t> buffer,
        std::chrono::milliseconds timeout
    ) noexcept -> result<std::size_t>;

    // state queries
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }
    [[nodiscard]] constexpr auto fd() const noexcept -> int { return fd_; }
    [[nodiscard]] constexpr auto protocol_type() const noexcept -> protocol { return proto_; }
    [[nodiscard]] auto bound_interface() const noexcept -> std::optional<interface_info> const& {
        return bound_interface_;
    }

    // explicit close
    auto close() noexcept -> void;

private:
    // internal constructor for factory
    explicit raw_socket(int fd, protocol proto) noexcept;
};

// ============================================================================
// tcp socket for control plane - minimal wrapper for handshake
// ============================================================================

class tcp_socket {
private:
    int fd_{-1};

public:
    tcp_socket() noexcept = default;
    ~tcp_socket() noexcept;

    tcp_socket(tcp_socket const&) = delete;
    auto operator=(tcp_socket const&) -> tcp_socket& = delete;

    tcp_socket(tcp_socket&& other) noexcept;
    auto operator=(tcp_socket&& other) noexcept -> tcp_socket&;

    // server operations
    [[nodiscard]] static auto create_server(std::uint16_t port) noexcept
        -> result<tcp_socket>;

    [[nodiscard]] auto accept() noexcept -> result<tcp_socket>;

    // client operations
    [[nodiscard]] static auto connect(
        std::string_view ip,
        std::uint16_t port,
        std::chrono::seconds timeout = std::chrono::seconds{10}
    ) noexcept -> result<tcp_socket>;

    // data transfer
    [[nodiscard]] auto send(std::span<std::uint8_t const> data) noexcept
        -> result<std::size_t>;

    [[nodiscard]] auto receive(std::span<std::uint8_t> buffer) noexcept
        -> result<std::size_t>;

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }

    auto close() noexcept -> void;

private:
    explicit tcp_socket(int fd) noexcept;
};

} // namespace l2net
