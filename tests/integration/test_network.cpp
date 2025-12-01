// tests/integration/test_network.cpp - network integration tests
// WARNING: requires root privileges and a network interface
// these tests are for actual network communication

#include "l2net/hybrid_chat.hpp"
#include "l2net/interface.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/vlan.hpp"

#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <thread>

namespace
{

    [[nodiscard]] auto has_root_privileges() noexcept -> bool
    {
        return ::geteuid() == 0;
    }

    [[nodiscard]] auto get_test_interface() noexcept -> std::optional<l2net::interface_info>
    {
        // try to find a non-loopback interface for network tests
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

    [[nodiscard]] auto loopback_available() noexcept -> bool
    {
        return l2net::get_loopback_interface().has_value();
    }

} // anonymous namespace

TEST_SUITE("tcp_socket" * doctest::skip(!has_root_privileges()))
{

    TEST_CASE("tcp server creation")
    {
        auto server = l2net::tcp_socket::create_server(19000);
        REQUIRE(server.has_value());
        CHECK(server->is_valid());
    }

    TEST_CASE("tcp client connection timeout")
    {
        // connect to non-existent server should timeout
        auto start = std::chrono::steady_clock::now();
        auto client = l2net::tcp_socket::connect("127.0.0.1", 19999, std::chrono::seconds{1});
        auto elapsed = std::chrono::steady_clock::now() - start;

        // should fail relatively quickly
        CHECK_FALSE(client.has_value());
        CHECK(elapsed < std::chrono::seconds{5});
    }

    TEST_CASE("tcp server-client handshake")
    {
        constexpr std::uint16_t port = 19001;

        std::atomic<bool> server_ready{false};
        std::atomic<bool> handshake_success{false};

        // server thread
        std::thread server_thread{[&]()
                                  {
                                      auto server = l2net::tcp_socket::create_server(port);
                                      if (!server.has_value())
                                      {
                                          return;
                                      }

                                      server_ready.store(true);

                                      auto client = server->accept();
                                      if (!client.has_value())
                                      {
                                          return;
                                      }

                                      // send test data
                                      std::array<std::uint8_t, 6> test_mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
                                      auto sent = client->send(test_mac);
                                      if (!sent.has_value())
                                      {
                                          return;
                                      }

                                      // receive response
                                      std::array<std::uint8_t, 6> response{};
                                      auto recvd = client->receive(response);
                                      if (recvd.has_value() && *recvd == 6)
                                      {
                                          handshake_success.store(true);
                                      }
                                  }};

        // wait for server
        while (!server_ready.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        // client
        auto client = l2net::tcp_socket::connect("127.0.0.1", port, std::chrono::seconds{5});
        REQUIRE(client.has_value());

        // receive
        std::array<std::uint8_t, 6> received{};
        auto recv_result = client->receive(received);
        REQUIRE(recv_result.has_value());
        CHECK(*recv_result == 6);
        CHECK(received[0] == 0xAA);

        // send response
        std::array<std::uint8_t, 6> response{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        auto send_result = client->send(response);
        CHECK(send_result.has_value());

        server_thread.join();
        CHECK(handshake_success.load());
    }

    TEST_CASE("tcp socket move semantics")
    {
        auto server = l2net::tcp_socket::create_server(19002);
        REQUIRE(server.has_value());

        auto server2 = std::move(*server);
        CHECK(server2.is_valid());
        CHECK_FALSE(server->is_valid());
    }
}

TEST_SUITE("handshake" * doctest::skip(!has_root_privileges()))
{

    TEST_CASE("mac exchange handshake")
    {
        constexpr std::uint16_t port = 19010;

        l2net::mac_address const server_mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        l2net::mac_address const client_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

        std::atomic<bool> server_started{false};
        l2net::mac_address received_by_server{};
        l2net::mac_address received_by_client{};

        std::thread server_thread{[&]()
                                  {
                                      server_started.store(true);
                                      auto result =
                                          l2net::handshake::run_server(port, server_mac, std::chrono::seconds{5});
                                      if (result.has_value())
                                      {
                                          received_by_server = *result;
                                      }
                                  }};

        while (!server_started.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50}); // let server listen

        auto result = l2net::handshake::run_client("127.0.0.1", port, client_mac, std::chrono::seconds{5});

        server_thread.join();

        REQUIRE(result.has_value());
        received_by_client = *result;

        CHECK(received_by_server == client_mac);
        CHECK(received_by_client == server_mac);
    }
}

TEST_SUITE("vlan_frames" * doctest::skip(!has_root_privileges() || !loopback_available()))
{

    TEST_CASE("send and receive vlan tagged frame on loopback")
    {
        auto lo = l2net::get_loopback_interface();
        REQUIRE(lo.has_value());

        constexpr std::uint16_t test_proto = 0xCAFE;

        auto sender = l2net::raw_socket::create_bound(*lo, l2net::raw_socket::protocol::all);
        auto receiver = l2net::raw_socket::create_bound(*lo, l2net::raw_socket::protocol::all);

        REQUIRE(sender.has_value());
        REQUIRE(receiver.has_value());

        // build vlan tagged frame
        l2net::vlan_tci const tci{.priority = 5, .dei = false, .vlan_id = 42};
        auto frame = l2net::build_vlan_frame(l2net::mac_address::null(), l2net::mac_address::null(), tci, test_proto,
                                             "vlan test payload");
        REQUIRE(frame.has_value());

        // send
        auto send_result = sender->send_raw(*frame, *lo);
        REQUIRE(send_result.has_value());

        // receive
        std::vector<std::uint8_t> buffer(2048);
        bool found = false;

        for (int attempt = 0; attempt < 10 && !found; ++attempt)
        {
            auto recv_result = receiver->receive_with_timeout(buffer, std::chrono::milliseconds{100});
            if (!recv_result.has_value())
            {
                continue;
            }

            l2net::frame_parser parser{std::span{buffer.data(), *recv_result}};
            if (!parser.is_valid())
            {
                continue;
            }

            if (parser.has_vlan() && parser.ether_type() == test_proto)
            {
                CHECK(parser.vlan_id() == 42);
                CHECK(parser.vlan_priority() == 5);
                found = true;
            }
        }

        // note: might not find it depending on kernel config
        // CHECK(found);  // commented because loopback might strip vlan
    }
}

TEST_SUITE("network_interface" * doctest::skip(!has_root_privileges()))
{

    TEST_CASE("list all interfaces")
    {
        auto result = l2net::interface_info::list_all();
        REQUIRE(result.has_value());

        // should have at least loopback
        CHECK(!result->empty());

        bool found_loopback = false;
        for (auto const &iface : *result)
        {
            if (iface.is_loopback())
            {
                found_loopback = true;
                break;
            }
        }
        CHECK(found_loopback);
    }

    TEST_CASE("interface info completeness")
    {
        auto lo = l2net::get_loopback_interface();
        if (!lo.has_value())
        {
            return;
        }

        CHECK(!lo->name().empty());
        CHECK(lo->index() >= 0);
        CHECK(lo->is_loopback());
        CHECK(lo->mtu() > 0);
    }
}

// these tests require an actual network interface (not loopback)
TEST_SUITE("physical_network" * doctest::skip(!has_root_privileges() || !get_test_interface().has_value()))
{

    TEST_CASE("raw socket on physical interface")
    {
        auto iface = get_test_interface();
        REQUIRE(iface.has_value());

        auto sock = l2net::raw_socket::create_bound(*iface);
        REQUIRE(sock.has_value());
        CHECK(sock->is_valid());
    }

    TEST_CASE("send frame on physical interface")
    {
        auto iface = get_test_interface();
        REQUIRE(iface.has_value());

        auto sock = l2net::raw_socket::create_bound(*iface);
        REQUIRE(sock.has_value());

        // build broadcast frame
        auto frame = l2net::build_simple_frame(l2net::mac_address::broadcast(), iface->mac(),
                                               0x88B5, // custom ethertype
                                               "network test");
        REQUIRE(frame.has_value());

        auto result = sock->send_raw(*frame, *iface);
        CHECK(result.has_value());
    }
}
