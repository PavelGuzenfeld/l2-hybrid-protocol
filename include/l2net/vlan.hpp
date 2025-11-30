#pragma once

// vlan.hpp - 802.1Q VLAN tagging support
// because manually bit-shifting TCI fields is medieval torture

#include "common.hpp"
#include "frame.hpp"

#include <span>
#include <vector>

namespace l2net {

// ============================================================================
// vlan tag control information
// ============================================================================

struct vlan_tci {
    std::uint8_t priority{0};  // 0-7, higher is more important
    bool dei{false};           // drop eligible indicator (formerly CFI)
    std::uint16_t vlan_id{0};  // 0-4095

    // validation
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool {
        return priority <= constants::max_priority && vlan_id <= constants::max_vlan_id;
    }

    // encode to 16-bit TCI value
    [[nodiscard]] constexpr auto encode() const noexcept -> std::uint16_t {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(priority) << 13) |
            (static_cast<std::uint16_t>(dei ? 1 : 0) << 12) |
            (vlan_id & 0x0FFF)
        );
    }

    // decode from 16-bit TCI value
    [[nodiscard]] static constexpr auto decode(std::uint16_t const tci) noexcept -> vlan_tci {
        return vlan_tci{
            .priority = static_cast<std::uint8_t>((tci >> 13) & 0x07),
            .dei = ((tci >> 12) & 0x01) != 0,
            .vlan_id = static_cast<std::uint16_t>(tci & 0x0FFF)
        };
    }

    // comparison
    [[nodiscard]] constexpr auto operator<=>(vlan_tci const&) const noexcept = default;
};

// ============================================================================
// vlan header structure - for direct memory mapping
// ============================================================================

struct vlan_header {
    std::uint16_t tpid;  // tag protocol identifier (0x8100)
    std::uint16_t tci;   // tag control information

    [[nodiscard]] constexpr auto tag_info() const noexcept -> vlan_tci {
        return vlan_tci::decode(byte_utils::ntohs_constexpr(tci));
    }

    [[nodiscard]] constexpr auto is_8021q() const noexcept -> bool {
        return byte_utils::ntohs_constexpr(tpid) == constants::eth_p_8021q;
    }
} __attribute__((packed));

static_assert(sizeof(vlan_header) == 4, "vlan header must be 4 bytes, did you break something?");

// ============================================================================
// vlan frame builder - for constructing tagged frames
// ============================================================================

class vlan_frame_builder {
private:
    mac_address dest_mac_{};
    mac_address src_mac_{};
    vlan_tci tci_{};
    std::uint16_t inner_ether_type_{0};
    std::vector<std::uint8_t> payload_{};

public:
    vlan_frame_builder() = default;

    // fluent interface
    [[nodiscard]] auto set_dest(mac_address const& mac) noexcept -> vlan_frame_builder& {
        dest_mac_ = mac;
        return *this;
    }

    [[nodiscard]] auto set_src(mac_address const& mac) noexcept -> vlan_frame_builder& {
        src_mac_ = mac;
        return *this;
    }

    [[nodiscard]] auto set_vlan_id(std::uint16_t id) noexcept -> vlan_frame_builder& {
        tci_.vlan_id = id;
        return *this;
    }

    [[nodiscard]] auto set_priority(std::uint8_t prio) noexcept -> vlan_frame_builder& {
        tci_.priority = prio;
        return *this;
    }

    [[nodiscard]] auto set_dei(bool dei) noexcept -> vlan_frame_builder& {
        tci_.dei = dei;
        return *this;
    }

    [[nodiscard]] auto set_tci(vlan_tci const& tci) noexcept -> vlan_frame_builder& {
        tci_ = tci;
        return *this;
    }

    [[nodiscard]] auto set_inner_ether_type(std::uint16_t type) noexcept -> vlan_frame_builder& {
        inner_ether_type_ = type;
        return *this;
    }

    [[nodiscard]] auto set_payload(std::span<std::uint8_t const> data) noexcept -> vlan_frame_builder& {
        payload_.assign(data.begin(), data.end());
        return *this;
    }

    [[nodiscard]] auto set_payload(std::string_view data) noexcept -> vlan_frame_builder& {
        payload_.assign(data.begin(), data.end());
        return *this;
    }

    // build
    [[nodiscard]] auto build() noexcept -> result<std::vector<std::uint8_t>>;

    [[nodiscard]] auto build_into(std::span<std::uint8_t> buffer) noexcept
        -> result<std::size_t>;

    // validation
    [[nodiscard]] auto validate() const noexcept -> void_result;

    // size calculation
    [[nodiscard]] constexpr auto required_size() const noexcept -> std::size_t {
        return constants::eth_vlan_header_size + payload_.size();
    }

    auto reset() noexcept -> void;
};

// ============================================================================
// convenience functions
// ============================================================================

[[nodiscard]] auto build_vlan_frame(
    mac_address const& dest,
    mac_address const& src,
    vlan_tci const& tci,
    std::uint16_t inner_ether_type,
    std::span<std::uint8_t const> payload
) noexcept -> result<std::vector<std::uint8_t>>;

[[nodiscard]] auto build_vlan_frame(
    mac_address const& dest,
    mac_address const& src,
    vlan_tci const& tci,
    std::uint16_t inner_ether_type,
    std::string_view payload
) noexcept -> result<std::vector<std::uint8_t>>;

// check if a frame is vlan tagged
[[nodiscard]] auto is_vlan_tagged(std::span<std::uint8_t const> frame) noexcept -> bool;

// strip vlan tag from frame (returns new buffer)
[[nodiscard]] auto strip_vlan_tag(std::span<std::uint8_t const> frame) noexcept
    -> result<std::vector<std::uint8_t>>;

} // namespace l2net
