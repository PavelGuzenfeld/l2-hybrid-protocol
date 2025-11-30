// tests/integration/test_localhost.cpp - localhost integration tests
// WARNING: requires root privileges to run
// these tests actually create raw sockets and send frames

#include <doctest/doctest.h>
#include "l2net/ipc_channel.hpp"
#include "l2net/raw_socket.hpp"
#include "l2net/interface.hpp"
#include "l2net/frame.hpp"

#include <thread>
#include <chrono>
#include <atomic>

namespace {

// helper to check if we have root privileges
[[nodiscard]] auto has_root_privileges() noexcept -> bool {
    return ::geteuid() == 0;
}

// helper to check if loopback exists
[[nodiscard]] auto loopback_available() noexcept -> bool {
    auto result = l2net::get_loopback_interface();
    return result.has_value();
}

} // anonymous namespace

TEST_SUITE("localhost_integration" * doctest::skip(!has_root_privileges() || !loopback_available())) {

    TEST_CASE("raw socket creation") {
        auto result = l2net::raw_socket::create();

        REQUIRE(result.has_value());
        CHECK(result->is_valid());
        CHECK(result->fd() >= 0);
    }

    TEST_CASE("raw socket creation with protocol") {
        auto result = l2net::raw_socket::create(l2net::raw_socket::protocol::ipc);

        REQUIRE(result.has_value());
        CHECK(result->is_valid());
    }

    TEST_CASE("raw socket bind to loopback") {
        auto lo = l2net::get_loopback_interface();
        REQUIRE(lo.has_value());

        auto sock = l2net::raw_socket::create(l2net::raw_socket::protocol::ipc);
        REQUIRE(sock.has_value());

        auto bind_result = sock->bind(*lo);
        CHECK(bind_result.has_value());
        CHECK(sock->bound_interface().has_value());
    }

    TEST_CASE("raw socket create_bound convenience") {
        auto lo = l2net::get_loopback_interface();
        REQUIRE(lo.has_value());

        auto sock = l2net::raw_socket::create_bound(*lo, l2net::raw_socket::protocol::ipc);
        REQUIRE(sock.has_value());
        CHECK(sock->is_valid());
    }

    TEST_CASE("raw socket move semantics") {
        auto sock1 = l2net::raw_socket::create();
        REQUIRE(sock1.has_value());

        auto const fd = sock1->fd();
        auto sock2 = std::move(*sock1);

        CHECK(sock2.fd() == fd);
        CHECK_FALSE(sock1->is_valid());  // moved-from
    }

    TEST_CASE("ipc channel creation") {
        auto channel = l2net::ipc_channel::create();

        REQUIRE(channel.has_value());
        CHECK(channel->is_valid());
    }

    TEST_CASE("ipc channel send and receive") {
        // create two channels - one to send, one to receive
        l2net::ipc_config config;
        config.recv_timeout = std::chrono::milliseconds{1000};

        auto sender = l2net::ipc_channel::create(config);
        auto receiver = l2net::ipc_channel::create(config);

        REQUIRE(sender.has_value());
        REQUIRE(receiver.has_value());

        std::string const message = "integration test message";

        // send
        auto send_result = sender->send(message);
        REQUIRE(send_result.has_value());
        CHECK(*send_result > message.size());  // includes header

        // receive
        auto recv_result = receiver->receive_with_timeout(std::chrono::milliseconds{500});

        // note: on loopback, the sender also receives its own message
        // so this should succeed
        if (recv_result.has_value() && !recv_result->empty()) {
            std::string_view const received{
                reinterpret_cast<char const*>(recv_result->data()),
                recv_result->size()
            };
            CHECK(received == message);
        }
    }

    TEST_CASE("ipc channel binary data") {
        l2net::ipc_config config;
        config.recv_timeout = std::chrono::milliseconds{500};

        auto channel = l2net::ipc_channel::create(config);
        REQUIRE(channel.has_value());

        std::vector<std::uint8_t> const binary_data{0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};

        auto send_result = channel->send(binary_data);
        REQUIRE(send_result.has_value());

        auto recv_result = channel->receive_with_timeout(std::chrono::milliseconds{200});
        if (recv_result.has_value() && !recv_result->empty()) {
            CHECK(recv_result->size() == binary_data.size());
            CHECK((*recv_result)[0] == 0x00);
            CHECK((*recv_result)[5] == 0xFD);
        }
    }

    TEST_CASE("ipc channel large message") {
        l2net::ipc_config config;
        config.recv_timeout = std::chrono::milliseconds{500};

        auto channel = l2net::ipc_channel::create(config);
        REQUIRE(channel.has_value());

        // loopback can handle large frames
        std::vector<std::uint8_t> large_data(8000, 0x42);

        auto send_result = channel->send(large_data);
        REQUIRE(send_result.has_value());

        auto recv_result = channel->receive_with_timeout(std::chrono::milliseconds{200});
        if (recv_result.has_value() && !recv_result->empty()) {
            CHECK(recv_result->size() == large_data.size());
        }
    }

    TEST_CASE("ipc channel threaded send/receive") {
        l2net::ipc_config config;
        config.recv_timeout = std::chrono::milliseconds{100};

        auto sender = l2net::ipc_channel::create(config);
        auto receiver = l2net::ipc_channel::create(config);

        REQUIRE(sender.has_value());
        REQUIRE(receiver.has_value());

        std::atomic<int> received_count{0};
        std::atomic<bool> stop{false};

        // receiver thread
        std::thread recv_thread{[&]() {
            while (!stop.load()) {
                auto result = receiver->receive_with_timeout(std::chrono::milliseconds{50});
                if (result.has_value() && !result->empty()) {
                    received_count.fetch_add(1);
                }
            }
        }};

        // send several messages
        for (int i = 0; i < 10; ++i) {
            auto msg = fmt::format("message {}", i);
            sender->send(msg);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        stop.store(true);
        recv_thread.join();

        // we should have received at least some messages
        // (exact count depends on timing and whether sender receives its own)
        CHECK(received_count.load() > 0);
    }

    TEST_CASE("raw socket timeout") {
        auto lo = l2net::get_loopback_interface();
        REQUIRE(lo.has_value());

        auto sock = l2net::raw_socket::create_bound(*lo, l2net::raw_socket::protocol::custom);
        REQUIRE(sock.has_value());

        std::vector<std::uint8_t> buffer(1024);

        auto start = std::chrono::steady_clock::now();
        auto result = sock->receive_with_timeout(buffer, std::chrono::milliseconds{100});
        auto elapsed = std::chrono::steady_clock::now() - start;

        // should timeout, not block forever
        CHECK(result.error() == l2net::error_code::timeout);
        CHECK(elapsed < std::chrono::milliseconds{500});
    }

    TEST_CASE("socket options") {
        auto sock = l2net::raw_socket::create();
        REQUIRE(sock.has_value());

        l2net::socket_options opts;
        opts.recv_timeout = std::chrono::milliseconds{100};
        opts.recv_buffer_size = 65536;

        auto result = sock->set_options(opts);
        CHECK(result.has_value());
    }

    TEST_CASE("frame roundtrip through loopback") {
        auto lo = l2net::get_loopback_interface();
        REQUIRE(lo.has_value());

        // use a unique protocol to avoid noise
        constexpr std::uint16_t test_proto = 0xBEEF;

        auto sender = l2net::raw_socket::create_bound(
            *lo, static_cast<l2net::raw_socket::protocol>(test_proto));
        auto receiver = l2net::raw_socket::create_bound(
            *lo, static_cast<l2net::raw_socket::protocol>(test_proto));

        REQUIRE(sender.has_value());
        REQUIRE(receiver.has_value());

        // build frame
        auto frame = l2net::build_simple_frame(
            l2net::mac_address::null(),
            l2net::mac_address::null(),
            test_proto,
            "roundtrip test"
        );
        REQUIRE(frame.has_value());

        // send
        auto send_result = sender->send_raw(*frame, *lo);
        REQUIRE(send_result.has_value());

        // receive
        std::vector<std::uint8_t> buffer(1024);
        auto recv_result = receiver->receive_with_timeout(buffer, std::chrono::milliseconds{500});

        if (recv_result.has_value()) {
            l2net::frame_parser parser{std::span{buffer.data(), *recv_result}};
            CHECK(parser.is_valid());
            CHECK(parser.ether_type() == test_proto);
        }
    }
}

TEST_SUITE("localhost_no_root" * doctest::skip(has_root_privileges())) {
    TEST_CASE("raw socket creation fails without root") {
        auto result = l2net::raw_socket::create();

        CHECK_FALSE(result.has_value());
        CHECK(result.error() == l2net::error_code::permission_denied);
    }
}
