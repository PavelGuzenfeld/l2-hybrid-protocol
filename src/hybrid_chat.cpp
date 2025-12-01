// hybrid_chat.cpp - hybrid control/data plane implementation
// industrial protocol simulation without the industrial-grade bugs

#include "l2net/hybrid_chat.hpp"
#include "l2net/frame.hpp"

#include <algorithm>

namespace l2net
{

    // ============================================================================
    // hybrid_endpoint implementation
    // ============================================================================

    hybrid_endpoint::hybrid_endpoint(
        interface_info iface,
        hybrid_config config,
        mac_address peer,
        raw_socket socket) noexcept
        : interface_{std::move(iface)}, config_{config}, peer_mac_{peer}, data_socket_{std::move(socket)}
    {
    }

    hybrid_endpoint::~hybrid_endpoint()
    {
        stop_receiver();
    }

    hybrid_endpoint::hybrid_endpoint(hybrid_endpoint &&other) noexcept
        : interface_{std::move(other.interface_)}, config_{other.config_}, peer_mac_{other.peer_mac_}, data_socket_{std::move(other.data_socket_)}, running_{other.running_.load()}
    {
        other.running_.store(false);
        if (other.recv_thread_.joinable())
        {
            other.recv_thread_.join();
        }
    }

    auto hybrid_endpoint::operator=(hybrid_endpoint &&other) noexcept -> hybrid_endpoint &
    {
        if (this != &other)
        {
            stop_receiver();
            interface_ = std::move(other.interface_);
            config_ = other.config_;
            peer_mac_ = other.peer_mac_;
            data_socket_ = std::move(other.data_socket_);
            running_.store(other.running_.load());
            other.running_.store(false);
        }
        return *this;
    }

    auto hybrid_endpoint::create_server(
        interface_info const &iface,
        hybrid_config const &config) noexcept -> result<hybrid_endpoint>
    {
        // phase 1: tcp handshake
        auto peer_result = handshake::run_server(config.tcp_port, iface.mac(), config.tcp_timeout);
        if (!peer_result.has_value())
        {
            return std::unexpected{peer_result.error()};
        }

        // phase 2: create raw socket for data plane
        auto sock_result = raw_socket::create(raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            return std::unexpected{sock_result.error()};
        }

        return hybrid_endpoint{
            iface,
            config,
            *peer_result,
            std::move(*sock_result)};
    }

    auto hybrid_endpoint::create_client(
        interface_info const &iface,
        std::string_view const server_ip,
        hybrid_config const &config) noexcept -> result<hybrid_endpoint>
    {
        // phase 1: tcp handshake
        auto peer_result = handshake::run_client(server_ip, config.tcp_port, iface.mac(), config.tcp_timeout);
        if (!peer_result.has_value())
        {
            return std::unexpected{peer_result.error()};
        }

        // phase 2: create raw socket for data plane
        auto sock_result = raw_socket::create(raw_socket::protocol::all);
        if (!sock_result.has_value())
        {
            return std::unexpected{sock_result.error()};
        }

        return hybrid_endpoint{
            iface,
            config,
            *peer_result,
            std::move(*sock_result)};
    }

    auto hybrid_endpoint::build_vlan_frame(std::span<std::uint8_t const> payload) noexcept
        -> result<std::vector<std::uint8_t>>
    {
        vlan_tci tci{
            .priority = config_.vlan_priority,
            .dei = false,
            .vlan_id = config_.vlan_id};

        return l2net::build_vlan_frame(
            peer_mac_,
            interface_.mac(),
            tci,
            config_.data_protocol,
            payload);
    }

    auto hybrid_endpoint::send_data(std::span<std::uint8_t const> payload) noexcept -> void_result
    {
        auto frame_result = build_vlan_frame(payload);
        if (!frame_result.has_value())
        {
            return std::unexpected{frame_result.error()};
        }

        auto send_result = data_socket_.send_raw(*frame_result, interface_);
        if (!send_result.has_value())
        {
            return std::unexpected{send_result.error()};
        }

        return {};
    }

    auto hybrid_endpoint::send_data(std::string_view const payload) noexcept -> void_result
    {
        return send_data(std::span<std::uint8_t const>{
            reinterpret_cast<std::uint8_t const *>(payload.data()),
            payload.size()});
    }

