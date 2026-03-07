// test_mtu.cpp - unit tests for MTU detection and payload calculation

#include "l2net/mtu.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace l2net;

// =============================================================================
// constants tests
// =============================================================================

TEST_SUITE("mtu_constants")
{
    TEST_CASE("ethernet header size is 14 bytes")
    {
        CHECK(constants::eth_header_size == 14);
    }

    TEST_CASE("vlan header size is 4 bytes")
    {
        CHECK(constants::vlan_header_size == 4);
    }

    TEST_CASE("standard mtu is 1500")
    {
        CHECK(mtu_constants::standard_mtu == 1500);
    }

    TEST_CASE("jumbo mtu is 9000")
    {
        CHECK(mtu_constants::jumbo_mtu == 9000);
    }

    TEST_CASE("minimum payload per IEEE 802.3 is 46 bytes")
    {
        CHECK(mtu_constants::min_payload_size == 46);
    }
}

// =============================================================================
// calculate_max_payload tests
// =============================================================================

TEST_SUITE("calculate_max_payload")
{
    TEST_CASE("standard ethernet without vlan")
    {
        CHECK(calculate_max_payload(1500) == 1486);
    }

    TEST_CASE("standard ethernet with vlan")
    {
        CHECK(calculate_max_payload(1500, true) == 1482);
    }

    TEST_CASE("jumbo frames without vlan")
    {
        CHECK(calculate_max_payload(9000) == 8986);
    }

    TEST_CASE("jumbo frames with vlan")
    {
        CHECK(calculate_max_payload(9000, true) == 8982);
    }

    TEST_CASE("is constexpr")
    {
        constexpr auto result = calculate_max_payload(1500);
        static_assert(result == 1486);
        CHECK(result == 1486);
    }
}

// =============================================================================
// calculate_required_mtu tests
// =============================================================================

TEST_SUITE("calculate_required_mtu")
{
    TEST_CASE("small payload without vlan")
    {
        CHECK(calculate_required_mtu(64) == 78);
    }

    TEST_CASE("small payload with vlan")
    {
        CHECK(calculate_required_mtu(64, true) == 82);
    }

    TEST_CASE("max standard payload without vlan")
    {
        CHECK(calculate_required_mtu(1486) == 1500);
    }

    TEST_CASE("max standard payload with vlan")
    {
        CHECK(calculate_required_mtu(1482, true) == 1500);
    }

    TEST_CASE("4096 byte payload requires jumbo frames")
    {
        CHECK(calculate_required_mtu(4096) == 4110);
        CHECK(calculate_required_mtu(4096) > mtu_constants::standard_mtu);
    }

    TEST_CASE("is constexpr")
    {
        constexpr auto result = calculate_required_mtu(1400);
        static_assert(result == 1414);
        CHECK(result == 1414);
    }
}

// =============================================================================
// payload_fits_mtu tests
// =============================================================================

TEST_SUITE("payload_fits_mtu")
{
    TEST_CASE("64 bytes fits standard mtu")
    {
        CHECK(payload_fits_mtu(64, 1500) == true);
    }

    TEST_CASE("1400 bytes fits standard mtu")
    {
        CHECK(payload_fits_mtu(1400, 1500) == true);
    }

    TEST_CASE("1486 bytes exactly fits standard mtu")
    {
        CHECK(payload_fits_mtu(1486, 1500) == true);
    }

    TEST_CASE("1487 bytes exceeds standard mtu")
    {
        CHECK(payload_fits_mtu(1487, 1500) == false);
    }

    TEST_CASE("4096 bytes does not fit standard mtu")
    {
        CHECK(payload_fits_mtu(4096, 1500) == false);
    }

    TEST_CASE("4096 bytes fits jumbo mtu")
    {
        CHECK(payload_fits_mtu(4096, 9000) == true);
    }

    TEST_CASE("vlan reduces available space")
    {
        CHECK(payload_fits_mtu(1486, 1500, false) == true);
        CHECK(payload_fits_mtu(1486, 1500, true) == false);
        CHECK(payload_fits_mtu(1482, 1500, true) == true);
    }

    TEST_CASE("is constexpr")
    {
        constexpr bool fits = payload_fits_mtu(1400, 1500);
        static_assert(fits == true);
        CHECK(fits);
    }
}

// =============================================================================
// negotiate_mtu tests
// =============================================================================

TEST_SUITE("negotiate_mtu")
{
    TEST_CASE("symmetric mtu")
    {
        auto const result = negotiate_mtu(1500, 1500);
        CHECK(result.local_mtu == 1500);
        CHECK(result.remote_mtu == 1500);
        CHECK(result.effective_mtu == 1500);
        CHECK(result.max_payload == 1486);
        CHECK(result.has_vlan == false);
        CHECK(result.jumbo_capable == false);
    }

    TEST_CASE("asymmetric mtu uses minimum")
    {
        auto const result = negotiate_mtu(9000, 1500);
        CHECK(result.local_mtu == 9000);
        CHECK(result.remote_mtu == 1500);
        CHECK(result.effective_mtu == 1500);
        CHECK(result.max_payload == 1486);
        CHECK(result.jumbo_capable == false);
    }

    TEST_CASE("both jumbo capable")
    {
        auto const result = negotiate_mtu(9000, 9000);
        CHECK(result.effective_mtu == 9000);
        CHECK(result.max_payload == 8986);
        CHECK(result.jumbo_capable == true);
    }

    TEST_CASE("with vlan")
    {
        auto const result = negotiate_mtu(1500, 1500, true);
        CHECK(result.effective_mtu == 1500);
        CHECK(result.max_payload == 1482);
        CHECK(result.has_vlan == true);
    }

    TEST_CASE("can_send_payload checks")
    {
        auto const result = negotiate_mtu(1500, 1500);

        CHECK(result.can_send_payload(64) == true);
        CHECK(result.can_send_payload(1400) == true);
        CHECK(result.can_send_payload(1486) == true);
        CHECK(result.can_send_payload(1487) == false);
        CHECK(result.can_send_payload(4096) == false);

        CHECK(result.can_send_payload(46) == true);
        CHECK(result.can_send_payload(45) == false);
    }

    TEST_CASE("is constexpr")
    {
        constexpr auto result = negotiate_mtu(1500, 9000);
        static_assert(result.effective_mtu == 1500);
        static_assert(result.max_payload == 1486);
        CHECK(result.effective_mtu == 1500);
    }
}

