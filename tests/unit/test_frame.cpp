
// tests/unit/test_frame.cpp

#include "l2net/common.hpp"
#include "l2net/frame.hpp"
#include "test_helpers.hpp"

#include <doctest/doctest.h>

using namespace l2net;
using namespace l2net::testing;

TEST_SUITE("frame_builder")
{
    TEST_CASE("build minimal frame")
    {
        auto result = frame_builder{}.set_dest(TEST_DEST_MAC).set_src(TEST_SRC_MAC).set_ether_type(0x0800).build();

        REQUIRE(result.has_value());
        CHECK(result->size() == constants::eth_header_size);

        verify_frame_header(*result, TEST_DEST_MAC, TEST_SRC_MAC, 0x0800);
    }

    TEST_CASE("build frame with payload")
    {
        std::string_view const payload = "test payload data";
        auto result = frame_builder{}
                          .set_dest(TEST_DEST_MAC)
                          .set_src(TEST_SRC_MAC)
                          .set_ether_type(0x88B5)
                          .set_payload(payload)
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == constants::eth_header_size + payload.size());

        verify_frame_header(*result, TEST_DEST_MAC, TEST_SRC_MAC, 0x88B5);
    }

    TEST_CASE("build_into with sufficient buffer")
    {
        std::array<std::uint8_t, 64> buffer{};
        auto builder = frame_builder{}.set_dest(TEST_DEST_MAC).set_src(TEST_SRC_MAC).set_ether_type(0x0800);

        auto result = builder.build_into(buffer);
        REQUIRE(result.has_value());
        CHECK(*result == constants::eth_header_size);

        frame_parser parser{std::span<std::uint8_t const>{buffer.data(), *result}};
        CHECK(parser.dest_mac() == TEST_DEST_MAC);
        CHECK(parser.src_mac() == TEST_SRC_MAC);
        CHECK(parser.ether_type() == 0x0800);
    }

    TEST_CASE("build_into with insufficient buffer")
    {
        std::array<std::uint8_t, 10> buffer{};
        auto result = frame_builder{}
                          .set_dest(TEST_DEST_MAC)
                          .set_src(mac_address::null())
                          .set_ether_type(0x0800)
                          .build_into(buffer);

        CHECK_FALSE(result.has_value());
        CHECK(result.error() == error_code::buffer_too_small);
    }

    TEST_CASE("required_size calculation")
    {
        auto builder = frame_builder{};
        CHECK(builder.required_size() == constants::eth_header_size);
        (void)builder.set_payload("hello");
        CHECK(builder.required_size() == constants::eth_header_size + 5);
    }

    TEST_CASE("reset clears state")
    {
        auto builder = frame_builder{}.set_dest(mac_address::broadcast()).set_payload("test data");
        CHECK(builder.required_size() > constants::eth_header_size);
        builder.reset();
        CHECK(builder.required_size() == constants::eth_header_size);
    }
}

TEST_SUITE("frame_parser")
{
    TEST_CASE("parse valid untagged frame")
    {
        std::vector<std::uint8_t> frame(20);
        std::fill_n(frame.begin(), 6, 0xFF);
        std::copy(TEST_SRC_MAC.bytes().begin(), TEST_SRC_MAC.bytes().end(),
                  frame.begin() + 6); // wait, mac_address needs begin/end exposed or use data

        // Actually let's just use the manual construction to match your specific test case
        // but cleaner
        std::vector<std::uint8_t> manual_frame = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Dest
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, // Src
            0x08, 0x00,                         // Type
            'H',  'I'                           // Payload
        };

        // Oops, the vector constructor above created size 20 but init list created new one.
        // Let's fix the logic.
        frame[12] = 0x08;
        frame[13] = 0x00;
        frame[14] = 'H';
        frame[15] = 'I';

        // Actually, let's just use the build_simple_frame to create a valid frame to test parser.
        // That ensures we test round trip.
        auto built = build_simple_frame(mac_address::broadcast(), TEST_SRC_MAC, 0x0800, "HI");
        REQUIRE(built.has_value());

        frame_parser parser{*built};
        CHECK(parser.is_valid());
        CHECK_FALSE(parser.has_vlan());
        CHECK(parser.dest_mac().is_broadcast());
        CHECK(parser.src_mac() == TEST_SRC_MAC);
        CHECK(parser.ether_type() == 0x0800);
        CHECK(parser.payload_size() == 2);
    }

    TEST_CASE("parse tagged frame")
    {
        vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
        auto built = build_vlan_frame(mac_address::broadcast(), mac_address::null(), tci, 0x88B5, "TEST");
        REQUIRE(built.has_value());

        frame_parser parser{*built};
        CHECK(parser.is_valid());
        CHECK(parser.has_vlan());
        CHECK(parser.vlan_id() == 10);
        CHECK(parser.vlan_priority() == 7);
        CHECK(parser.ether_type() == 0x88B5);
        CHECK(parser.header_size() == constants::eth_vlan_header_size);
    }

    TEST_CASE("parse too small frame")
    {
        std::vector<std::uint8_t> const tiny(5);
        frame_parser parser{tiny};
        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("parse empty frame")
    {
        frame_parser parser{std::span<std::uint8_t const>{}};
        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("extract mac addresses correctly")
    {
        auto built = build_simple_frame(TEST_DEST_MAC, TEST_SRC_MAC, 0x0800, "");
        REQUIRE(built.has_value());

        frame_parser parser{*built};
        REQUIRE(parser.is_valid());

        CHECK(parser.dest_mac() == TEST_DEST_MAC);
        CHECK(parser.src_mac() == TEST_SRC_MAC);
    }
}

TEST_SUITE("build_simple_frame")
{
    TEST_CASE("convenience function works")
    {
        auto result = build_simple_frame(mac_address::broadcast(), mac_address::null(), 0x88B5, "test payload");
        REQUIRE(result.has_value());
        CHECK(result->size() == constants::eth_header_size + 12);
        verify_frame_header(*result, mac_address::broadcast(), mac_address::null(), 0x88B5);
    }

    TEST_CASE("binary payload")
    {
        std::vector<std::uint8_t> const payload{0x00, 0x01, 0x02, 0x03};
        auto result = build_simple_frame(mac_address::broadcast(), mac_address::null(), 0x0800, payload);
        REQUIRE(result.has_value());

        frame_parser parser{*result};
        auto const parsed_payload = parser.payload();
        CHECK(parsed_payload.size() >= 4);
        CHECK(parsed_payload[0] == 0x00);
        CHECK(parsed_payload[3] == 0x03);
    }
}

TEST_SUITE("ethernet_header")
{
    TEST_CASE("size is correct")
    {
        static_assert(sizeof(ethernet_header) == 14);
    }

    TEST_CASE("direct memory access")
    {
        std::array<std::uint8_t, 14> raw{};
        std::fill(raw.begin(), raw.begin() + 6, 0xFF);
        raw[12] = 0x08;
        raw[13] = 0x00;
        auto const *header = reinterpret_cast<ethernet_header const *>(raw.data());
        CHECK(header->dest().is_broadcast());
        CHECK(header->type() == 0x0800);
    }
}
