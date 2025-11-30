#pragma once

#include "common.hpp"
#include "interface.hpp" // FIXED: Added this import

#include <chrono>
#include <cstdint> // FIXED: Added cstdint
#include <optional>
#include <span> // FIXED: Added span

namespace l2net
{

    struct socket_options
    {
        std::optional<std::chrono::milliseconds> recv_timeout{};
        std::optional<std::chrono::milliseconds> send_timeout{};
        bool reuse_addr{false};
        bool broadcast{false};
        std::optional<int> recv_buffer_size{};
        std::optional<int> send_buffer_size{};
    };

    class raw_socket
    {
    public:
        enum class protocol : std::uint16_t
        {
            all = 0x0003,
            custom = 0x88B5,
            ipc = 0xAAAA,
            vlan = 0x8100,
        };

    private:
        int fd_{-1};
        protocol proto_{protocol::all};
        std::optional<interface_info> bound_interface_{};

    public:
        raw_socket() noexcept = default;
        ~raw_socket() noexcept;

        raw_socket(raw_socket const &) = delete;
        auto operator=(raw_socket const &) -> raw_socket & = delete;

        raw_socket(raw_socket &&other) noexcept;
        auto operator=(raw_socket &&other) noexcept -> raw_socket &;

        [[nodiscard]] static auto create(protocol proto = protocol::all) noexcept -> result<raw_socket>;

        [[nodiscard]] static auto create_bound(
            interface_info const &iface,
            protocol proto = protocol::all) noexcept -> result<raw_socket>;

        [[nodiscard]] auto bind(interface_info const &iface) noexcept -> void_result;
        [[nodiscard]] auto set_options(socket_options const &opts) noexcept -> void_result;

        [[nodiscard]] auto send_to(
            std::span<std::uint8_t const> data,
            interface_info const &iface,
            mac_address const &dest_mac) noexcept -> result<std::size_t>;

        [[nodiscard]] auto send_raw(
            std::span<std::uint8_t const> data,
            interface_info const &iface) noexcept -> result<std::size_t>;

        [[nodiscard]] auto receive(
            std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>;

        [[nodiscard]] auto receive_with_timeout(
            std::span<std::uint8_t> buffer,
            std::chrono::milliseconds timeout) noexcept -> result<std::size_t>;

        [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }
        [[nodiscard]] constexpr auto fd() const noexcept -> int { return fd_; }
        [[nodiscard]] constexpr auto protocol_type() const noexcept -> protocol { return proto_; }
        [[nodiscard]] auto bound_interface() const noexcept -> std::optional<interface_info> const &
        {
            return bound_interface_;
        }

        auto close() noexcept -> void;

    private:
        explicit raw_socket(int fd, protocol proto) noexcept;
    };

    class tcp_socket
    {
    private:
        int fd_{-1};

    public:
        tcp_socket() noexcept = default;
        ~tcp_socket() noexcept;

        tcp_socket(tcp_socket const &) = delete;
        auto operator=(tcp_socket const &) -> tcp_socket & = delete;

        tcp_socket(tcp_socket &&other) noexcept;
        auto operator=(tcp_socket &&other) noexcept -> tcp_socket &;

        [[nodiscard]] static auto create_server(std::uint16_t port) noexcept -> result<tcp_socket>;
        [[nodiscard]] auto accept() noexcept -> result<tcp_socket>;

        [[nodiscard]] static auto connect(
            std::string_view ip,
            std::uint16_t port,
            std::chrono::seconds timeout = std::chrono::seconds{10}) noexcept -> result<tcp_socket>;

        [[nodiscard]] auto send(std::span<std::uint8_t const> data) noexcept -> result<std::size_t>;
        [[nodiscard]] auto receive(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>;

        [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return fd_ >= 0; }

        auto close() noexcept -> void;

    private:
        explicit tcp_socket(int fd) noexcept;
    };

} // namespace l2net