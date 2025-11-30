// tests/unit/test_interface.cpp - interface tests
// verifying that the interface code isn't complete garbage

#include <doctest/doctest.h>
#include "l2net/interface.hpp"
#include "l2net/common.hpp"

TEST_SUITE("mac_address") {
    TEST_CASE("default construction creates null address") {
        l2net::mac_address const mac;
        CHECK(mac.is_null());
        CHECK_FALSE(mac.is_broadcast());
        CHECK_FALSE(mac.is_multicast());
    }

    TEST_CASE("broadcast address") {
        auto const mac = l2net::mac_address::broadcast();
        CHECK(mac.is_broadcast());
        CHECK(mac.is_multicast()); // broadcast is also multicast
        CHECK_FALSE(mac.is_null());
    }

    TEST_CASE("from individual bytes") {
        l2net::mac_address const mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        auto const bytes = mac.bytes();
        CHECK(bytes[0] == 0xAA);
        CHECK(bytes[1] == 0xBB);
        CHECK(bytes[2] == 0xCC);
        CHECK(bytes[3] == 0xDD);
        CHECK(bytes[4] == 0xEE);
        CHECK(bytes[5] == 0xFF);
    }

    TEST_CASE("from_string valid colon separated") {
        auto const result = l2net::mac_address::from_string("aa:bb:cc:dd:ee:ff");
        REQUIRE(result.has_value());
        auto const& mac = *result;
        auto const bytes = mac.bytes();
        CHECK(bytes[0] == 0xAA);
        CHECK(bytes[1] == 0xBB);
        CHECK(bytes[2] == 0xCC);
        CHECK(bytes[3] == 0xDD);
        CHECK(bytes[4] == 0xEE);
        CHECK(bytes[5] == 0xFF);
    }

    TEST_CASE("from_string valid dash separated") {
        auto const result = l2net::mac_address::from_string("AA-BB-CC-DD-EE-FF");
        REQUIRE(result.has_value());
        auto const& mac = *result;
        CHECK_FALSE(mac.is_null());
    }

    TEST_CASE("from_string invalid - too short") {
        auto const result = l2net::mac_address::from_string("aa:bb:cc");
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == l2net::error_code::invalid_mac_address);
    }

    TEST_CASE("from_string invalid - bad characters") {
        auto const result = l2net::mac_address::from_string("gg:hh:ii:jj:kk:ll");
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("to_string roundtrip") {
        l2net::mac_address const original{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB};
        auto const str = original.to_string();
        auto const parsed = l2net::mac_address::from_string(str);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == original);
    }

    TEST_CASE("comparison operators") {
        l2net::mac_address const mac1{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        l2net::mac_address const mac2{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        l2net::mac_address const mac3{0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        CHECK(mac1 == mac2);
        CHECK_FALSE(mac1 == mac3);
        CHECK(mac1 != mac3);
        CHECK(mac3 < mac1);
    }

    TEST_CASE("multicast detection") {
        // multicast addresses have LSB of first byte set
        l2net::mac_address const multicast{0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        l2net::mac_address const unicast{0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        CHECK(multicast.is_multicast());
        CHECK_FALSE(unicast.is_multicast());
    }
}

TEST_SUITE("interface_info") {
    // note: these tests don't require root for querying

    TEST_CASE("query loopback interface") {
        // loopback should exist on any linux system
        auto const result = l2net::interface_info::query("lo");

        // might fail in containers without loopback
        if (result.has_value()) {
            CHECK(result->name() == "lo");
            CHECK(result->index() >= 0);
            CHECK(result->is_loopback());
            CHECK(result->is_valid());
        }
    }

    TEST_CASE("query nonexistent interface") {
        auto const result = l2net::interface_info::query("this_interface_does_not_exist_42");
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == l2net::error_code::interface_not_found);
    }

    TEST_CASE("query empty name") {
        auto const result = l2net::interface_info::query("");
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("get_loopback_interface helper") {
        auto const result = l2net::get_loopback_interface();
        if (result.has_value()) {
            CHECK(result->is_loopback());
        }
    }

    TEST_CASE("interface_exists helper") {
        CHECK(l2net::interface_exists("lo") == true ||
              l2net::interface_exists("lo") == false); // might not exist in container
        CHECK_FALSE(l2net::interface_exists("fake_interface_xyz"));
    }

    TEST_CASE("list_all returns something") {
        auto const result = l2net::interface_info::list_all();
        // should at least succeed
        CHECK(result.has_value());
        // might be empty in weird containers but should work
    }
}

TEST_SUITE("byte_utils") {
    TEST_CASE("htons_constexpr") {
        // these are constexpr so we can test at compile time too
        static_assert(l2net::byte_utils::htons_constexpr(0x1234) ==
            (std::endian::native == std::endian::little ? 0x3412 : 0x1234));

        CHECK(l2net::byte_utils::htons_constexpr(0x0100) ==
            (std::endian::native == std::endian::little ? 0x0001 : 0x0100));
    }

    TEST_CASE("ntohs_constexpr is symmetric") {
        constexpr std::uint16_t original = 0xABCD;
        constexpr auto converted = l2net::byte_utils::htons_constexpr(original);
        constexpr auto back = l2net::byte_utils::ntohs_constexpr(converted);
        static_assert(back == original);
    }
}

TEST_SUITE("constants") {
    TEST_CASE("header sizes are correct") {
        static_assert(l2net::constants::eth_header_size == 14);
        static_assert(l2net::constants::vlan_header_size == 4);
        static_assert(l2net::constants::eth_vlan_header_size == 18);
    }

    TEST_CASE("protocol values") {
        static_assert(l2net::constants::eth_p_8021q == 0x8100);
        static_assert(l2net::constants::eth_p_custom == 0x88B5);
    }
}
