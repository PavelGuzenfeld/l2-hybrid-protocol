#pragma once

#include "common.hpp"
#include <optional>
#include <string>
#include <vector> // FIXED: Added vector

namespace l2net
{

    class interface_info
    {
    private:
        std::string name_{};
        int index_{-1};
        mac_address mac_{};
        bool is_up_{false};
        bool is_loopback_{false};
        std::uint32_t mtu_{0};

    public:
        [[nodiscard]] static auto query(std::string_view interface_name) noexcept -> result<interface_info>;
        [[nodiscard]] static auto list_all() noexcept -> result<std::vector<interface_info>>;

        // FIXED: Made public so other classes can hold it
        interface_info() noexcept = default;

        [[nodiscard]] constexpr auto name() const noexcept -> std::string_view { return name_; }
        [[nodiscard]] constexpr auto index() const noexcept -> int { return index_; }
        [[nodiscard]] constexpr auto mac() const noexcept -> mac_address const & { return mac_; }
        [[nodiscard]] constexpr auto is_up() const noexcept -> bool { return is_up_; }
        [[nodiscard]] constexpr auto is_loopback() const noexcept -> bool { return is_loopback_; }
        [[nodiscard]] constexpr auto mtu() const noexcept -> std::uint32_t { return mtu_; }

        [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
        {
            return index_ >= 0 && !name_.empty();
        }

        [[nodiscard]] auto operator==(interface_info const &other) const noexcept -> bool = default;

    private:
        friend auto query_interface_impl(std::string_view name) noexcept -> result<interface_info>;
    };

    [[nodiscard]] auto interface_exists(std::string_view name) noexcept -> bool;
    [[nodiscard]] auto get_loopback_interface() noexcept -> result<interface_info>;

} // namespace l2net