// =============================================================================
// filter_payload_sizes tests
// =============================================================================

TEST_SUITE("filter_payload_sizes")
{
    TEST_CASE("filters out oversized payloads")
    {
        std::vector<std::size_t> const sizes{64, 256, 512, 1024, 1400, 4096, 8192};
        auto const filtered = filter_payload_sizes(sizes, 1500);

        REQUIRE(filtered.size() == 5);
        CHECK(filtered[0] == 64);
        CHECK(filtered[1] == 256);
        CHECK(filtered[2] == 512);
        CHECK(filtered[3] == 1024);
        CHECK(filtered[4] == 1400);
    }

    TEST_CASE("keeps all with jumbo mtu")
    {
        std::vector<std::size_t> const sizes{64, 256, 512, 1024, 1400, 4096, 8192};
        auto const filtered = filter_payload_sizes(sizes, 9000);

        REQUIRE(filtered.size() == 7);
    }

    TEST_CASE("handles empty input")
    {
        std::vector<std::size_t> const sizes{};
        auto const filtered = filter_payload_sizes(sizes, 1500);

        CHECK(filtered.empty());
    }

    TEST_CASE("vlan affects filtering")
    {
        std::vector<std::size_t> const sizes{1400, 1482, 1486};

        auto const without_vlan = filter_payload_sizes(sizes, 1500, false);
        REQUIRE(without_vlan.size() == 3);

        auto const with_vlan = filter_payload_sizes(sizes, 1500, true);
        REQUIRE(with_vlan.size() == 2);
        CHECK(with_vlan[0] == 1400);
        CHECK(with_vlan[1] == 1482);
    }
}

// =============================================================================
// get_interface_mtu tests
// =============================================================================

TEST_SUITE("get_interface_mtu")
{
    TEST_CASE("loopback interface exists on all systems")
    {
        auto const result = get_interface_mtu("lo");

        if (result.has_value())
        {
            CHECK(result.value() > 0);
            CHECK(result.value() >= 1500);
            MESSAGE("lo MTU: ", result.value());
        }
        else
        {
            MESSAGE("loopback not available");
        }
    }

    TEST_CASE("nonexistent interface returns error")
    {
        auto const result = get_interface_mtu("this_interface_definitely_does_not_exist_12345");

        CHECK(!result.has_value());
    }

    TEST_CASE("empty interface name returns error")
    {
        auto const result = get_interface_mtu("");

        CHECK(!result.has_value());
        CHECK(result.error() == error_code::interface_not_found);
    }

    TEST_CASE("too long interface name returns error")
    {
        std::string const long_name(20, 'x');
        auto const result = get_interface_mtu(long_name);

        CHECK(!result.has_value());
        CHECK(result.error() == error_code::interface_not_found);
    }
}

// =============================================================================
// integration scenarios
// =============================================================================

TEST_SUITE("mtu_integration")
{
    TEST_CASE("benchmark payload filtering scenario")
    {
        std::vector<std::size_t> const requested_sizes{64, 128, 256, 512, 1024, 1400, 4096, 8192};

        SUBCASE("standard ethernet filters large payloads")
        {
            auto const negotiated = negotiate_mtu(1500, 1500);
            auto const filtered = filter_payload_sizes(requested_sizes, negotiated.effective_mtu);

            CHECK(filtered.size() == 6);
            CHECK(filtered.back() == 1400);
        }

        SUBCASE("jumbo frames allow all payloads")
        {
            auto const negotiated = negotiate_mtu(9000, 9000);
            auto const filtered = filter_payload_sizes(requested_sizes, negotiated.effective_mtu);

            CHECK(filtered.size() == 8);
        }

        SUBCASE("asymmetric MTU uses minimum")
        {
            auto const negotiated = negotiate_mtu(9000, 1500);
            auto const filtered = filter_payload_sizes(requested_sizes, negotiated.effective_mtu);

            CHECK(negotiated.effective_mtu == 1500);
            CHECK(filtered.size() == 6);
        }
    }

    TEST_CASE("common payload sizes fit standard mtu")
    {
        auto const mtu = mtu_constants::standard_mtu;

        CHECK(payload_fits_mtu(64, mtu));
        CHECK(payload_fits_mtu(128, mtu));
        CHECK(payload_fits_mtu(256, mtu));
        CHECK(payload_fits_mtu(512, mtu));
        CHECK(payload_fits_mtu(1024, mtu));
        CHECK(payload_fits_mtu(1400, mtu));

        CHECK(!payload_fits_mtu(4096, mtu));
        CHECK(!payload_fits_mtu(8192, mtu));
    }
}
