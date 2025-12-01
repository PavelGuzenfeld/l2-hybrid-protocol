// tests/unit/test_vlan.cpp

#include "l2net/frame.hpp"
#include "l2net/vlan.hpp"
#include "test_helpers.hpp"

#include <doctest/doctest.h>

using namespace l2net;
using namespace l2net::testing;

TEST_SUITE("vlan_tci")
{
    TEST_CASE("default construction")
    {
        vlan_tci const tci{};
        CHECK(tci.priority == 0);
        CHECK(tci.dei == false);
        CHECK(tci.vlan_id == 0);
        CHECK(tci.is_valid());
    }

    TEST_CASE("encode/decode roundtrip")
    {
        vlan_tci const original{.priority = 7, .dei = true, .vlan_id = 100};

        auto const encoded = original.encode();
        auto const decoded = vlan_tci::decode(encoded);

        CHECK(decoded.priority == original.priority);
        CHECK(decoded.dei == original.dei);
        CHECK(decoded.vlan_id == original.vlan_id);
    }

    TEST_CASE("encode specific values")
    {
        vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
        CHECK(tci.encode() == 0xE00A);
    }

    TEST_CASE("validation - invalid priority")
    {
        vlan_tci const tci{.priority = 8, .dei = false, .vlan_id = 10};
        CHECK_FALSE(tci.is_valid());
    }

    TEST_CASE("validation - invalid vlan_id")
    {
        vlan_tci const tci{.priority = 0, .dei = false, .vlan_id = 4096};
        CHECK_FALSE(tci.is_valid());
    }

    TEST_CASE("comparison")
    {
        vlan_tci const a{.priority = 5, .dei = false, .vlan_id = 100};
        vlan_tci const b{.priority = 5, .dei = false, .vlan_id = 100};
        vlan_tci const c{.priority = 3, .dei = false, .vlan_id = 100};

        CHECK(a == b);
        CHECK_FALSE(a == c);
    }
}

TEST_SUITE("vlan_header")
{
    TEST_CASE("size is correct")
    {
        static_assert(sizeof(vlan_header) == 4);
    }

    TEST_CASE("is_8021q detection")
    {
        vlan_header header{};
        header.tpid = byte_utils::htons_constexpr(0x8100);
        CHECK(header.is_8021q());

        header.tpid = byte_utils::htons_constexpr(0x0800);
        CHECK_FALSE(header.is_8021q());
    }
}

