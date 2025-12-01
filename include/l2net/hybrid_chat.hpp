#pragma once

// hybrid_chat.hpp - tcp control plane + raw data plane
// industrial protocol simulator, now without the spaghetti

#include "common.hpp"
#include "interface.hpp"
#include "raw_socket.hpp"
#include "vlan.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string_view>
#include <thread>

namespace l2net
{

    // ============================================================================
    // hybrid chat configuration
    // ============================================================================

    struct hybrid_config
    {
        std::uint16_t tcp_port{9000};
        std::uint16_t data_protocol{constants::eth_p_custom};
        std::uint16_t vlan_id{10};
        std::uint8_t vlan_priority{7}; // highest priority
        std::chrono::milliseconds send_interval{500};
        std::size_t recv_buffer_size{2048};
        std::chrono::seconds tcp_timeout{30};
    };

    // ============================================================================
    // data plane message
    // ============================================================================

    struct data_message
    {
        mac_address source;
        std::uint8_t priority{0};
        std::uint16_t vlan_id{0};
        bool was_tagged{false};
        std::vector<std::uint8_t> payload;
    };

    // ============================================================================
    // hybrid chat endpoint - server or client
    // ============================================================================

    class hybrid_endpoint
    {
    public:
        using message_callback = std::function<void(data_message const &)>;

    private:
        interface_info interface_{};
        hybrid_config config_{};
        mac_address peer_mac_{};
        raw_socket data_socket_{};
        std::atomic<bool> running_{false};
        std::thread recv_thread_{};

    public:
        hybrid_endpoint() = default;
        ~hybrid_endpoint();

        // no copy
        hybrid_endpoint(hybrid_endpoint const &) = delete;
        auto operator=(hybrid_endpoint const &) -> hybrid_endpoint & = delete;

        // move
        hybrid_endpoint(hybrid_endpoint &&) noexcept;
        auto operator=(hybrid_endpoint &&) noexcept -> hybrid_endpoint &;

        // server: wait for client connection and exchange MACs
        [[nodiscard]] static auto create_server(interface_info const &iface, hybrid_config const &config = {}) noexcept
            -> result<hybrid_endpoint>;

        // client: connect to server and exchange MACs
        [[nodiscard]] static auto create_client(interface_info const &iface, std::string_view server_ip,
                                                hybrid_config const &config = {}) noexcept -> result<hybrid_endpoint>;

        // data plane operations
        [[nodiscard]] auto send_data(std::span<std::uint8_t const> payload) noexcept -> void_result;

        [[nodiscard]] auto send_data(std::string_view payload) noexcept -> void_result;

        // blocking receive
        [[nodiscard]] auto receive_data() noexcept -> result<data_message>;

        // start background receiver
        [[nodiscard]] auto start_receiver(message_callback callback) noexcept -> void_result;

        // stop background receiver
        auto stop_receiver() noexcept -> void;

        // continuous send loop (blocking)
        [[nodiscard]] auto send_loop(std::function<std::vector<std::uint8_t>()> message_generator) noexcept
            -> void_result;

        // state
        [[nodiscard]] auto is_valid() const noexcept -> bool
        {
            return data_socket_.is_valid() && !peer_mac_.is_null();
        }

        [[nodiscard]] auto peer() const noexcept -> mac_address const &
        {
            return peer_mac_;
        }
        [[nodiscard]] auto interface() const noexcept -> interface_info const &
        {
            return interface_;
        }
        [[nodiscard]] auto config() const noexcept -> hybrid_config const &
        {
            return config_;
        }
        [[nodiscard]] auto is_running() const noexcept -> bool
        {
            return running_.load();
        }

    private:
        hybrid_endpoint(interface_info iface, hybrid_config config, mac_address peer, raw_socket socket) noexcept;

        [[nodiscard]] auto build_vlan_frame(std::span<std::uint8_t const> payload) noexcept
            -> result<std::vector<std::uint8_t>>;

        auto receiver_loop(message_callback callback) noexcept -> void;
    };

    // ============================================================================
    // handshake utilities - exposed for testing
    // ============================================================================

    namespace handshake
    {

        [[nodiscard]] auto run_server(std::uint16_t port, mac_address const &local_mac,
                                      std::chrono::seconds timeout = std::chrono::seconds{30}) noexcept
            -> result<mac_address>;

        [[nodiscard]] auto run_client(std::string_view server_ip, std::uint16_t port, mac_address const &local_mac,
                                      std::chrono::seconds timeout = std::chrono::seconds{30}) noexcept
            -> result<mac_address>;

    } // namespace handshake

} // namespace l2net
