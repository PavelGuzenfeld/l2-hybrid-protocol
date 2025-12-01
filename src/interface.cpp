// interface.cpp - network interface queries
// where we interact with the linux kernel's C api and try not to cry

#include "l2net/interface.hpp"

#include <algorithm>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace l2net
{

    namespace
    {

        // RAII wrapper for socket used in ioctl - because leaking fds is pathetic
        class ioctl_socket
        {
            int fd_{-1};

        public:
            ioctl_socket() noexcept : fd_{::socket(AF_INET, SOCK_DGRAM, 0)} {}
            ~ioctl_socket() noexcept
            {
                if (fd_ >= 0)
                    ::close(fd_);
            }
            ioctl_socket(ioctl_socket const &) = delete;
            ioctl_socket &operator=(ioctl_socket const &) = delete;
            [[nodiscard]] auto fd() const noexcept -> int { return fd_; }
            [[nodiscard]] auto is_valid() const noexcept -> bool { return fd_ >= 0; }
        };

        // RAII wrapper for ifaddrs
        class ifaddrs_guard
        {
            struct ifaddrs *addrs_{nullptr};

        public:
            ifaddrs_guard() noexcept { ::getifaddrs(&addrs_); }
            ~ifaddrs_guard() noexcept
            {
                if (addrs_)
                    ::freeifaddrs(addrs_);
            }
            ifaddrs_guard(ifaddrs_guard const &) = delete;
            ifaddrs_guard &operator=(ifaddrs_guard const &) = delete;
            [[nodiscard]] auto get() const noexcept -> struct ifaddrs * { return addrs_; }
        };

    } // anonymous namespace

    // ----------------------------------------------------------------------------
    // mac_address implementation
    // ----------------------------------------------------------------------------

    auto mac_address::from_string(std::string_view str) noexcept -> result<mac_address>
    {
        // expected format: "aa:bb:cc:dd:ee:ff" or "aa-bb-cc-dd-ee-ff"
        // separators must be consistent - no mixing colons and dashes
        if (str.size() != 17)
        {
            return std::unexpected{error_code::invalid_mac_address};
        }

        // determine separator from first occurrence (position 2)
        char const separator = str[2];
        if (separator != ':' && separator != '-')
        {
            return std::unexpected{error_code::invalid_mac_address};
        }

        storage_type bytes{};
        std::size_t byte_idx = 0;

        for (std::size_t i = 0; i < str.size() && byte_idx < 6; ++i)
        {
            if (i % 3 == 2)
            {
                // separator position - must match the first separator
                if (str[i] != separator)
                {
                    return std::unexpected{error_code::invalid_mac_address};
                }
                continue;
            }

            // parse hex digit
            auto const hex_to_int = [](char const c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };

            auto const high = hex_to_int(str[i]);
            auto const low = hex_to_int(str[i + 1]);

            if (high < 0 || low < 0)
            {
                return std::unexpected{error_code::invalid_mac_address};
            }

            bytes[byte_idx++] = static_cast<std::uint8_t>((high << 4) | low);
            ++i; // skip the low nibble we just processed
        }

        if (byte_idx != 6)
        {
            return std::unexpected{error_code::invalid_mac_address};
        }

        return mac_address{bytes};
    }

    auto mac_address::to_string() const -> std::string
    {
        return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                           bytes_[0], bytes_[1], bytes_[2], bytes_[3], bytes_[4], bytes_[5]);
    }

    // ----------------------------------------------------------------------------
    // interface_info implementation
    // ----------------------------------------------------------------------------

    auto interface_info::query(std::string_view interface_name) noexcept -> result<interface_info>
    {
        if (interface_name.empty() || interface_name.size() >= IFNAMSIZ)
        {
            return std::unexpected{error_code::interface_not_found};
        }

        ioctl_socket sock;
        if (!sock.is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct ifreq ifr{};
        // safe copy because we checked size above
        std::copy_n(interface_name.begin(),
                    std::min(interface_name.size(), static_cast<std::size_t>(IFNAMSIZ - 1)),
                    ifr.ifr_name);

        interface_info info;
        info.name_ = std::string{interface_name};

        // get index
        if (::ioctl(sock.fd(), SIOCGIFINDEX, &ifr) < 0)
        {
            return std::unexpected{error_code::interface_not_found};
        }
        info.index_ = ifr.ifr_ifindex;

        // get mac address
        if (::ioctl(sock.fd(), SIOCGIFHWADDR, &ifr) < 0)
        {
            return std::unexpected{error_code::interface_query_failed};
        }
        std::copy_n(reinterpret_cast<std::uint8_t const *>(ifr.ifr_hwaddr.sa_data),
                    mac_address::size,
                    info.mac_.data());

        // get flags
        if (::ioctl(sock.fd(), SIOCGIFFLAGS, &ifr) >= 0)
        {
            info.is_up_ = (ifr.ifr_flags & IFF_UP) != 0;
            info.is_loopback_ = (ifr.ifr_flags & IFF_LOOPBACK) != 0;
        }

        // get mtu
        if (::ioctl(sock.fd(), SIOCGIFMTU, &ifr) >= 0)
        {
            info.mtu_ = static_cast<std::uint32_t>(ifr.ifr_mtu);
        }

        return info;
    }

    auto interface_info::list_all() noexcept -> result<std::vector<interface_info>>
    {
        ifaddrs_guard addrs;
        if (!addrs.get())
        {
            return std::unexpected{error_code::interface_query_failed};
        }

        std::vector<std::string> seen_names;
        std::vector<interface_info> interfaces;

        for (auto *ifa = addrs.get(); ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (!ifa->ifa_name)
                continue;

            std::string name{ifa->ifa_name};
            // avoid duplicates (interfaces can appear multiple times for different address families)
            if (std::find(seen_names.begin(), seen_names.end(), name) != seen_names.end())
            {
                continue;
            }
            seen_names.push_back(name);

            auto result = query(name);
            if (result.has_value())
            {
                interfaces.push_back(std::move(*result));
            }
        }

        return interfaces;
    }

    // ----------------------------------------------------------------------------
    // utility functions
    // ----------------------------------------------------------------------------

    auto interface_exists(std::string_view name) noexcept -> bool
    {
        return interface_info::query(name).has_value();
    }

    auto get_loopback_interface() noexcept -> result<interface_info>
    {
        // try common loopback names
        static constexpr std::array loopback_names = {"lo", "lo0", "loopback"};

        for (auto const &name : loopback_names)
        {
            auto result = interface_info::query(name);
            if (result.has_value() && result->is_loopback())
            {
                return result;
            }
        }

        // fallback: search all interfaces
        auto all = interface_info::list_all();
        if (!all.has_value())
        {
            return std::unexpected{all.error()};
        }

        auto it = std::find_if(all->begin(), all->end(),
                               [](auto const &iface)
                               { return iface.is_loopback(); });

        if (it != all->end())
        {
            return *it;
        }

        return std::unexpected{error_code::interface_not_found};
    }

} // namespace l2net
