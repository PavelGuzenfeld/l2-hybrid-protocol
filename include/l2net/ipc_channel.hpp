#pragma once

// ipc_channel.hpp - local IPC over L2 loopback
// because shared memory is too mainstream

#include "common.hpp"
#include "frame.hpp"
#include "raw_socket.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace l2net
{

    // ============================================================================
    // ipc channel configuration
    // ============================================================================

    struct ipc_config
    {
        std::string_view interface_name{"lo"};
        std::uint16_t protocol_id{constants::eth_p_ipc};
        std::size_t recv_buffer_size{70000}; // loopback can handle jumbo frames
        std::optional<std::chrono::milliseconds> recv_timeout{};
    };

    // ============================================================================
    // ipc channel - bidirectional local communication
    // ============================================================================

    class ipc_channel
    {
    public:
        // message callback type
        using message_callback = std::function<void(std::span<std::uint8_t const>)>;

    private:
        raw_socket socket_{};
        interface_info interface_{};
        ipc_config config_{};
        std::vector<std::uint8_t> recv_buffer_{};

    public:
        ipc_channel() = default;
        ~ipc_channel() = default;

        // no copy, yes move
        ipc_channel(ipc_channel const &) = delete;
        auto operator=(ipc_channel const &) -> ipc_channel & = delete;
        ipc_channel(ipc_channel &&) noexcept = default;
        auto operator=(ipc_channel &&) noexcept -> ipc_channel & = default;

        // factory
        [[nodiscard]] static auto create(ipc_config const &config = {}) noexcept -> result<ipc_channel>;

        // send message
        [[nodiscard]] auto send(std::span<std::uint8_t const> data) noexcept -> result<std::size_t>;

        [[nodiscard]] auto send(std::string_view message) noexcept -> result<std::size_t>;

        // receive message (blocking)
        [[nodiscard]] auto receive() noexcept -> result<std::vector<std::uint8_t>>;

        // receive with timeout
        [[nodiscard]] auto receive_with_timeout(std::chrono::milliseconds timeout) noexcept
            -> result<std::vector<std::uint8_t>>;

        // try receive (non-blocking)
        [[nodiscard]] auto try_receive() noexcept -> result<std::optional<std::vector<std::uint8_t>>>;

        // receive loop with callback
        [[nodiscard]] auto receive_loop(message_callback const &callback) noexcept -> void_result;

        // state
        [[nodiscard]] auto is_valid() const noexcept -> bool
        {
            return socket_.is_valid();
        }

        [[nodiscard]] auto interface() const noexcept -> interface_info const &
        {
            return interface_;
        }

    private:
        ipc_channel(raw_socket socket, interface_info iface, ipc_config config) noexcept;
    };

    // ============================================================================
    // ipc pair - convenience for creating sender/receiver pair
    // ============================================================================

    struct ipc_pair
    {
        ipc_channel sender;
        ipc_channel receiver;
    };

    [[nodiscard]] auto create_ipc_pair(ipc_config const &config = {}) noexcept -> result<ipc_pair>;

} // namespace l2net
