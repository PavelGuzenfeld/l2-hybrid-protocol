// frame.cpp - ethernet frame construction and parsing

#include "l2net/frame.hpp"
#include "l2net/vlan.hpp"

#include <algorithm>
#include <arpa/inet.h>

namespace l2net
{

    auto frame_builder::build() noexcept -> result<std::vector<std::uint8_t>>
    {
        auto const total_size = required_size();
        if (total_size < constants::eth_header_size)
        {
            return tl::unexpected{error_code::invalid_frame_size};
        }
        std::vector<std::uint8_t> frame(total_size);
        auto const written = build_into(frame);
        if (!written.has_value())
        {
            return tl::unexpected{written.error()};
        }
        return frame;
    }

    auto frame_builder::build_into(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>
    {
        auto const total_size = required_size();
        if (buffer.size() < total_size)
        {
            return tl::unexpected{error_code::buffer_too_small};
        }

        std::copy_n(dest_mac_.data(), 6, buffer.data());
        std::copy_n(src_mac_.data(), 6, buffer.data() + 6);

        buffer[12] = static_cast<std::uint8_t>(ether_type_ >> 8);
        buffer[13] = static_cast<std::uint8_t>(ether_type_ & 0xFF);

        if (!buffer_.empty())
        {
            std::copy(buffer_.begin(), buffer_.end(), buffer.begin() + constants::eth_header_size);
        }
        finalized_ = true;
        return total_size;
    }

    auto frame_builder::reset() noexcept -> void
    {
        buffer_.clear();
        dest_mac_ = mac_address{};
        src_mac_ = mac_address{};
        ether_type_ = 0;
        finalized_ = false;
    }

    frame_parser::frame_parser(std::span<std::uint8_t const> data) noexcept
    {
        (void)parse(data);
    }

    auto frame_parser::parse(std::span<std::uint8_t const> data) noexcept -> bool
    {
        data_ = data;
        valid_ = false;
        has_vlan_tag_ = false;

        if (data.size() < constants::eth_header_size)
        {
            return false;
        }

        // Read Big Endian bytes back into host integer
        auto const type_field = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[12]) << 8) | data[13]);

        if (type_field == constants::eth_p_8021q)
        {
            has_vlan_tag_ = true;
            if (data.size() < constants::eth_vlan_header_size)
            {
                return false;
            }
        }

        valid_ = true;
        return true;
    }

    auto frame_parser::dest_mac() const noexcept -> mac_address
    {
        if (!valid_ || data_.size() < 6)
            return mac_address{};
        mac_address::storage_type bytes{};
        std::copy_n(data_.data(), 6, bytes.begin());
        return mac_address{bytes};
    }

    auto frame_parser::src_mac() const noexcept -> mac_address
    {
        if (!valid_ || data_.size() < 12)
            return mac_address{};
        mac_address::storage_type bytes{};
        std::copy_n(data_.data() + 6, 6, bytes.begin());
        return mac_address{bytes};
    }

    auto frame_parser::ether_type() const noexcept -> std::uint16_t
    {
        if (!valid_)
            return 0;
        if (has_vlan_tag_)
        {
            if (data_.size() < 18)
                return 0;
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[16]) << 8) | data_[17]);
        }
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[12]) << 8) | data_[13]);
    }

    auto frame_parser::vlan_id() const noexcept -> std::uint16_t
    {
        if (!valid_ || !has_vlan_tag_ || data_.size() < 16)
            return 0;
        auto const tci = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[14]) << 8) | data_[15]);
        return tci & 0x0FFF;
    }

    auto frame_parser::vlan_priority() const noexcept -> std::uint8_t
    {
        if (!valid_ || !has_vlan_tag_ || data_.size() < 16)
            return 0;
        auto const tci = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[14]) << 8) | data_[15]);
        return static_cast<std::uint8_t>((tci >> 13) & 0x07);
    }

    auto frame_parser::payload() const noexcept -> std::span<std::uint8_t const>
    {
        if (!valid_)
            return {};
        auto const hdr_size = header_size();
        if (data_.size() <= hdr_size)
            return {};
        return data_.subspan(hdr_size);
    }

    auto frame_parser::payload_size() const noexcept -> std::size_t
    {
        if (!valid_)
            return 0;
        auto const hdr_size = header_size();
        if (data_.size() <= hdr_size)
            return 0;
        return data_.size() - hdr_size;
    }

    auto build_simple_frame(mac_address const &dest, mac_address const &src, std::uint16_t const ether_type, std::span<std::uint8_t const> payload) noexcept -> result<std::vector<std::uint8_t>>
    {
        return frame_builder{}.set_dest(dest).set_src(src).set_ether_type(ether_type).set_payload(payload).build();
    }

    auto build_simple_frame(mac_address const &dest, mac_address const &src, std::uint16_t const ether_type, std::string_view const payload) noexcept -> result<std::vector<std::uint8_t>>
    {
        return frame_builder{}.set_dest(dest).set_src(src).set_ether_type(ether_type).set_payload(payload).build();
    }

} // namespace l2net