TEST_SUITE("vlan_frame_builder")
{
    TEST_CASE("build basic vlan frame")
    {
        auto result = vlan_frame_builder{}
                          .set_dest(mac_address::broadcast())
                          .set_src(TEST_SRC_MAC)
                          .set_vlan_id(10)
                          .set_priority(7)
                          .set_inner_ether_type(0x88B5)
                          .set_payload("test")
                          .build();

        REQUIRE(result.has_value());
        CHECK(result->size() == constants::eth_vlan_header_size + 4);

        verify_frame_header(*result, mac_address::broadcast(), TEST_SRC_MAC, 0x88B5);
        verify_vlan_tag(*result, vlan_tci{.priority = 7, .dei = false, .vlan_id = 10});
    }

    TEST_CASE("verify frame structure")
    {
        auto result = vlan_frame_builder{}
                          .set_dest(TEST_DEST_MAC)
                          .set_src(TEST_SRC_MAC)
                          .set_vlan_id(10)
                          .set_priority(7)
                          .set_inner_ether_type(0x88B5)
                          .build();

        REQUIRE(result.has_value());
        auto const &frame = *result;

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

    TEST_CASE("validation fails for invalid vlan_id")
    {
        auto builder = vlan_frame_builder{}.set_vlan_id(5000); // invalid

        auto validation = builder.validate();
        CHECK_FALSE(validation.has_value());
        CHECK(validation.error() == error_code::invalid_vlan_id);
    }

    TEST_CASE("validation fails for invalid priority")
    {
        auto builder = vlan_frame_builder{}.set_priority(10); // invalid

        auto validation = builder.validate();
        CHECK_FALSE(validation.has_value());
        CHECK(validation.error() == error_code::invalid_priority);
    }

    TEST_CASE("build with tci struct")
    {
        vlan_tci const tci{.priority = 5, .dei = true, .vlan_id = 200};

        auto result = vlan_frame_builder{}
                          .set_dest(mac_address::broadcast())
                          .set_src(mac_address::null())
                          .set_tci(tci)
                          .set_inner_ether_type(0x0800)
                          .build();

        REQUIRE(result.has_value());

        verify_vlan_tag(*result, tci);
    }

    TEST_CASE("reset clears state")
    {
        auto builder = vlan_frame_builder{}.set_vlan_id(100).set_priority(7).set_payload("lots of data here");

        auto const size_before = builder.required_size();
        builder.reset();

        CHECK(builder.required_size() < size_before);
    }
}

TEST_SUITE("vlan convenience functions")
{
    TEST_CASE("build_vlan_frame string payload")
    {
        vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};

        auto result = build_vlan_frame(mac_address::broadcast(), mac_address::null(), tci, 0x88B5, "test message");

        REQUIRE(result.has_value());
        CHECK(result->size() == constants::eth_vlan_header_size + 12);
    }

    TEST_CASE("build_vlan_frame binary payload")
    {
        vlan_tci const tci{.priority = 3, .dei = false, .vlan_id = 50};
        std::vector<std::uint8_t> const payload{0x01, 0x02, 0x03};

        auto result = build_vlan_frame(mac_address::broadcast(), mac_address::null(), tci, 0x0800, payload);

        REQUIRE(result.has_value());
    }

    TEST_CASE("is_vlan_tagged detection")
    {
        // untagged frame
        std::vector<std::uint8_t> untagged(14);
        untagged[12] = 0x08;
        untagged[13] = 0x00; // IPv4
        CHECK_FALSE(is_vlan_tagged(untagged));

        // tagged frame
        std::vector<std::uint8_t> tagged(18);
        tagged[12] = 0x81;
        tagged[13] = 0x00; // 802.1Q
        CHECK(is_vlan_tagged(tagged));
    }

    TEST_CASE("is_vlan_tagged too small")
    {
        std::vector<std::uint8_t> const tiny(10);
        CHECK_FALSE(is_vlan_tagged(tiny));
    }

    TEST_CASE("strip_vlan_tag from tagged frame")
    {
        // build a tagged frame
        vlan_tci const tci{.priority = 5, .dei = false, .vlan_id = 100};
        auto tagged = build_vlan_frame(mac_address::broadcast(), mac_address::null(), tci, 0x0800, "payload");
        REQUIRE(tagged.has_value());

        // strip the tag
        auto stripped = strip_vlan_tag(*tagged);
        REQUIRE(stripped.has_value());

        // should be 4 bytes smaller
        CHECK(stripped->size() == tagged->size() - 4);

        // should no longer be tagged
        CHECK_FALSE(is_vlan_tagged(*stripped));

        // ether type should be preserved
        frame_parser parser{*stripped};
        CHECK(parser.ether_type() == 0x0800);
    }

    TEST_CASE("strip_vlan_tag from untagged frame")
    {
        auto untagged = build_simple_frame(mac_address::broadcast(), mac_address::null(), 0x0800, "test");
        REQUIRE(untagged.has_value());

        // should return copy unchanged
        auto result = strip_vlan_tag(*untagged);
        REQUIRE(result.has_value());
        CHECK(result->size() == untagged->size());
    }
}

TEST_SUITE("vlan roundtrip")
{
    TEST_CASE("build and parse vlan frame")
    {
        vlan_tci const tci{.priority = 6, .dei = false, .vlan_id = 42};
        std::string_view const payload = "hello vlan world";

        auto frame = build_vlan_frame(TEST_DEST_MAC, TEST_SRC_MAC, tci, 0x88B5, payload);
        REQUIRE(frame.has_value());

        frame_parser parser{*frame};

        CHECK(parser.is_valid());
        CHECK(parser.has_vlan());
        CHECK(parser.dest_mac() == TEST_DEST_MAC);
        CHECK(parser.src_mac() == TEST_SRC_MAC);
        CHECK(parser.vlan_priority() == 6);
        CHECK(parser.vlan_id() == 42);
        CHECK(parser.ether_type() == 0x88B5);

        auto const parsed_payload = parser.payload();
        CHECK(parsed_payload.size() == payload.size());

        std::string_view const parsed_str{reinterpret_cast<char const *>(parsed_payload.data()), parsed_payload.size()};
        CHECK(parsed_str == payload);
    }
}
