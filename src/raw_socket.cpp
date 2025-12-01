// raw_socket.cpp - raw socket implementation
// the part where we actually create sockets without embarrassing ourselves

// GCC false positive suppression for std::optional<std::string>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include "l2net/raw_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace l2net
{

    // ============================================================================
    // raw_socket implementation
    // ============================================================================

    raw_socket::raw_socket(int const fd, protocol const proto) noexcept
        : fd_{fd}, proto_{proto}
    {
    }

    raw_socket::~raw_socket() noexcept
    {
        close();
    }

    raw_socket::raw_socket(raw_socket &&other) noexcept
        : fd_{std::exchange(other.fd_, -1)}, proto_{other.proto_}, bound_interface_{std::move(other.bound_interface_)}
    {
    }

    auto raw_socket::operator=(raw_socket &&other) noexcept -> raw_socket &
    {
        if (this != &other)
        {
            close();
            fd_ = std::exchange(other.fd_, -1);
            proto_ = other.proto_;
            bound_interface_ = std::move(other.bound_interface_);
        }
        return *this;
    }

    auto raw_socket::create(protocol const proto) noexcept -> result<raw_socket>
    {
        auto const fd = ::socket(AF_PACKET, SOCK_RAW, htons(static_cast<std::uint16_t>(proto)));

        if (fd < 0)
        {
            if (errno == EPERM || errno == EACCES)
            {
                return std::unexpected{error_code::permission_denied};
            }
            return std::unexpected{error_code::socket_creation_failed};
        }

        return raw_socket{fd, proto};
    }

    auto raw_socket::create_bound(
        interface_info const &iface,
        protocol const proto) noexcept -> result<raw_socket>
    {
        auto sock_result = create(proto);
        if (!sock_result.has_value())
        {
            return sock_result;
        }

        auto bind_result = sock_result->bind(iface);
        if (!bind_result.has_value())
        {
            return std::unexpected{bind_result.error()};
        }

        return sock_result;
    }

    auto raw_socket::bind(interface_info const &iface) noexcept -> void_result
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct sockaddr_ll sll{};
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(static_cast<std::uint16_t>(proto_));
        sll.sll_ifindex = iface.index();

        if (::bind(fd_, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0)
        {
            return std::unexpected{error_code::socket_bind_failed};
        }

        bound_interface_ = iface;
        return {};
    }

    auto raw_socket::set_options(socket_options const &opts) noexcept -> void_result
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        if (opts.recv_timeout.has_value())
        {
            struct timeval tv{};
            auto const ms = opts.recv_timeout->count();
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        if (opts.send_timeout.has_value())
        {
            struct timeval tv{};
            auto const ms = opts.send_timeout->count();
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            if (::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        if (opts.reuse_addr)
        {
            int const opt = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        if (opts.broadcast)
        {
            int const opt = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        if (opts.recv_buffer_size.has_value())
        {
            int const size = *opts.recv_buffer_size;
            if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        if (opts.send_buffer_size.has_value())
        {
            int const size = *opts.send_buffer_size;
            if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
            {
                return std::unexpected{error_code::socket_bind_failed};
            }
        }

        return {};
    }

    auto raw_socket::send_to(
        std::span<std::uint8_t const> data,
        interface_info const &iface,
        mac_address const &dest_mac) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct sockaddr_ll addr{};
        addr.sll_family = AF_PACKET;
        addr.sll_ifindex = iface.index();
        addr.sll_halen = ETH_ALEN;
        std::copy_n(dest_mac.data(), ETH_ALEN, addr.sll_addr);

        auto const sent = ::sendto(fd_, data.data(), data.size(), 0,
                                   reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));

        if (sent < 0)
        {
            return std::unexpected{error_code::socket_send_failed};
        }

        return static_cast<std::size_t>(sent);
    }

    auto raw_socket::send_raw(
        std::span<std::uint8_t const> data,
        interface_info const &iface) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct sockaddr_ll addr{};
        addr.sll_family = AF_PACKET;
        addr.sll_ifindex = iface.index();
        addr.sll_halen = ETH_ALEN;
        // dest mac is in the frame already

        auto const sent = ::sendto(fd_, data.data(), data.size(), 0,
                                   reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));

        if (sent < 0)
        {
            return std::unexpected{error_code::socket_send_failed};
        }

        return static_cast<std::size_t>(sent);
    }

    auto raw_socket::receive(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        auto const received = ::recv(fd_, buffer.data(), buffer.size(), 0);

        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return std::unexpected{error_code::timeout};
            }
            return std::unexpected{error_code::socket_recv_failed};
        }

        return static_cast<std::size_t>(received);
    }

    auto raw_socket::receive_with_timeout(
        std::span<std::uint8_t> buffer,
        std::chrono::milliseconds timeout) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        auto const ret = ::poll(&pfd, 1, static_cast<int>(timeout.count()));

        if (ret < 0)
        {
            return std::unexpected{error_code::socket_recv_failed};
        }
        if (ret == 0)
        {
            return std::unexpected{error_code::timeout};
        }

        return receive(buffer);
    }

    auto raw_socket::close() noexcept -> void
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        bound_interface_.reset();
    }

    // ============================================================================
    // tcp_socket implementation
    // ============================================================================

    tcp_socket::tcp_socket(int const fd) noexcept : fd_{fd} {}

    tcp_socket::~tcp_socket() noexcept
    {
        close();
    }

    tcp_socket::tcp_socket(tcp_socket &&other) noexcept
        : fd_{std::exchange(other.fd_, -1)}
    {
    }

    auto tcp_socket::operator=(tcp_socket &&other) noexcept -> tcp_socket &
    {
        if (this != &other)
        {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    auto tcp_socket::create_server(std::uint16_t const port) noexcept -> result<tcp_socket>
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        int const opt = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        {
            ::close(fd);
            return std::unexpected{error_code::socket_bind_failed};
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return std::unexpected{error_code::socket_bind_failed};
        }

        if (::listen(fd, 1) < 0)
        {
            ::close(fd);
            return std::unexpected{error_code::socket_bind_failed};
        }

        return tcp_socket{fd};
    }

    auto tcp_socket::accept() noexcept -> result<tcp_socket>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        auto const client_fd = ::accept(fd_, nullptr, nullptr);
        if (client_fd < 0)
        {
            return std::unexpected{error_code::connection_failed};
        }

        return tcp_socket{client_fd};
    }

    auto tcp_socket::connect(
        std::string_view const ip,
        std::uint16_t const port,
        std::chrono::seconds const timeout) noexcept -> result<tcp_socket>
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // need null-terminated string for inet_pton
        std::string ip_str{ip};
        if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1)
        {
            ::close(fd);
            return std::unexpected{error_code::connection_failed};
        }

        auto const deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
            {
                return tcp_socket{fd};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        ::close(fd);
        return std::unexpected{error_code::connection_failed};
    }

    auto tcp_socket::send(std::span<std::uint8_t const> data) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        auto const sent = ::send(fd_, data.data(), data.size(), 0);
        if (sent < 0)
        {
            return std::unexpected{error_code::socket_send_failed};
        }

        return static_cast<std::size_t>(sent);
    }

    auto tcp_socket::receive(std::span<std::uint8_t> buffer) noexcept -> result<std::size_t>
    {
        if (!is_valid())
        {
            return std::unexpected{error_code::socket_creation_failed};
        }

        auto const received = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (received < 0)
        {
            return std::unexpected{error_code::socket_recv_failed};
        }

        return static_cast<std::size_t>(received);
    }

    auto tcp_socket::close() noexcept -> void
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

} // namespace l2net

#pragma GCC diagnostic pop