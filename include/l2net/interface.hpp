#pragma once

// interface.hpp - network interface abstraction
// because raw ioctl calls scattered everywhere is a war crime

#include "common.hpp"
#include <string>
#include <optional>

namespace l2net {

// ============================================================================
// network interface information - immutable value type
// ============================================================================

class interface_info {
private:
    std::string name_{};
    int index_{-1};
    mac_address mac_{};
    bool is_up_{false};
    bool is_loopback_{false};
    std::uint32_t mtu_{0};

public:
    // factory function - the only way to create this properly
    [[nodiscard]] static auto query(std::string_view interface_name) noexcept
        -> result<interface_info>;

    // query all available interfaces
    [[nodiscard]] static auto list_all() noexcept
        -> result<std::vector<interface_info>>;

    // accessors - all [[nodiscard]] because ignoring return values is a sin
    [[nodiscard]] constexpr auto name() const noexcept -> std::string_view { return name_; }
    [[nodiscard]] constexpr auto index() const noexcept -> int { return index_; }
    [[nodiscard]] constexpr auto mac() const noexcept -> mac_address const& { return mac_; }
    [[nodiscard]] constexpr auto is_up() const noexcept -> bool { return is_up_; }
    [[nodiscard]] constexpr auto is_loopback() const noexcept -> bool { return is_loopback_; }
    [[nodiscard]] constexpr auto mtu() const noexcept -> std::uint32_t { return mtu_; }

    // validation
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool {
        return index_ >= 0 && !name_.empty();
    }

    // comparison for testing
    [[nodiscard]] auto operator==(interface_info const& other) const noexcept -> bool = default;

private:
    // private constructor - use query() factory
    interface_info() noexcept = default;

    // allow factory to construct
    friend auto query_interface_impl(std::string_view name) noexcept -> result<interface_info>;
};

// ============================================================================
// interface utilities
// ============================================================================

// check if interface exists
[[nodiscard]] auto interface_exists(std::string_view name) noexcept -> bool;

// get loopback interface (usually "lo")
[[nodiscard]] auto get_loopback_interface() noexcept -> result<interface_info>;

} // namespace l2net
