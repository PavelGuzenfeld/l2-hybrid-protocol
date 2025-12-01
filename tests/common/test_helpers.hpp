#pragma once

#include "l2net/frame.hpp"
#include "l2net/vlan.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <random>
#include <span>
#include <vector>

namespace l2net::testing
{

    // generate random bytes - widely used in edge cases
    [[nodiscard]] inline auto generate_random_bytes(std::size_t const count) -> std::vector<std::uint8_t>
    {
        std::vector<std::uint8_t> result(count);
        std::random_device rd;
        std::mt19937 gen{rd()};
        std::uniform_int_distribution<> dist{0, 255};
        for (auto &byte : result)
        {
            byte = static_cast<std::uint8_t>(dist(gen));
        }
        return result;
    }

    // common mac addresses for testing
    inline constexpr mac_address TEST_DEST_MAC{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    inline constexpr mac_address TEST_SRC_MAC{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    // helper to verify basic frame properties
    inline void verify_frame_header(std::span<std::uint8_t const> frame, mac_address const &expected_dest,
                                    mac_address const &expected_src, std::uint16_t expected_type)
    {
        REQUIRE(frame.size() >= constants::eth_header_size);
        frame_parser parser{frame};
        CHECK(parser.is_valid());
        CHECK(parser.dest_mac() == expected_dest);
        CHECK(parser.src_mac() == expected_src);
        CHECK(parser.ether_type() == expected_type);
    }

    // helper to verify vlan tag
    inline void verify_vlan_tag(std::span<std::uint8_t const> frame, vlan_tci const &expected_tci)
    {
        REQUIRE(frame.size() >= constants::eth_vlan_header_size);
        frame_parser parser{frame};
        REQUIRE(parser.has_vlan());
        CHECK(parser.vlan_id() == expected_tci.vlan_id);
        CHECK(parser.vlan_priority() == expected_tci.priority);
    }

} // namespace l2net::testing
