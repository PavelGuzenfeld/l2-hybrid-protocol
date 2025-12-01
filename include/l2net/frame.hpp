#pragma once

// frame.hpp - ethernet frame construction and parsing
// because your pointer arithmetic made my eyes bleed

#include "common.hpp"

#include <span>
#include <vector>

namespace l2net
{

    // ============================================================================
    // ethernet header - properly packed, unlike your original mess
    // ============================================================================

    struct ethernet_header
    {
        std::array<std::uint8_t, 6> dest_mac;
        std::array<std::uint8_t, 6> src_mac;
        std::uint16_t ether_type; // in network byte order

        [[nodiscard]] constexpr auto dest() const noexcept -> mac_address
        {
            return mac_address{dest_mac};
        }

        [[nodiscard]] constexpr auto src() const noexcept -> mac_address
        {
            return mac_address{src_mac};
        }

        [[nodiscard]] constexpr auto type() const noexcept -> std::uint16_t
        {
            return byte_utils::ntohs_constexpr(ether_type);
        }
    } __attribute__((packed));

    static_assert(sizeof(ethernet_header) == 14, "ethernet header must be 14 bytes you absolute donut");

    // ============================================================================
    // frame builder - fluent interface for frame construction
    // ============================================================================

    class frame_builder
    {
    private:
        std::vector<std::uint8_t> buffer_{};
        mac_address dest_mac_{};
        mac_address src_mac_{};
        std::uint16_t ether_type_{0};
        bool finalized_{false};

    public:
        frame_builder() = default;

        // fluent setters - because builder pattern is actually good sometimes
        [[nodiscard]] auto set_dest(mac_address const &mac) noexcept -> frame_builder &
        {
            dest_mac_ = mac;
            return *this;
        }

        [[nodiscard]] auto set_src(mac_address const &mac) noexcept -> frame_builder &
        {
            src_mac_ = mac;
            return *this;
        }

        [[nodiscard]] auto set_ether_type(std::uint16_t type) noexcept -> frame_builder &
        {
            ether_type_ = type;
            return *this;
        }

        [[nodiscard]] auto set_payload(std::span<std::uint8_t const> data) noexcept -> frame_builder &
        {
            buffer_.assign(data.begin(), data.end());
            return *this;
        }

        [[nodiscard]] auto set_payload(std::string_view data) noexcept -> frame_builder &
        {
            buffer_.assign(data.begin(), data.end());
            return *this;
        }

        // build the frame
        [[nodiscard]] auto build() noexcept -> result<std::vector<std::uint8_t>>;

        // build into existing buffer (for zero-copy scenarios)
        [[nodiscard]] auto build_into(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>;

        // reset for reuse
        auto reset() noexcept -> void;

        // get required size before building
        [[nodiscard]] constexpr auto required_size() const noexcept -> std::size_t
        {
            return constants::eth_header_size + buffer_.size();
        }
    };

    // ============================================================================
    // frame parser - for reading received frames
    // ============================================================================

    class frame_parser
    {
    private:
        std::span<std::uint8_t const> data_{};
        bool valid_{false};
        bool has_vlan_tag_{false};

    public:
        frame_parser() noexcept = default;

        // construct from buffer
        explicit frame_parser(std::span<std::uint8_t const> data) noexcept;

        // parse and validate
        [[nodiscard]] auto parse(std::span<std::uint8_t const> data) noexcept -> bool;

        // accessors
        [[nodiscard]] auto is_valid() const noexcept -> bool
        {
            return valid_;
        }
        [[nodiscard]] auto has_vlan() const noexcept -> bool
        {
            return has_vlan_tag_;
        }

        [[nodiscard]] auto dest_mac() const noexcept -> mac_address;
        [[nodiscard]] auto src_mac() const noexcept -> mac_address;
        [[nodiscard]] auto ether_type() const noexcept -> std::uint16_t;

        // for vlan frames
        [[nodiscard]] auto vlan_id() const noexcept -> std::uint16_t;
        [[nodiscard]] auto vlan_priority() const noexcept -> std::uint8_t;

        // payload - the actual data you probably care about
        [[nodiscard]] auto payload() const noexcept -> std::span<std::uint8_t const>;
        [[nodiscard]] auto payload_size() const noexcept -> std::size_t;

        // raw access
        [[nodiscard]] auto raw_data() const noexcept -> std::span<std::uint8_t const>
        {
            return data_;
        }
        [[nodiscard]] auto header_size() const noexcept -> std::size_t
        {
            return has_vlan_tag_ ? constants::eth_vlan_header_size : constants::eth_header_size;
        }
    };

    // ============================================================================
    // utility functions
    // ============================================================================

    // quick frame construction for simple cases
    [[nodiscard]] auto build_simple_frame(mac_address const &dest, mac_address const &src, std::uint16_t ether_type,
                                          std::span<std::uint8_t const> payload) noexcept
        -> result<std::vector<std::uint8_t>>;

    [[nodiscard]] auto build_simple_frame(mac_address const &dest, mac_address const &src, std::uint16_t ether_type,
                                          std::string_view payload) noexcept -> result<std::vector<std::uint8_t>>;

} // namespace l2net
