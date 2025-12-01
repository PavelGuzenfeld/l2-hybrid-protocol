// bench/bench_localhost.cpp

#include "l2net/frame.hpp"
#include "l2net/interface.hpp"
#include "l2net/ipc_channel.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fmt/format.h>
#include <nanobench.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace bench
{

    namespace
    {

        [[nodiscard]] auto can_run_raw_benchmarks() -> bool
        {
            if (::geteuid() != 0)
            {
                return false;
            }
            auto lo = l2net::get_loopback_interface();
            return lo.has_value();
        }

        class udp_socket
        {
            int fd_{-1};

        public:
            udp_socket() : fd_{::socket(AF_INET, SOCK_DGRAM, 0)}
            {
            }
            ~udp_socket()
            {
                if (fd_ >= 0)
                {
                    ::close(fd_);
                }
            }

            [[nodiscard]] auto bind(std::uint16_t port) -> bool
            {
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port = htons(port);
                return ::bind(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0;
            }

            [[nodiscard]] auto send_to(void const *data, std::size_t len, std::uint16_t port) -> ssize_t
            {
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(port);
                return ::sendto(fd_, data, len, 0, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
            }

            [[nodiscard]] auto recv(void *data, std::size_t len) -> ssize_t
            {
                return ::recv(fd_, data, len, 0);
            }

            [[nodiscard]] auto is_valid() const -> bool
            {
                return fd_ >= 0;
            }
            [[nodiscard]] auto fd() const -> int
            {
                return fd_;
            }
        };

    } // namespace

    void run_localhost_benchmarks()
    {
        using namespace ankerl::nanobench;

        Bench().run("FrameBuild_Small",
                    [&]
                    {
                        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
                        std::array<std::uint8_t, 50> payload{};
                        auto frame = l2net::build_simple_frame(dest, src, 0x88B5, payload);
                        doNotOptimizeAway(frame);
                    });

        Bench().run("FrameBuild_Large",
                    [&]
                    {
                        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
                        std::vector<std::uint8_t> payload(1400, 0x42);
                        auto frame = l2net::build_simple_frame(dest, src, 0x88B5, payload);
                        doNotOptimizeAway(frame);
                    });

        Bench().run("FrameBuild_IntoBuffer",
                    [&]
                    {
                        l2net::mac_address const dest{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        l2net::mac_address const src{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
                        std::vector<std::uint8_t> payload(1400, 0x42);
                        std::vector<std::uint8_t> buffer(1500);
                        auto builder =
                            l2net::frame_builder{}.set_dest(dest).set_src(src).set_ether_type(0x88B5).set_payload(
                                payload);
                        auto result = builder.build_into(buffer);
                        doNotOptimizeAway(result);
                    });

        // state setup for parsing benchmarks
        std::vector<std::uint8_t> large_frame(1414, 0x42); // approximated size
        l2net::vlan_tci const tci{.priority = 7, .dei = false, .vlan_id = 10};
        auto tagged_frame = l2net::build_vlan_frame(l2net::mac_address::null(), l2net::mac_address::null(), tci, 0x88B5,
                                                    std::vector<std::uint8_t>(1386, 0x42));

        Bench().run("FrameParse_Untagged",
                    [&]
                    {
                        l2net::frame_parser parser{large_frame};
                        doNotOptimizeAway(parser.is_valid());
                    });

        if (tagged_frame.has_value())
        {
            Bench().run("FrameParse_Tagged",
                        [&]
                        {
                            l2net::frame_parser parser{*tagged_frame};
                            doNotOptimizeAway(parser.is_valid());
                        });
        }

        // Network IO benchmarks
        if (can_run_raw_benchmarks())
        {
            auto channel = l2net::ipc_channel::create();
            if (channel.has_value())
            {
                std::array<std::uint8_t, 50> small_payload{};
                Bench().batch(small_payload.size()).run("L2_Send_Small", [&] { (void)channel->send(small_payload); });

                std::vector<std::uint8_t> large_payload(1400, 0x42);
                Bench().batch(large_payload.size()).run("L2_Send_Large", [&] { (void)channel->send(large_payload); });

                std::vector<std::uint8_t> jumbo_payload(8000, 0x42);
                Bench().batch(jumbo_payload.size()).run("L2_Send_Jumbo", [&] { (void)channel->send(jumbo_payload); });
            }
        }
        else
        {
            fmt::print(stderr, "Skipping L2 Send benchmarks (requires root/loopback)\n");
        }

        // UDP Comparisons
        {
            udp_socket sock;
            if (sock.is_valid())
            {
                std::array<std::uint8_t, 50> small_payload{};
                Bench()
                    .batch(small_payload.size())
                    .run("UDP_Send_Small",
                         [&] { (void)sock.send_to(small_payload.data(), small_payload.size(), 19999); });

                std::vector<std::uint8_t> large_payload(1400, 0x42);
                Bench()
                    .batch(large_payload.size())
                    .run("UDP_Send_Large",
                         [&] { (void)sock.send_to(large_payload.data(), large_payload.size(), 19999); });

                std::vector<std::uint8_t> jumbo_payload(8000, 0x42);
                Bench()
                    .batch(jumbo_payload.size())
                    .run("UDP_Send_Jumbo",
                         [&] { (void)sock.send_to(jumbo_payload.data(), jumbo_payload.size(), 19999); });
            }
        }

        // Latency
        if (can_run_raw_benchmarks())
        {
            l2net::ipc_config config;
            config.recv_timeout = std::chrono::milliseconds{100};
            auto channel = l2net::ipc_channel::create(config);

            if (channel.has_value())
            {
                std::array<std::uint8_t, 64> payload{};
                Bench().run("L2_Roundtrip_Latency",
                            [&]
                            {
                                (void)channel->send(payload);
                                auto result = channel->receive_with_timeout(std::chrono::milliseconds{10});
                                doNotOptimizeAway(result);
                            });
            }
        }

        // Utility benchmarks
        Bench().run("MacAddress_FromString",
                    [&]
                    {
                        auto result = l2net::mac_address::from_string("aa:bb:cc:dd:ee:ff");
                        doNotOptimizeAway(result);
                    });

        l2net::mac_address const mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        Bench().run("MacAddress_ToString",
                    [&]
                    {
                        auto result = mac.to_string();
                        doNotOptimizeAway(result);
                    });

        l2net::vlan_tci const vlan_tci_val{.priority = 7, .dei = false, .vlan_id = 100};
        Bench().run("VlanTci_Encode",
                    [&]
                    {
                        auto result = vlan_tci_val.encode();
                        doNotOptimizeAway(result);
                    });

        std::uint16_t const encoded_vlan = 0xE064;
        Bench().run("VlanTci_Decode",
                    [&]
                    {
                        auto result = l2net::vlan_tci::decode(encoded_vlan);
                        doNotOptimizeAway(result);
                    });
    }

} // namespace bench
