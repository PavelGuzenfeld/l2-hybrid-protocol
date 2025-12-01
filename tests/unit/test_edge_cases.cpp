// tests/unit/test_edge_cases.cpp - edge case tests
// the tests that expose just how fragile your code really is
// if you're reading this, congratulations on having standards

#include "l2net/common.hpp"
#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/ipc_channel.hpp"
#include "l2net/vlan.hpp"
#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <random>
#include <vector>

namespace
{

    // helper to generate random bytes - because manually typing 0x42 everywhere is boring
    [[nodiscard]] auto generate_random_bytes(std::size_t const count) -> std::vector<std::uint8_t>
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

} // anonymous namespace

// =============================================================================
// mac_address edge cases - because people will try everything
// =============================================================================
TEST_SUITE("mac_address_edge_cases")
{
    TEST_CASE("from_string with various invalid formats")
    {
        // too short
        CHECK_FALSE(l2net::mac_address::from_string("").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb:cc:dd:ee").has_value());

        // too long
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb:cc:dd:ee:ff:gg").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb:cc:dd:ee:ff:00").has_value());

        // wrong separators
        CHECK_FALSE(l2net::mac_address::from_string("aa.bb.cc.dd.ee.ff").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa_bb_cc_dd_ee_ff").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("aa bb cc dd ee ff").has_value());

        // mixed separators - should fail
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb-cc:dd-ee:ff").has_value());

        // invalid hex characters
        CHECK_FALSE(l2net::mac_address::from_string("gg:hh:ii:jj:kk:ll").has_value());
        CHECK_FALSE(l2net::mac_address::from_string("az:by:cx:dw:ev:fu").has_value());

        // special characters
        CHECK_FALSE(l2net::mac_address::from_string("a@:b#:c$:d%:e^:f&").has_value());

        // null bytes in string (if somehow passed)
        CHECK_FALSE(l2net::mac_address::from_string("aa:bb:cc:dd:ee:\0f").has_value());
    }

    TEST_CASE("from_string with valid edge cases")
    {
        // all zeros
        auto const zeros = l2net::mac_address::from_string("00:00:00:00:00:00");
        REQUIRE(zeros.has_value());
        CHECK(zeros->is_null());

        // all ones (broadcast)
        auto const broadcast = l2net::mac_address::from_string("ff:ff:ff:ff:ff:ff");
        REQUIRE(broadcast.has_value());
        CHECK(broadcast->is_broadcast());

        // uppercase
        auto const upper = l2net::mac_address::from_string("AA:BB:CC:DD:EE:FF");
        REQUIRE(upper.has_value());

        // mixed case
        auto const mixed = l2net::mac_address::from_string("aA:Bb:cC:Dd:eE:Ff");
        REQUIRE(mixed.has_value());

        // dash separator
        auto const dashed = l2net::mac_address::from_string("aa-bb-cc-dd-ee-ff");
        REQUIRE(dashed.has_value());
    }

    TEST_CASE("multicast bit detection edge cases")
    {
        // exactly at multicast boundary
        l2net::mac_address const multicast_min{0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        CHECK(multicast_min.is_multicast());

        // just below multicast (unicast)
        l2net::mac_address const unicast{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK_FALSE(unicast.is_multicast());

        // broadcast is also multicast
        CHECK(l2net::mac_address::broadcast().is_multicast());
    }

    TEST_CASE("comparison edge cases")
    {
        l2net::mac_address const a{0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        l2net::mac_address const b{0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        l2net::mac_address const c{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

        CHECK(a < b);
        CHECK(b < c);
        CHECK(a < c);
        CHECK_FALSE(a > b);
        CHECK(a <= a);
        CHECK(a >= a);
    }

    TEST_CASE("to_string format consistency")
    {
        l2net::mac_address const mac{0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
        auto const str = mac.to_string();

        // should be lowercase with colons
        CHECK(str == "0a:0b:0c:0d:0e:0f");
        CHECK(str.length() == 17);
    }
}

// =============================================================================
// frame builder edge cases - because garbage in, garbage out
// =============================================================================
TEST_SUITE("frame_builder_edge_cases")
{
    TEST_CASE("empty payload")
    {
        auto result = l2net::frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_ether_type(0x0800)
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size);
    }

    TEST_CASE("maximum standard payload")
    {
        // max ethernet payload is 1500 bytes
        std::vector<std::uint8_t> const max_payload(1500, 0x42);

        auto result = l2net::frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_ether_type(0x0800)
                          .set_payload(max_payload)
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size + 1500);
    }

    TEST_CASE("jumbo frame payload")
    {
        // jumbo frames can be up to 9000 bytes
        std::vector<std::uint8_t> const jumbo_payload(9000, 0x42);

        auto result = l2net::frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_ether_type(0x0800)
                          .set_payload(jumbo_payload)
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size + 9000);
    }

    TEST_CASE("ether type boundary values")
    {
        // minimum ether type (below 1536 is length field, not type)
        auto min_type = l2net::frame_builder{}
                            .set_dest(l2net::mac_address::broadcast())
                            .set_src(l2net::mac_address::null())
                            .set_ether_type(0x0000)
                            .build();
        REQUIRE(min_type.has_value());

        // maximum ether type
        auto max_type = l2net::frame_builder{}
                            .set_dest(l2net::mac_address::broadcast())
                            .set_src(l2net::mac_address::null())
                            .set_ether_type(0xFFFF)
                            .build();
        REQUIRE(max_type.has_value());
    }

    TEST_CASE("build_into with exact size buffer")
    {
        std::string_view const payload = "test";
        auto builder = l2net::frame_builder{}
                           .set_dest(l2net::mac_address::broadcast())
                           .set_src(l2net::mac_address::null())
                           .set_ether_type(0x0800)
                           .set_payload(payload);

        std::vector<std::uint8_t> exact_buffer(builder.required_size());
        auto result = builder.build_into(exact_buffer);
        REQUIRE(result.has_value());
        CHECK(*result == builder.required_size());
    }

    TEST_CASE("build_into with one byte short buffer")
    {
        std::string_view const payload = "test";
        auto builder = l2net::frame_builder{}
                           .set_dest(l2net::mac_address::broadcast())
                           .set_src(l2net::mac_address::null())
                           .set_ether_type(0x0800)
                           .set_payload(payload);

        std::vector<std::uint8_t> short_buffer(builder.required_size() - 1);
        auto result = builder.build_into(short_buffer);
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == l2net::error_code::buffer_too_small);
    }

    TEST_CASE("build_into with zero size buffer")
    {
        auto builder = l2net::frame_builder{}
                           .set_dest(l2net::mac_address::broadcast())
                           .set_src(l2net::mac_address::null())
                           .set_ether_type(0x0800);

        std::span<std::uint8_t> empty_buffer;
        auto result = builder.build_into(empty_buffer);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("multiple builds from same builder")
    {
        auto builder = l2net::frame_builder{}
                           .set_dest(l2net::mac_address::broadcast())
                           .set_src(l2net::mac_address::null())
                           .set_ether_type(0x0800)
                           .set_payload("test");

        auto result1 = builder.build();
        auto result2 = builder.build();

        REQUIRE(result1.has_value());
        REQUIRE(result2.has_value());
        CHECK(*result1 == *result2);
    }

    TEST_CASE("payload with embedded nulls")
    {
        std::vector<std::uint8_t> const payload_with_nulls{0x00, 0x01, 0x00, 0x02, 0x00};

        auto result = l2net::frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_ether_type(0x0800)
                          .set_payload(payload_with_nulls)
                          .build();

        REQUIRE(result.has_value());

        l2net::frame_parser parser{*result};
        auto const parsed_payload = parser.payload();
        CHECK(parsed_payload.size() == 5);
        CHECK(parsed_payload[0] == 0x00);
        CHECK(parsed_payload[2] == 0x00);
    }
}

// =============================================================================
// frame parser edge cases - because the network sends you garbage
// =============================================================================
TEST_SUITE("frame_parser_edge_cases")
{
    TEST_CASE("parse minimum valid frame")
    {
        std::vector<std::uint8_t> min_frame(l2net::constants::eth_header_size);
        std::fill_n(min_frame.begin(), 6, 0xFF);     // dest
        std::fill_n(min_frame.begin() + 6, 6, 0x00); // src
        min_frame[12] = 0x08;
        min_frame[13] = 0x00; // type

        l2net::frame_parser parser{min_frame};
        CHECK(parser.is_valid());
        CHECK(parser.payload_size() == 0);
    }

    TEST_CASE("parse frame exactly one byte short")
    {
        std::vector<std::uint8_t> const short_frame(l2net::constants::eth_header_size - 1);
        l2net::frame_parser parser{short_frame};
        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("parse vlan frame exactly at minimum size")
    {
        std::vector<std::uint8_t> min_vlan(l2net::constants::eth_vlan_header_size);
        std::fill_n(min_vlan.begin(), 6, 0xFF);
        std::fill_n(min_vlan.begin() + 6, 6, 0x00);
        min_vlan[12] = 0x81;
        min_vlan[13] = 0x00; // VLAN tag
        min_vlan[14] = 0x00;
        min_vlan[15] = 0x0A; // TCI (VLAN 10)
        min_vlan[16] = 0x08;
        min_vlan[17] = 0x00; // inner type

        l2net::frame_parser parser{min_vlan};
        CHECK(parser.is_valid());
        CHECK(parser.has_vlan());
        CHECK(parser.payload_size() == 0);
    }

    TEST_CASE("parse vlan frame one byte short of header")
    {
        std::vector<std::uint8_t> short_vlan(l2net::constants::eth_vlan_header_size - 1);
        std::fill_n(short_vlan.begin(), 6, 0xFF);
        std::fill_n(short_vlan.begin() + 6, 6, 0x00);
        short_vlan[12] = 0x81;
        short_vlan[13] = 0x00;

        l2net::frame_parser parser{short_vlan};
        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("reparse same parser object")
    {
        std::vector<std::uint8_t> frame1(20);
        std::fill_n(frame1.begin(), 6, 0xFF);
        frame1[12] = 0x08;
        frame1[13] = 0x00;

        std::vector<std::uint8_t> frame2(20);
        std::fill_n(frame2.begin(), 6, 0xAA);
        frame2[12] = 0x08;
        frame2[13] = 0x06;

        l2net::frame_parser parser{frame1};
        CHECK(parser.dest_mac().is_broadcast());
        CHECK(parser.ether_type() == 0x0800);

        // reparse with different frame
        (void)parser.parse(frame2);
        CHECK_FALSE(parser.dest_mac().is_broadcast());
        CHECK(parser.ether_type() == 0x0806);
    }

    TEST_CASE("parse with garbage data")
    {
        auto garbage = generate_random_bytes(100);

        // ensure it's not accidentally a valid VLAN tag
        if (garbage.size() >= 14)
        {
            garbage[12] = 0x08;
            garbage[13] = 0x00;
        }

        l2net::frame_parser parser{garbage};
        // should still parse as valid (just random data)
        if (garbage.size() >= l2net::constants::eth_header_size)
        {
            CHECK(parser.is_valid());
        }
    }

    TEST_CASE("accessors on invalid parser")
    {
        l2net::frame_parser const invalid_parser{};

        CHECK_FALSE(invalid_parser.is_valid());
        CHECK(invalid_parser.dest_mac().is_null());
        CHECK(invalid_parser.src_mac().is_null());
        CHECK(invalid_parser.ether_type() == 0);
        CHECK(invalid_parser.payload().empty());
        CHECK(invalid_parser.payload_size() == 0);
    }
}

// =============================================================================
// vlan_tci edge cases - because 12 bits should be enough for anybody
// =============================================================================
TEST_SUITE("vlan_tci_edge_cases")
{
    TEST_CASE("priority boundary values")
    {
        l2net::vlan_tci const min_prio{.priority = 0, .dei = false, .vlan_id = 1};
        CHECK(min_prio.is_valid());

        l2net::vlan_tci const max_prio{.priority = 7, .dei = false, .vlan_id = 1};
        CHECK(max_prio.is_valid());

        l2net::vlan_tci const invalid_prio{.priority = 8, .dei = false, .vlan_id = 1};
        CHECK_FALSE(invalid_prio.is_valid());

        l2net::vlan_tci const way_invalid_prio{.priority = 255, .dei = false, .vlan_id = 1};
        CHECK_FALSE(way_invalid_prio.is_valid());
    }

    TEST_CASE("vlan_id boundary values")
    {
        l2net::vlan_tci const min_vlan{.priority = 0, .dei = false, .vlan_id = 0};
        CHECK(min_vlan.is_valid());

        l2net::vlan_tci const max_vlan{.priority = 0, .dei = false, .vlan_id = 4095};
        CHECK(max_vlan.is_valid());

        l2net::vlan_tci const invalid_vlan{.priority = 0, .dei = false, .vlan_id = 4096};
        CHECK_FALSE(invalid_vlan.is_valid());

        l2net::vlan_tci const way_invalid_vlan{.priority = 0, .dei = false, .vlan_id = 65535};
        CHECK_FALSE(way_invalid_vlan.is_valid());
    }

    TEST_CASE("encode/decode with all bits set")
    {
        l2net::vlan_tci const max_tci{.priority = 7, .dei = true, .vlan_id = 4095};
        auto const encoded = max_tci.encode();
        CHECK(encoded == 0xFFFF);

        auto const decoded = l2net::vlan_tci::decode(encoded);
        CHECK(decoded.priority == 7);
        CHECK(decoded.dei == true);
        CHECK(decoded.vlan_id == 4095);
    }

    TEST_CASE("encode/decode with no bits set")
    {
        l2net::vlan_tci const zero_tci{.priority = 0, .dei = false, .vlan_id = 0};
        auto const encoded = zero_tci.encode();
        CHECK(encoded == 0x0000);

        auto const decoded = l2net::vlan_tci::decode(encoded);
        CHECK(decoded.priority == 0);
        CHECK(decoded.dei == false);
        CHECK(decoded.vlan_id == 0);
    }

    TEST_CASE("dei bit preservation")
    {
        l2net::vlan_tci const with_dei{.priority = 0, .dei = true, .vlan_id = 0};
        auto const encoded = with_dei.encode();
        CHECK((encoded & 0x1000) != 0);

        l2net::vlan_tci const without_dei{.priority = 0, .dei = false, .vlan_id = 0};
        auto const encoded2 = without_dei.encode();
        CHECK((encoded2 & 0x1000) == 0);
    }
}

// =============================================================================
// vlan frame builder edge cases
// =============================================================================
TEST_SUITE("vlan_frame_builder_edge_cases")
{
    TEST_CASE("maximum valid vlan frame")
    {
        std::vector<std::uint8_t> const max_payload(1496, 0x42); // 1500 - 4 for VLAN tag

        auto result = l2net::vlan_frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_vlan_id(4095)
                          .set_priority(7)
                          .set_dei(true)
                          .set_inner_ether_type(0xFFFF)
                          .set_payload(max_payload)
                          .build();

        REQUIRE(result.has_value());
    }

    TEST_CASE("empty vlan frame")
    {
        auto result = l2net::vlan_frame_builder{}
                          .set_dest(l2net::mac_address::broadcast())
                          .set_src(l2net::mac_address::null())
                          .set_vlan_id(1)
                          .set_priority(0)
                          .set_inner_ether_type(0x0800)
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_vlan_header_size);
    }

    TEST_CASE("build fails with invalid tci before building")
    {
        auto builder = l2net::vlan_frame_builder{}
                           .set_dest(l2net::mac_address::broadcast())
                           .set_src(l2net::mac_address::null())
                           .set_vlan_id(5000) // invalid
                           .set_priority(0)
                           .set_inner_ether_type(0x0800);

        auto result = builder.build();
        CHECK_FALSE(result.has_value());
    }
}

// =============================================================================
// byte_utils edge cases
// =============================================================================
TEST_SUITE("byte_utils_edge_cases")
{
    TEST_CASE("htons with boundary values")
    {
        // zero
        CHECK(l2net::byte_utils::htons_constexpr(0x0000) == 0x0000);

        // max
        auto const max_converted = l2net::byte_utils::htons_constexpr(0xFFFF);
        CHECK(l2net::byte_utils::ntohs_constexpr(max_converted) == 0xFFFF);

        // alternating bits
        auto const alt1 = l2net::byte_utils::htons_constexpr(0xAAAA);
        CHECK(l2net::byte_utils::ntohs_constexpr(alt1) == 0xAAAA);

        auto const alt2 = l2net::byte_utils::htons_constexpr(0x5555);
        CHECK(l2net::byte_utils::ntohs_constexpr(alt2) == 0x5555);
    }

    TEST_CASE("double conversion is identity")
    {
        for (std::uint16_t i = 0; i < 1000; ++i)
        {
            auto const converted = l2net::byte_utils::htons_constexpr(i);
            auto const back = l2net::byte_utils::ntohs_constexpr(converted);
            CHECK(back == i);
        }

        // spot check high values
        CHECK(l2net::byte_utils::ntohs_constexpr(
                  l2net::byte_utils::htons_constexpr(0xDEAD)) == 0xDEAD);
        CHECK(l2net::byte_utils::ntohs_constexpr(
                  l2net::byte_utils::htons_constexpr(0xBEEF)) == 0xBEEF);
    }
}

// =============================================================================
// interface query edge cases
// =============================================================================
TEST_SUITE("interface_edge_cases")
{
    TEST_CASE("query with maximum length name")
    {
        // IFNAMSIZ is typically 16, so 15 chars + null
        std::string const long_name(15, 'x');
        auto result = l2net::interface_info::query(long_name);
        // should fail but not crash
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("query with name exceeding maximum")
    {
        std::string const too_long_name(100, 'x');
        auto result = l2net::interface_info::query(too_long_name);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("query with special characters")
    {
        CHECK_FALSE(l2net::interface_info::query("eth0\n").has_value());
        CHECK_FALSE(l2net::interface_info::query("eth0\t").has_value());
        CHECK_FALSE(l2net::interface_info::query("eth 0").has_value());
        CHECK_FALSE(l2net::interface_info::query("eth/0").has_value());
    }
}

// =============================================================================
// error_code edge cases
// =============================================================================
TEST_SUITE("error_code_edge_cases")
{
    TEST_CASE("all error codes have string representation")
    {
        // ensure no "unknown_error" for valid codes
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::success) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::socket_creation_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::socket_bind_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::socket_send_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::socket_recv_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::interface_not_found) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::interface_query_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::invalid_mac_address) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::invalid_frame_size) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::invalid_vlan_id) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::invalid_priority) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::connection_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::handshake_failed) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::permission_denied) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::buffer_too_small) != "unknown_error");
        CHECK(l2net::error_code_formatter::to_string(l2net::error_code::timeout) != "unknown_error");
    }

    TEST_CASE("invalid error code gives unknown")
    {
        auto const invalid = static_cast<l2net::error_code>(255);
        CHECK(l2net::error_code_formatter::to_string(invalid) == "unknown_error");
    }
}

