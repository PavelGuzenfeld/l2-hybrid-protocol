// tests/unit/test_frame.cpp - frame construction/parsing tests
// making sure we can actually build valid ethernet frames

#include <doctest/doctest.h>
#include "l2net/frame.hpp"
#include "l2net/common.hpp"

TEST_SUITE("frame_builder") {
    TEST_CASE("build minimal frame") {
        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

        auto result = l2net::frame_builder{}
            .set_dest(dest)
            .set_src(src)
            .set_ether_type(0x0800)  // IPv4
            .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size);
    }

    TEST_CASE("build frame with payload") {
        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        std::string_view const payload = "test payload data";

        auto result = l2net::frame_builder{}
            .set_dest(dest)
            .set_src(src)
            .set_ether_type(0x88B5)
            .set_payload(payload)
            .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size + payload.size());
    }

    TEST_CASE("build_into with sufficient buffer") {
        l2net::mac_address const dest{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        l2net::mac_address const src{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

        std::array<std::uint8_t, 64> buffer{};

        auto builder = l2net::frame_builder{}
            .set_dest(dest)
            .set_src(src)
            .set_ether_type(0x0800);

        auto result = builder.build_into(buffer);

        REQUIRE(result.has_value());
        CHECK(*result == l2net::constants::eth_header_size);

        // verify dest mac
        CHECK(buffer[0] == 0xAA);
        CHECK(buffer[5] == 0xFF);

        // verify src mac
        CHECK(buffer[6] == 0x11);
        CHECK(buffer[11] == 0x66);

        // verify ether type (network byte order)
        CHECK(buffer[12] == 0x08);
        CHECK(buffer[13] == 0x00);
    }

    TEST_CASE("build_into with insufficient buffer") {
        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        l2net::mac_address const src{0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        std::array<std::uint8_t, 10> buffer{};  // too small

        auto result = l2net::frame_builder{}
            .set_dest(dest)
            .set_src(src)
            .set_ether_type(0x0800)
            .build_into(buffer);

        CHECK_FALSE(result.has_value());
        CHECK(result.error() == l2net::error_code::buffer_too_small);
    }

    TEST_CASE("required_size calculation") {
        auto builder = l2net::frame_builder{};
        CHECK(builder.required_size() == l2net::constants::eth_header_size);

        builder.set_payload("hello");
        CHECK(builder.required_size() == l2net::constants::eth_header_size + 5);
    }

    TEST_CASE("reset clears state") {
        auto builder = l2net::frame_builder{}
            .set_dest(l2net::mac_address::broadcast())
            .set_payload("test data");

        CHECK(builder.required_size() > l2net::constants::eth_header_size);

        builder.reset();
        CHECK(builder.required_size() == l2net::constants::eth_header_size);
    }
}

TEST_SUITE("frame_parser") {
    TEST_CASE("parse valid untagged frame") {
        // construct a valid frame
        std::vector<std::uint8_t> frame(20);
        // dest mac
        frame[0] = 0xFF; frame[1] = 0xFF; frame[2] = 0xFF;
        frame[3] = 0xFF; frame[4] = 0xFF; frame[5] = 0xFF;
        // src mac
        frame[6] = 0x00; frame[7] = 0x11; frame[8] = 0x22;
        frame[9] = 0x33; frame[10] = 0x44; frame[11] = 0x55;
        // ether type (0x0800 - IPv4)
        frame[12] = 0x08; frame[13] = 0x00;
        // payload
        frame[14] = 'H'; frame[15] = 'I';

        l2net::frame_parser parser{frame};

        CHECK(parser.is_valid());
        CHECK_FALSE(parser.has_vlan());
        CHECK(parser.dest_mac().is_broadcast());
        CHECK(parser.ether_type() == 0x0800);
        CHECK(parser.payload_size() == 6);  // remaining bytes
        CHECK(parser.header_size() == l2net::constants::eth_header_size);
    }

    TEST_CASE("parse tagged frame") {
        // construct a VLAN tagged frame
        std::vector<std::uint8_t> frame(22);
        // dest mac
        std::fill_n(frame.begin(), 6, 0xFF);
        // src mac
        std::fill_n(frame.begin() + 6, 6, 0x00);
        // TPID (0x8100)
        frame[12] = 0x81; frame[13] = 0x00;
        // TCI (priority 7, VLAN 10)
        std::uint16_t const tci = (7 << 13) | 10;
        frame[14] = static_cast<std::uint8_t>(tci >> 8);
        frame[15] = static_cast<std::uint8_t>(tci & 0xFF);
        // inner ether type
        frame[16] = 0x88; frame[17] = 0xB5;
        // payload
        frame[18] = 'T'; frame[19] = 'E'; frame[20] = 'S'; frame[21] = 'T';

        l2net::frame_parser parser{frame};

        CHECK(parser.is_valid());
        CHECK(parser.has_vlan());
        CHECK(parser.vlan_id() == 10);
        CHECK(parser.vlan_priority() == 7);
        CHECK(parser.ether_type() == 0x88B5);
        CHECK(parser.header_size() == l2net::constants::eth_vlan_header_size);
    }

    TEST_CASE("parse too small frame") {
        std::vector<std::uint8_t> const tiny(5);  // way too small
        l2net::frame_parser parser{tiny};

        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("parse empty frame") {
        l2net::frame_parser parser{std::span<std::uint8_t const>{}};
        CHECK_FALSE(parser.is_valid());
    }

    TEST_CASE("extract mac addresses correctly") {
        std::vector<std::uint8_t> frame(14);
        frame[0] = 0xAA; frame[1] = 0xBB; frame[2] = 0xCC;
        frame[3] = 0xDD; frame[4] = 0xEE; frame[5] = 0xFF;
        frame[6] = 0x11; frame[7] = 0x22; frame[8] = 0x33;
        frame[9] = 0x44; frame[10] = 0x55; frame[11] = 0x66;
        frame[12] = 0x08; frame[13] = 0x00;

        l2net::frame_parser parser{frame};
        REQUIRE(parser.is_valid());

        auto const dest = parser.dest_mac();
        auto const src = parser.src_mac();

        CHECK(dest.bytes()[0] == 0xAA);
        CHECK(dest.bytes()[5] == 0xFF);
        CHECK(src.bytes()[0] == 0x11);
        CHECK(src.bytes()[5] == 0x66);
    }
}

TEST_SUITE("build_simple_frame") {
    TEST_CASE("convenience function works") {
        auto result = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x88B5,
            "test payload"
        );

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_header_size + 12);

        // verify with parser
        l2net::frame_parser parser{*result};
        CHECK(parser.is_valid());
        CHECK(parser.dest_mac().is_broadcast());
        CHECK(parser.ether_type() == 0x88B5);
    }

    TEST_CASE("binary payload") {
        std::vector<std::uint8_t> const payload{0x00, 0x01, 0x02, 0x03};

        auto result = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x0800,
            payload
        );

        REQUIRE(result.has_value());

        l2net::frame_parser parser{*result};
        auto const parsed_payload = parser.payload();

        CHECK(parsed_payload.size() >= 4);
        CHECK(parsed_payload[0] == 0x00);
        CHECK(parsed_payload[3] == 0x03);
    }
}

TEST_SUITE("ethernet_header") {
    TEST_CASE("size is correct") {
        static_assert(sizeof(l2net::ethernet_header) == 14);
    }

    TEST_CASE("direct memory access") {
        std::array<std::uint8_t, 14> raw{};
        raw[0] = 0xFF; raw[5] = 0xFF;  // broadcast dest
        raw[12] = 0x08; raw[13] = 0x00;  // IPv4

        auto const* header = reinterpret_cast<l2net::ethernet_header const*>(raw.data());

        CHECK(header->dest().is_broadcast());
        CHECK(header->type() == 0x0800);
    }
}
