#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtautological-compare"
#include <fmt/ranges.h>
#pragma GCC diagnostic pop
#include <array>
#include <cstdint>
#include <expected>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace l2net
{

    // ============================================================================
    // error handling - because exceptions are for amateurs in real-time systems
    // ============================================================================

    enum class error_code : std::uint8_t
    {
        success = 0,
        socket_creation_failed,
        socket_bind_failed,
        socket_send_failed,
        socket_recv_failed,
        interface_not_found,
        interface_query_failed,
        invalid_mac_address,
        invalid_frame_size,
        invalid_vlan_id,
        invalid_priority,
        connection_failed,
        handshake_failed,
        permission_denied,
        buffer_too_small,
        timeout,
    };

    struct error_code_formatter
    {
        [[nodiscard]] static constexpr auto to_string(error_code const ec) noexcept
            -> std::string_view
        {
            switch (ec)
            {
            case error_code::success:
                return "success";
            case error_code::socket_creation_failed:
                return "socket_creation_failed";
            case error_code::socket_bind_failed:
                return "socket_bind_failed";
            case error_code::socket_send_failed:
                return "socket_send_failed";
            case error_code::socket_recv_failed:
                return "socket_recv_failed";
            case error_code::interface_not_found:
                return "interface_not_found";
            case error_code::interface_query_failed:
                return "interface_query_failed";
            case error_code::invalid_mac_address:
                return "invalid_mac_address";
            case error_code::invalid_frame_size:
                return "invalid_frame_size";
            case error_code::invalid_vlan_id:
                return "invalid_vlan_id";
            case error_code::invalid_priority:
                return "invalid_priority";
            case error_code::connection_failed:
                return "connection_failed";
            case error_code::handshake_failed:
                return "handshake_failed";
            case error_code::permission_denied:
                return "permission_denied";
            case error_code::buffer_too_small:
                return "buffer_too_small";
            case error_code::timeout:
                return "timeout";
            }
            return "unknown_error";
        }
    };

    template <typename T>
    using result = std::expected<T, error_code>;

    using void_result = std::expected<void, error_code>;

    class mac_address
    {
    public:
        static constexpr std::size_t size = 6;
        using storage_type = std::array<std::uint8_t, size>;

    private:
        storage_type bytes_{};

    public:
        constexpr mac_address() noexcept = default;
        constexpr mac_address(mac_address const &) noexcept = default;
        constexpr mac_address(mac_address &&) noexcept = default;
        constexpr auto operator=(mac_address const &) noexcept -> mac_address & = default;
        constexpr auto operator=(mac_address &&) noexcept -> mac_address & = default;
        constexpr ~mac_address() noexcept = default;

        constexpr explicit mac_address(storage_type const &bytes) noexcept : bytes_{bytes} {}

        constexpr mac_address(
            std::uint8_t const b0, std::uint8_t const b1, std::uint8_t const b2,
            std::uint8_t const b3, std::uint8_t const b4, std::uint8_t const b5) noexcept : bytes_{b0, b1, b2, b3, b4, b5} {}

        [[nodiscard]] static auto from_string(std::string_view str) noexcept -> result<mac_address>;

        [[nodiscard]] constexpr auto data() noexcept -> std::uint8_t * { return bytes_.data(); }
        [[nodiscard]] constexpr auto data() const noexcept -> std::uint8_t const * { return bytes_.data(); }
        [[nodiscard]] constexpr auto bytes() const noexcept -> storage_type const & { return bytes_; }
        [[nodiscard]] constexpr auto as_span() noexcept -> std::span<std::uint8_t, size> { return bytes_; }
        [[nodiscard]] constexpr auto as_span() const noexcept -> std::span<std::uint8_t const, size> { return bytes_; }

        [[nodiscard]] auto to_string() const -> std::string;

        [[nodiscard]] static consteval auto broadcast() noexcept -> mac_address
        {
            return mac_address{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        }

        [[nodiscard]] static consteval auto null() noexcept -> mac_address
        {
            return mac_address{};
        }

        [[nodiscard]] constexpr auto is_broadcast() const noexcept -> bool
        {
            return bytes_ == broadcast().bytes_;
        }

        [[nodiscard]] constexpr auto is_null() const noexcept -> bool
        {
            return bytes_ == null().bytes_;
        }

        [[nodiscard]] constexpr auto is_multicast() const noexcept -> bool
        {
            return (bytes_[0] & 0x01) != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(mac_address const &) const noexcept = default;

        friend auto format_as(mac_address const &mac) -> std::string
        {
            return mac.to_string();
        }
    };

    namespace constants
    {
        inline constexpr std::uint16_t eth_header_size = 14;
        inline constexpr std::uint16_t vlan_header_size = 4;
        inline constexpr std::uint16_t eth_vlan_header_size = eth_header_size + vlan_header_size;
        inline constexpr std::uint16_t min_frame_size = 64;
        inline constexpr std::uint16_t max_frame_size = 1518;
        inline constexpr std::uint16_t max_jumbo_frame_size = 9000;
        // FIXED: Changed to uint32_t to avoid overflow
        inline constexpr std::uint32_t loopback_mtu = 65536;

        inline constexpr std::uint16_t eth_p_8021q = 0x8100;
        inline constexpr std::uint16_t eth_p_custom = 0x88B5;
        inline constexpr std::uint16_t eth_p_ipc = 0xAAAA;

        inline constexpr std::uint16_t max_vlan_id = 4095;
        inline constexpr std::uint8_t max_priority = 7;
    }

    namespace byte_utils
    {
        [[nodiscard]] constexpr auto htons_constexpr(std::uint16_t const value) noexcept -> std::uint16_t
        {
            if constexpr (std::endian::native == std::endian::little)
            {
                return static_cast<std::uint16_t>((value >> 8) | (value << 8));
            }
            else
            {
                return value;
            }
        }

        [[nodiscard]] constexpr auto ntohs_constexpr(std::uint16_t const value) noexcept -> std::uint16_t
        {
            return htons_constexpr(value);
        }
    }

} // namespace l2net

template <>
struct fmt::formatter<l2net::error_code> : fmt::formatter<std::string_view>
{
    auto format(l2net::error_code const ec, format_context &ctx) const
    {
        return fmt::formatter<std::string_view>::format(
            l2net::error_code_formatter::to_string(ec), ctx);
    }
};