// =============================================================================
// build_simple_frame edge cases
// =============================================================================
TEST_SUITE("build_simple_frame_edge_cases")
{
    TEST_CASE("string payload with various sizes")
    {
        // empty string
        auto empty = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x0800,
            std::string_view{});
        REQUIRE(empty.has_value());

        // single character
        auto single = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x0800,
            "X");
        REQUIRE(single.has_value());
        CHECK(single->size() == l2net::constants::eth_header_size + 1);
    }

    TEST_CASE("binary payload roundtrip")
    {
        // all byte values 0x00-0xFF
        std::vector<std::uint8_t> all_bytes(256);
        for (std::size_t i = 0; i < 256; ++i)
        {
            all_bytes[i] = static_cast<std::uint8_t>(i);
        }

        auto frame = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x0800,
            all_bytes);
        REQUIRE(frame.has_value());

        l2net::frame_parser parser{*frame};
        auto const payload = parser.payload();
        REQUIRE(payload.size() == 256);

        for (std::size_t i = 0; i < 256; ++i)
        {
            CHECK(payload[i] == static_cast<std::uint8_t>(i));
        }
    }
}

// =============================================================================
// strip_vlan_tag edge cases
// =============================================================================
TEST_SUITE("strip_vlan_tag_edge_cases")
{
    TEST_CASE("strip from minimum vlan frame")
    {
        std::vector<std::uint8_t> min_vlan(l2net::constants::eth_vlan_header_size);
        std::fill_n(min_vlan.begin(), 6, 0xFF);
        std::fill_n(min_vlan.begin() + 6, 6, 0x00);
        min_vlan[12] = 0x81;
        min_vlan[13] = 0x00;
        min_vlan[14] = 0x00;
        min_vlan[15] = 0x0A;
        min_vlan[16] = 0x08;
        min_vlan[17] = 0x00;

        auto result = l2net::strip_vlan_tag(min_vlan);
        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size);
        CHECK_FALSE(l2net::is_vlan_tagged(*result));
    }

    TEST_CASE("strip from frame that looks like vlan but is too short")
    {
        std::vector<std::uint8_t> short_vlan(16);
        short_vlan[12] = 0x81;
        short_vlan[13] = 0x00;

        auto result = l2net::strip_vlan_tag(short_vlan);
        CHECK_FALSE(result.has_value());
    }
}