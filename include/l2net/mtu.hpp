// mtu.hpp - MTU detection and payload size negotiation utilities
// because raw L2 doesn't have IP fragmentation and users shouldn't have to
// learn that the hard way when their 4096 byte packets vanish into the void

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <expected>
#include <string_view>
#include <system_error>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l2net
{

    // =============================================================================
    // constants - because magic numbers are for amateurs
    // =============================================================================

    namespace mtu_constants
    {

        // ethernet header: 6 bytes dst MAC + 6 bytes src MAC + 2 bytes ethertype = 14 bytes
        inline constexpr int eth_header_size = 14;

        // 802.1Q VLAN tag adds 4 bytes
        inline constexpr int vlan_tag_size = 4;

        // standard ethernet MTU - the one your grandma's router uses
        inline constexpr int standard_mtu = 1500;

        // jumbo frame MTU - for when you have actual datacenter equipment
        inline constexpr int jumbo_mtu = 9000;

        // baby jumbo / 9K frames commonly supported by enterprise gear
        inline constexpr int baby_jumbo_mtu = 9216;

        // minimum ethernet payload (per IEEE 802.3)
        inline constexpr int min_payload_size = 46;

        // absolute minimum MTU for ethernet
        inline constexpr int min_mtu = 68;

    } // namespace mtu_constants

    // =============================================================================
    // error codes - for when things go predictably wrong
    // =============================================================================

    enum class mtu_error
    {
        success = 0,
        socket_creation_failed,
        ioctl_failed,
        interface_not_found,
        invalid_interface_name,
    };

    [[nodiscard]] inline auto mtu_error_category() noexcept -> std::error_category const &
    {
        static struct : std::error_category
        {
            [[nodiscard]] auto name() const noexcept -> char const * override { return "l2net::mtu"; }

            [[nodiscard]] auto message(int ev) const -> std::string override
            {
                switch (static_cast<mtu_error>(ev))
                {
                case mtu_error::success:
                    return "success";
                case mtu_error::socket_creation_failed:
                    return "failed to create socket for ioctl";
                case mtu_error::ioctl_failed:
                    return "ioctl SIOCGIFMTU failed";
                case mtu_error::interface_not_found:
                    return "network interface not found";
                case mtu_error::invalid_interface_name:
                    return "invalid interface name (too long or empty)";
                default:
                    return "unknown mtu error";
                }
            }
        } instance;
        return instance;
    }

    [[nodiscard]] inline auto make_error_code(mtu_error e) noexcept -> std::error_code
    {
        return {static_cast<int>(e), mtu_error_category()};
    }

} // namespace l2net

// make mtu_error usable as std::error_code
template <>
struct std::is_error_code_enum<l2net::mtu_error> : std::true_type
{
};

namespace l2net
{

    // =============================================================================
    // MTU query functions
    // =============================================================================

    /// @brief Query the MTU of a network interface using ioctl
    /// @param interface_name Name of the network interface (e.g., "eth0", "enp0s3")
    /// @return MTU value on success, error code on failure
    /// @note Requires no special privileges - anyone can query MTU
    [[nodiscard]] inline auto get_interface_mtu(std::string_view interface_name) noexcept
        -> std::expected<int, std::error_code>
    {
        // validate interface name - IFNAMSIZ is typically 16
        if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        {
            return std::unexpected(make_error_code(mtu_error::invalid_interface_name));
        }

        // create a temporary socket for the ioctl call
        // doesn't need to be bound or connected - just needs to exist
        int const sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        // RAII socket closer because we're not savages who leak file descriptors
        struct socket_guard
        {
            int fd;
            ~socket_guard() { ::close(fd); }
        } guard{sock};

        // prepare the interface request structure
        ifreq ifr{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay) - C API requires this
        std::strncpy(ifr.ifr_name, interface_name.data(), IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0'; // paranoid null termination

        // query the MTU
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - ioctl is a C API
        if (::ioctl(sock, SIOCGIFMTU, &ifr) < 0)
        {
            if (errno == ENODEV)
            {
                return std::unexpected(make_error_code(mtu_error::interface_not_found));
            }
            return std::unexpected(std::error_code(errno, std::system_category()));
        }

        return ifr.ifr_mtu;
    }

    // =============================================================================
    // payload calculation utilities
    // =============================================================================

    /// @brief Calculate maximum L2 payload size given an MTU
    /// @param mtu The interface MTU
    /// @param has_vlan Whether 802.1Q VLAN tagging is used
    /// @return Maximum payload size that won't exceed MTU
    [[nodiscard]] constexpr auto calculate_max_payload(int mtu, bool has_vlan = false) noexcept -> int
    {
        int const header_overhead = mtu_constants::eth_header_size + (has_vlan ? mtu_constants::vlan_tag_size : 0);
        return mtu - header_overhead;
    }

    /// @brief Calculate minimum MTU required for a given payload
    /// @param payload_size The desired payload size
    /// @param has_vlan Whether 802.1Q VLAN tagging is used
    /// @return Minimum MTU required to send this payload
    [[nodiscard]] constexpr auto calculate_required_mtu(int payload_size, bool has_vlan = false) noexcept -> int
    {
        int const header_overhead = mtu_constants::eth_header_size + (has_vlan ? mtu_constants::vlan_tag_size : 0);
        return payload_size + header_overhead;
    }

    /// @brief Check if a payload size fits within an MTU
    /// @param payload_size The payload size to check
    /// @param mtu The interface MTU
    /// @param has_vlan Whether 802.1Q VLAN tagging is used
    /// @return true if the payload fits, false otherwise
    [[nodiscard]] constexpr auto payload_fits_mtu(int payload_size, int mtu, bool has_vlan = false) noexcept -> bool
    {
        return calculate_required_mtu(payload_size, has_vlan) <= mtu;
    }

    // =============================================================================
    // MTU negotiation result
    // =============================================================================

    /// @brief Result of MTU negotiation between two endpoints
    struct mtu_negotiation_result
    {
        int local_mtu;      ///< MTU of local interface
        int remote_mtu;     ///< MTU of remote interface
        int effective_mtu;  ///< min(local_mtu, remote_mtu)
        int max_payload;    ///< maximum safe payload size
        bool has_vlan;      ///< whether VLAN tagging is in use
        bool jumbo_capable; ///< true if both sides support jumbo frames

        /// @brief Check if a payload size is safe to send
        [[nodiscard]] constexpr auto can_send_payload(int size) const noexcept -> bool
        {
            return size <= max_payload && size >= mtu_constants::min_payload_size;
        }
    };

    /// @brief Negotiate MTU between local and remote interfaces
    /// @param local_mtu The local interface MTU
    /// @param remote_mtu The remote interface MTU
    /// @param has_vlan Whether VLAN tagging is used
    /// @return Negotiation result with effective MTU and max payload
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

    /// @brief Filter a list of payload sizes to only include MTU-safe values
    /// @tparam Container Container type with push_back support
    /// @param payload_sizes Input payload sizes
    /// @param mtu The effective MTU to check against
    /// @param has_vlan Whether VLAN tagging is used
    /// @return Vector of filtered payload sizes
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