    auto hybrid_endpoint::receive_data() noexcept -> result<data_message>
    {
        std::vector<std::uint8_t> buffer(config_.recv_buffer_size);

        auto recv_result = data_socket_.receive(buffer);
        if (!recv_result.has_value())
        {
            return std::unexpected{recv_result.error()};
        }

        frame_parser parser{std::span{buffer.data(), *recv_result}};
        if (!parser.is_valid())
        {
            return std::unexpected{error_code::invalid_frame_size};
        }

        // check if it's our protocol (with or without vlan tag)
        if (parser.ether_type() != config_.data_protocol)
        {
            // not our protocol
            return std::unexpected{error_code::invalid_frame_size};
        }

        data_message msg;
        msg.source = parser.src_mac();
        msg.was_tagged = parser.has_vlan();

        if (msg.was_tagged)
        {
            msg.priority = parser.vlan_priority();
            msg.vlan_id = parser.vlan_id();
        }

        auto const payload = parser.payload();
        msg.payload.assign(payload.begin(), payload.end());

        return msg;
    }

    auto hybrid_endpoint::start_receiver(message_callback callback) noexcept -> void_result
    {
        if (running_.load())
        {
            return {}; // already running
        }

        running_.store(true);
        recv_thread_ = std::thread{[this, cb = std::move(callback)]()
                                   {
                                       receiver_loop(cb);
                                   }};

        return {};
    }

    auto hybrid_endpoint::stop_receiver() noexcept -> void
    {
        running_.store(false);
        if (recv_thread_.joinable())
        {
            recv_thread_.join();
        }
    }

    auto hybrid_endpoint::send_loop(
        std::function<std::vector<std::uint8_t>()> message_generator) noexcept -> void_result
    {
        running_.store(true);

        while (running_.load())
        {
            auto const data = message_generator();
            auto result = send_data(data);
            if (!result.has_value())
            {
                return result;
            }

            std::this_thread::sleep_for(config_.send_interval);
        }

        return {};
    }

    auto hybrid_endpoint::receiver_loop(message_callback callback) noexcept -> void
    {
        std::vector<std::uint8_t> buffer(config_.recv_buffer_size);

        while (running_.load())
        {
            auto recv_result = data_socket_.receive_with_timeout(buffer, std::chrono::milliseconds{100});

            if (!recv_result.has_value())
            {
                if (recv_result.error() == error_code::timeout)
                {
                    continue; // just timeout, keep going
                }
                break; // actual error
            }

            frame_parser parser{std::span{buffer.data(), *recv_result}};
            if (!parser.is_valid())
            {
                continue;
            }

            // filter for our protocol
            if (parser.ether_type() != config_.data_protocol)
            {
                continue;
            }

            data_message msg;
            msg.source = parser.src_mac();
            msg.was_tagged = parser.has_vlan();

            if (msg.was_tagged)
            {
                msg.priority = parser.vlan_priority();
                msg.vlan_id = parser.vlan_id();
            }

            auto const payload = parser.payload();
            msg.payload.assign(payload.begin(), payload.end());

            callback(msg);
        }
    }

    // ============================================================================
    // handshake implementation
    // ============================================================================

    namespace handshake
    {

        auto run_server(
            std::uint16_t const port,
            mac_address const &local_mac,
            std::chrono::seconds const timeout) noexcept -> result<mac_address>
        {
            (void)timeout; // Suppress unused parameter warning
            auto server_result = tcp_socket::create_server(port);
            if (!server_result.has_value())
            {
                return std::unexpected{server_result.error()};
            }

            // TODO: add timeout to accept
            auto client_result = server_result->accept();
            if (!client_result.has_value())
            {
                return std::unexpected{client_result.error()};
            }

            // send our mac
            auto send_result = client_result->send(local_mac.as_span());
            if (!send_result.has_value())
            {
                return std::unexpected{send_result.error()};
            }

            // receive peer mac
            mac_address::storage_type peer_bytes{};
            auto recv_result = client_result->receive(peer_bytes);
            if (!recv_result.has_value())
            {
                return std::unexpected{recv_result.error()};
            }

            if (*recv_result != mac_address::size)
            {
                return std::unexpected{error_code::handshake_failed};
            }

            return mac_address{peer_bytes};
        }

        auto run_client(
            std::string_view const server_ip,
            std::uint16_t const port,
            mac_address const &local_mac,
            std::chrono::seconds const timeout) noexcept -> result<mac_address>
        {
            auto conn_result = tcp_socket::connect(server_ip, port, timeout);
            if (!conn_result.has_value())
            {
                return std::unexpected{conn_result.error()};
            }

            // receive server mac
            mac_address::storage_type peer_bytes{};
            auto recv_result = conn_result->receive(peer_bytes);
            if (!recv_result.has_value())
            {
                return std::unexpected{recv_result.error()};
            }

            if (*recv_result != mac_address::size)
            {
                return std::unexpected{error_code::handshake_failed};
            }

            // send our mac
            auto send_result = conn_result->send(local_mac.as_span());
            if (!send_result.has_value())
            {
                return std::unexpected{send_result.error()};
            }

            return mac_address{peer_bytes};
        }

    } // namespace handshake

} // namespace l2net
