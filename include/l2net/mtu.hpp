// mtu.hpp - MTU detection and payload size negotiation utilities

#pragma once

#include "common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l2net
{

    // =============================================================================
    // MTU constants
    // =============================================================================

    namespace mtu_constants
    {
        inline constexpr int standard_mtu = 1500;
        inline constexpr int jumbo_mtu = 9000;
        inline constexpr int baby_jumbo_mtu = 9216;
        inline constexpr int min_payload_size = 46; // per IEEE 802.3
        inline constexpr int min_mtu = 68;
    } // namespace mtu_constants

    // =============================================================================
    // MTU query
    // =============================================================================

    /// @brief Query the MTU of a network interface using ioctl
    [[nodiscard]] inline auto get_interface_mtu(std::string_view interface_name) noexcept
        -> result<int>
    {
        if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        {
            return std::unexpected{error_code::interface_not_found};
        }

        int const sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct socket_guard
        {
            int fd;
            ~socket_guard() { ::close(fd); }
        } guard{sock};

        ifreq ifr{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        std::strncpy(ifr.ifr_name, interface_name.data(), IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (::ioctl(sock, SIOCGIFMTU, &ifr) < 0)
        {
            if (errno == ENODEV)
            {
                return std::unexpected{error_code::interface_not_found};
            }
            return std::unexpected{error_code::interface_query_failed};
        }

        return ifr.ifr_mtu;
    }

    // =============================================================================
    // payload calculation utilities
    // =============================================================================

    [[nodiscard]] constexpr auto calculate_max_payload(int mtu, bool has_vlan = false) noexcept -> int
    {
        int const header_overhead =
            constants::eth_header_size + (has_vlan ? constants::vlan_header_size : 0);
        return mtu - header_overhead;
    }

    [[nodiscard]] constexpr auto calculate_required_mtu(int payload_size, bool has_vlan = false) noexcept -> int
    {
        int const header_overhead =
            constants::eth_header_size + (has_vlan ? constants::vlan_header_size : 0);
        return payload_size + header_overhead;
    }

    [[nodiscard]] constexpr auto payload_fits_mtu(int payload_size, int mtu, bool has_vlan = false) noexcept -> bool
    {
        return calculate_required_mtu(payload_size, has_vlan) <= mtu;
    }

    // =============================================================================
    // MTU negotiation
    // =============================================================================

    struct mtu_negotiation_result
    {
        int local_mtu;
        int remote_mtu;
        int effective_mtu;
        int max_payload;
        bool has_vlan;
        bool jumbo_capable;

        [[nodiscard]] constexpr auto can_send_payload(int size) const noexcept -> bool
        {
            return size <= max_payload && size >= mtu_constants::min_payload_size;
        }
    };

    [[nodiscard]] constexpr auto negotiate_mtu(int local_mtu, int remote_mtu, bool has_vlan = false) noexcept
        -> mtu_negotiation_result
    {
        int const effective = std::min(local_mtu, remote_mtu);
        return {
            .local_mtu = local_mtu,
            .remote_mtu = remote_mtu,
            .effective_mtu = effective,
            .max_payload = calculate_max_payload(effective, has_vlan),
            .has_vlan = has_vlan,
            .jumbo_capable = local_mtu >= mtu_constants::jumbo_mtu && remote_mtu >= mtu_constants::jumbo_mtu,
        };
    }

    template <typename Container>
    [[nodiscard]] auto filter_payload_sizes(Container const &payload_sizes, int mtu, bool has_vlan = false)
        -> std::vector<typename Container::value_type>
    {
        std::vector<typename Container::value_type> result;
        result.reserve(payload_sizes.size());

        int const max_payload = calculate_max_payload(mtu, has_vlan);

        for (auto const &size : payload_sizes)
        {
            if (static_cast<int>(size) <= max_payload)
            {
                result.push_back(size);
            }
        }

        return result;
    }

} // namespace l2net
