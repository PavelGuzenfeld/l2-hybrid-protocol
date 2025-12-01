// bench/bench_network.cpp - network benchmarks
// L2 raw socket vs UDP over physical network interface

#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp"

#include <arpa/inet.h>
#include <fmt/format.h>
#include <nanobench.h>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace bench
{

    namespace
    {

        [[nodiscard]] auto can_run_network_benchmarks() -> bool
        {
            if (::geteuid() != 0)
            {
                return false;
            }
            auto all = l2net::interface_info::list_all();
            if (!all.has_value())
            {
                return false;
            }
            for (auto const &iface : *all)
            {
                if (!iface.is_loopback() && iface.is_up() && !iface.mac().is_null())
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] auto get_network_interface() -> std::optional<l2net::interface_info>
        {
            auto all = l2net::interface_info::list_all();
            if (!all.has_value())
            {
                return std::nullopt;
            }
            for (auto const &iface : *all)
            {
                if (!iface.is_loopback() && iface.is_up() && !iface.mac().is_null())
                {
                    return iface;
                }
            }
            return std::nullopt;
        }

        class udp_broadcast_socket
        {
            int fd_{-1};

        public:
            udp_broadcast_socket() : fd_{::socket(AF_INET, SOCK_DGRAM, 0)}
            {
                if (fd_ >= 0)
                {
                    int opt = 1;
                    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
                }
            }
            ~udp_broadcast_socket()
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                }
            }

            [[nodiscard]] auto send_broadcast(void const *data, std::size_t len, std::uint16_t port) -> ssize_t
            {
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                addr.sin_port = htons(port);
                return ::sendto(fd_, data, len, 0, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
            }
            [[nodiscard]] auto is_valid() const -> bool
            {
                return fd_ >= 0;
            }
        };

    } // anonymous namespace

    void run_network_benchmarks()
    {
        using namespace ankerl::nanobench;

        if (!can_run_network_benchmarks())
        {
            fmt::print(stderr, "Skipping network benchmarks (requires root and physical interface)\n");
            return;
        }

        auto iface = get_network_interface();
        if (!iface.has_value())
        {
            return;
        }

        auto l2_sock = l2net::raw_socket::create_bound(*iface);
        if (!l2_sock.has_value())
        {
            return;
        }

        // L2 Send Benchmarks
        {
            std::vector<std::uint8_t> payload(50, 0x42);
            auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(),
                                                   l2net::constants::eth_p_custom, payload);
            if (frame.has_value())
            {
                Bench()
                    .batch(frame->size())
                    .run("L2_Network_Send_Small", [&] { (void)l2_sock->send_raw(*frame, *iface); });
            }
        }

        {
            std::vector<std::uint8_t> payload(1400, 0x42);
            auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(),
                                                   l2net::constants::eth_p_custom, payload);
            if (frame.has_value())
            {
                Bench()
                    .batch(frame->size())
                    .run("L2_Network_Send_Large", [&] { (void)l2_sock->send_raw(*frame, *iface); });
            }
        }

        // UDP Benchmarks
        {
            udp_broadcast_socket udp_sock;
            if (udp_sock.is_valid())
            {
                std::vector<std::uint8_t> payload(50, 0x42);
                Bench()
                    .batch(payload.size())
                    .run("UDP_Network_Broadcast_Small",
                         [&] { (void)udp_sock.send_broadcast(payload.data(), payload.size(), 19997); });

                payload.resize(1400);
                Bench()
                    .batch(payload.size())
                    .run("UDP_Network_Broadcast_Large",
                         [&] { (void)udp_sock.send_broadcast(payload.data(), payload.size(), 19997); });
            }
        }

        // Socket Creation
        if (::geteuid() == 0)
        {
            Bench().run("L2_Socket_Create",
                        [&]
                        {
                            auto sock = l2net::raw_socket::create();
                            doNotOptimizeAway(sock);
                        });
        }

        Bench().run("UDP_Socket_Create",
                    [&]
                    {
                        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
                        doNotOptimizeAway(fd);
                        if (fd >= 0)
                        {
                            ::close(fd);
                        }
                    });

        // Interface Queries
        Bench().run("Interface_Query",
                    [&]
                    {
                        auto result = l2net::interface_info::query("lo");
                        doNotOptimizeAway(result);
                    });

        Bench().run("Interface_ListAll",
                    [&]
                    {
                        auto result = l2net::interface_info::list_all();
                        doNotOptimizeAway(result);
                    });

        // Payload Size Sweep
        std::vector<std::size_t> sizes = {32, 64, 128, 256, 512, 1024, 1400};
        for (auto size : sizes)
        {
            std::vector<std::uint8_t> payload(size, 0x42);
            auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(),
                                                   l2net::constants::eth_p_custom, payload);

            if (frame.has_value())
            {
                Bench()
                    .batch(frame->size())
                    .run(fmt::format("L2_Payload_{}", size), [&] { (void)l2_sock->send_raw(*frame, *iface); });
            }
        }
    }

} // namespace bench
