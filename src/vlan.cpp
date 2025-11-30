// vlan.cpp - 802.1Q VLAN frame handling
// because bit manipulation shouldn't require a degree in archaeology

#include "l2net/vlan.hpp"

#include <arpa/inet.h>
#include <algorithm>

namespace l2net {

// ============================================================================
// vlan_frame_builder implementation
// ============================================================================

auto vlan_frame_builder::validate() const noexcept -> void_result {
    if (!tci_.is_valid()) {
        if (tci_.vlan_id > constants::max_vlan_id) {
            return tl::unexpected{error_code::invalid_vlan_id};
        }
        if (tci_.priority > constants::max_priority) {
            return tl::unexpected{error_code::invalid_priority};
        }
    }
    return {};
}

auto vlan_frame_builder::build() noexcept -> result<std::vector<std::uint8_t>> {
    auto const validation = validate();
    if (!validation.has_value()) {
        return tl::unexpected{validation.error()};
    }

    std::vector<std::uint8_t> frame(required_size());
    auto const written = build_into(frame);

    if (!written.has_value()) {
        return tl::unexpected{written.error()};
    }

    return frame;
}

auto vlan_frame_builder::build_into(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t> {
    auto const validation = validate();
    if (!validation.has_value()) {
        return tl::unexpected{validation.error()};
    }

    auto const total_size = required_size();
    if (buffer.size() < total_size) {
        return tl::unexpected{error_code::buffer_too_small};
    }

    // destination mac (bytes 0-5)
    std::copy_n(dest_mac_.data(), 6, buffer.data());

    // source mac (bytes 6-11)
    std::copy_n(src_mac_.data(), 6, buffer.data() + 6);

    // TPID (bytes 12-13) - 0x8100 for 802.1Q
    auto const tpid = htons(constants::eth_p_8021q);
    buffer[12] = static_cast<std::uint8_t>(tpid >> 8);
    buffer[13] = static_cast<std::uint8_t>(tpid & 0xFF);

    // TCI (bytes 14-15) - Priority + DEI + VLAN ID
    auto const tci = htons(tci_.encode());
    buffer[14] = static_cast<std::uint8_t>(tci >> 8);
    buffer[15] = static_cast<std::uint8_t>(tci & 0xFF);

    // Inner EtherType (bytes 16-17)
    auto const inner_type = htons(inner_ether_type_);
    buffer[16] = static_cast<std::uint8_t>(inner_type >> 8);
    buffer[17] = static_cast<std::uint8_t>(inner_type & 0xFF);

    // Payload (bytes 18+)
    if (!payload_.empty()) {
        std::copy(payload_.begin(), payload_.end(),
            buffer.begin() + constants::eth_vlan_header_size);
    }

    return total_size;
}

auto vlan_frame_builder::reset() noexcept -> void {
    dest_mac_ = mac_address{};
    src_mac_ = mac_address{};
    tci_ = vlan_tci{};
    inner_ether_type_ = 0;
    payload_.clear();
}

// ============================================================================
// convenience functions
// ============================================================================

auto build_vlan_frame(
    mac_address const& dest,
    mac_address const& src,
    vlan_tci const& tci,
    std::uint16_t const inner_ether_type,
    std::span<std::uint8_t const> payload
) noexcept -> result<std::vector<std::uint8_t>> {
    return vlan_frame_builder{}
        .set_dest(dest)
        .set_src(src)
        .set_tci(tci)
        .set_inner_ether_type(inner_ether_type)
        .set_payload(payload)
        .build();
}

auto build_vlan_frame(
    mac_address const& dest,
    mac_address const& src,
    vlan_tci const& tci,
    std::uint16_t const inner_ether_type,
    std::string_view const payload
) noexcept -> result<std::vector<std::uint8_t>> {
    return vlan_frame_builder{}
        .set_dest(dest)
        .set_src(src)
        .set_tci(tci)
        .set_inner_ether_type(inner_ether_type)
        .set_payload(payload)
        .build();
}

auto is_vlan_tagged(std::span<std::uint8_t const> frame) noexcept -> bool {
    if (frame.size() < constants::eth_header_size) {
        return false;
    }

    auto const type_field = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[12]) << 8) | frame[13]);

    return type_field == constants::eth_p_8021q;
}

auto strip_vlan_tag(std::span<std::uint8_t const> frame) noexcept
    -> result<std::vector<std::uint8_t>>
{
    if (!is_vlan_tagged(frame)) {
        // not tagged, return copy
        return std::vector<std::uint8_t>{frame.begin(), frame.end()};
    }

    if (frame.size() < constants::eth_vlan_header_size) {
        return tl::unexpected{error_code::invalid_frame_size};
    }

    // new frame: dest(6) + src(6) + inner_type(2) + payload
    std::vector<std::uint8_t> result;
    result.reserve(frame.size() - constants::vlan_header_size);

    // copy dest + src (12 bytes)
    result.insert(result.end(), frame.begin(), frame.begin() + 12);

    // copy inner ether type (bytes 16-17 in tagged frame)
    result.push_back(frame[16]);
    result.push_back(frame[17]);

    // copy payload (bytes 18+ in tagged frame)
    if (frame.size() > constants::eth_vlan_header_size) {
        result.insert(result.end(),
            frame.begin() + constants::eth_vlan_header_size,
            frame.end());
    }

    return result;
}

} // namespace l2net
