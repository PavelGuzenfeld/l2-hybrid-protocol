// tests/unit/test_vlan.cpp - VLAN frame tests
// making sure we can build proper 802.1Q tagged frames

#include <doctest/doctest.h>
#include "l2net/vlan.hpp"
#include "l2net/frame.hpp"

TEST_SUITE("vlan_tci") {
    TEST_CASE("default construction") {
        l2net::vlan_tci const tci{};
        CHECK(tci.priority == 0);
        CHECK(tci.dei == false);
        CHECK(tci.vlan_id == 0);
        CHECK(tci.is_valid());
    }

    TEST_CASE("encode/decode roundtrip") {
        l2net::vlan_tci const original{
            .priority = 7,
            .dei = true,
            .vlan_id = 100
        };

        auto const encoded = original.encode();
        auto const decoded = l2net::vlan_tci::decode(encoded);

        CHECK(decoded.priority == original.priority);
        CHECK(decoded.dei == original.dei);
        CHECK(decoded.vlan_id == original.vlan_id);
    }

    TEST_CASE("encode specific values") {
        // priority 7 = bits 15-13 = 111
        // dei = bit 12 = 0
        // vlan_id 10 = bits 11-0 = 0000 0000 1010
        // expected: 1110 0000 0000 1010 = 0xE00A
        l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
        CHECK(tci.encode() == 0xE00A);
    }

    TEST_CASE("validation - invalid priority") {
        l2net::vlan_tci const tci{.priority = 8, .dei = false, .vlan_id = 10};
        CHECK_FALSE(tci.is_valid());
    }

    TEST_CASE("validation - invalid vlan_id") {
        l2net::vlan_tci const tci{.priority = 0, .dei = false, .vlan_id = 4096};
        CHECK_FALSE(tci.is_valid());
    }

    TEST_CASE("comparison") {
        l2net::vlan_tci const a{.priority = 5, .dei = false, .vlan_id = 100};
        l2net::vlan_tci const b{.priority = 5, .dei = false, .vlan_id = 100};
        l2net::vlan_tci const c{.priority = 3, .dei = false, .vlan_id = 100};

        CHECK(a == b);
        CHECK_FALSE(a == c);
    }
}

TEST_SUITE("vlan_header") {
    TEST_CASE("size is correct") {
        static_assert(sizeof(l2net::vlan_header) == 4);
    }

    TEST_CASE("is_8021q detection") {
        l2net::vlan_header header{};
        header.tpid = l2net::byte_utils::htons_constexpr(0x8100);
        CHECK(header.is_8021q());

        header.tpid = l2net::byte_utils::htons_constexpr(0x0800);
        CHECK_FALSE(header.is_8021q());
    }
}

TEST_SUITE("vlan_frame_builder") {
    TEST_CASE("build basic vlan frame") {
        auto result = l2net::vlan_frame_builder{}
            .set_dest(l2net::mac_address::broadcast())
            .set_src(l2net::mac_address{0x00, 0x11, 0x22, 0x33, 0x44, 0x55})
            .set_vlan_id(10)
            .set_priority(7)
            .set_inner_ether_type(0x88B5)
            .set_payload("test")
            .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_vlan_header_size + 4);
    }

    TEST_CASE("verify frame structure") {
        auto result = l2net::vlan_frame_builder{}
            .set_dest(l2net::mac_address{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF})
            .set_src(l2net::mac_address{0x11, 0x22, 0x33, 0x44, 0x55, 0x66})
            .set_vlan_id(10)
            .set_priority(7)
            .set_inner_ether_type(0x88B5)
            .build();

        REQUIRE(result.has_value());
        auto const& frame = *result;

        // check dest mac
        CHECK(frame[0] == 0xAA);
        CHECK(frame[5] == 0xFF);

        // check src mac
        CHECK(frame[6] == 0x11);
        CHECK(frame[11] == 0x66);

        // check TPID (0x8100)
        CHECK(frame[12] == 0x81);
        CHECK(frame[13] == 0x00);

        // check inner ether type
        CHECK(frame[16] == 0x88);
        CHECK(frame[17] == 0xB5);
    }

    TEST_CASE("validation fails for invalid vlan_id") {
        auto builder = l2net::vlan_frame_builder{}
            .set_vlan_id(5000);  // invalid

        auto validation = builder.validate();
        CHECK_FALSE(validation.has_value());
        CHECK(validation.error() == l2net::error_code::invalid_vlan_id);
    }

    TEST_CASE("validation fails for invalid priority") {
        auto builder = l2net::vlan_frame_builder{}
            .set_priority(10);  // invalid

        auto validation = builder.validate();
        CHECK_FALSE(validation.has_value());
        CHECK(validation.error() == l2net::error_code::invalid_priority);
    }

    TEST_CASE("build with tci struct") {
        l2net::vlan_tci const tci{.priority = 5, .dei = true, .vlan_id = 200};

        auto result = l2net::vlan_frame_builder{}
            .set_dest(l2net::mac_address::broadcast())
            .set_src(l2net::mac_address::null())
            .set_tci(tci)
            .set_inner_ether_type(0x0800)
            .build();

        REQUIRE(result.has_value());

        // verify with parser
        l2net::frame_parser parser{*result};
        CHECK(parser.has_vlan());
        CHECK(parser.vlan_priority() == 5);
        CHECK(parser.vlan_id() == 200);
    }

    TEST_CASE("reset clears state") {
        auto builder = l2net::vlan_frame_builder{}
            .set_vlan_id(100)
            .set_priority(7)
            .set_payload("lots of data here");

        auto const size_before = builder.required_size();
        builder.reset();

        CHECK(builder.required_size() < size_before);
    }
}

TEST_SUITE("vlan convenience functions") {
    TEST_CASE("build_vlan_frame string payload") {
        l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};

        auto result = l2net::build_vlan_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            tci,
            0x88B5,
            "test message"
        );

        REQUIRE(result.has_value());
        CHECK(result->size() == l2net::constants::eth_vlan_header_size + 12);
    }

    TEST_CASE("build_vlan_frame binary payload") {
        l2net::vlan_tci const tci{.priority = 3, .dei = false, .vlan_id = 50};
        std::vector<std::uint8_t> const payload{0x01, 0x02, 0x03};

        auto result = l2net::build_vlan_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            tci,
            0x0800,
            payload
        );

        REQUIRE(result.has_value());
    }

    TEST_CASE("is_vlan_tagged detection") {
        // untagged frame
        std::vector<std::uint8_t> untagged(14);
        untagged[12] = 0x08; untagged[13] = 0x00;  // IPv4
        CHECK_FALSE(l2net::is_vlan_tagged(untagged));

        // tagged frame
        std::vector<std::uint8_t> tagged(18);
        tagged[12] = 0x81; tagged[13] = 0x00;  // 802.1Q
        CHECK(l2net::is_vlan_tagged(tagged));
    }

    TEST_CASE("is_vlan_tagged too small") {
        std::vector<std::uint8_t> const tiny(10);
        CHECK_FALSE(l2net::is_vlan_tagged(tiny));
    }

    TEST_CASE("strip_vlan_tag from tagged frame") {
        // build a tagged frame
        l2net::vlan_tci const tci{.priority = 5, .dei = false, .vlan_id = 100};
        auto tagged = l2net::build_vlan_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            tci,
            0x0800,
            "payload"
        );
        REQUIRE(tagged.has_value());

        // strip the tag
        auto stripped = l2net::strip_vlan_tag(*tagged);
        REQUIRE(stripped.has_value());

        // should be 4 bytes smaller
        CHECK(stripped->size() == tagged->size() - 4);

        // should no longer be tagged
        CHECK_FALSE(l2net::is_vlan_tagged(*stripped));

        // ether type should be preserved
        l2net::frame_parser parser{*stripped};
        CHECK(parser.ether_type() == 0x0800);
    }

    TEST_CASE("strip_vlan_tag from untagged frame") {
        auto untagged = l2net::build_simple_frame(
            l2net::mac_address::broadcast(),
            l2net::mac_address::null(),
            0x0800,
            "test"
        );
        REQUIRE(untagged.has_value());

        // should return copy unchanged
        auto result = l2net::strip_vlan_tag(*untagged);
        REQUIRE(result.has_value());
        CHECK(result->size() == untagged->size());
    }
}

TEST_SUITE("vlan roundtrip") {
    TEST_CASE("build and parse vlan frame") {
        l2net::mac_address const dest{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        l2net::mac_address const src{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        l2net::vlan_tci const tci{.priority = 6, .dei = false, .vlan_id = 42};
        std::string_view const payload = "hello vlan world";

        auto frame = l2net::build_vlan_frame(dest, src, tci, 0x88B5, payload);
        REQUIRE(frame.has_value());

        l2net::frame_parser parser{*frame};

        CHECK(parser.is_valid());
        CHECK(parser.has_vlan());
        CHECK(parser.dest_mac() == dest);
        CHECK(parser.src_mac() == src);
        CHECK(parser.vlan_priority() == 6);
        CHECK(parser.vlan_id() == 42);
        CHECK(parser.ether_type() == 0x88B5);

        auto const parsed_payload = parser.payload();
        CHECK(parsed_payload.size() == payload.size());

        std::string_view const parsed_str{
            reinterpret_cast<char const*>(parsed_payload.data()),
            parsed_payload.size()
        };
        CHECK(parsed_str == payload);
    }
}
