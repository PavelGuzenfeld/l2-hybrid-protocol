// ssh_session.hpp - SSH wrapper for remote benchmark deployment
// because testing on localhost is basically cheating

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// forward declare libssh types so we don't pollute everyone's namespace
// with that C garbage
struct ssh_session_struct;
struct ssh_channel_struct;
struct ssh_scp_struct;
struct sftp_session_struct;

namespace l2net::ssh
{

    // =============================================================================
    // error handling - because ssh has 47 ways to fail
    // =============================================================================

    enum class error_code : std::uint8_t
    {
        success = 0,
        connection_failed,
        authentication_failed,
        channel_open_failed,
        command_failed,
        scp_init_failed,
        scp_push_failed,
        scp_read_failed,
        file_not_found,
        permission_denied,
        timeout,
        host_key_failed,
        session_invalid,
        sftp_init_failed,
        unknown_error
    };

    [[nodiscard]] constexpr auto to_string(error_code const ec) noexcept -> std::string_view
    {
        switch (ec)
        {
        case error_code::success:
            return "success";
        case error_code::connection_failed:
            return "connection failed";
        case error_code::authentication_failed:
            return "authentication failed";
        case error_code::channel_open_failed:
            return "channel open failed";
        case error_code::command_failed:
            return "command execution failed";
        case error_code::scp_init_failed:
            return "scp initialization failed";
        case error_code::scp_push_failed:
            return "scp push failed";
        case error_code::scp_read_failed:
            return "scp read failed";
        case error_code::file_not_found:
            return "file not found";
        case error_code::permission_denied:
            return "permission denied";
        case error_code::timeout:
            return "operation timeout";
        case error_code::host_key_failed:
            return "host key verification failed";
        case error_code::session_invalid:
            return "session invalid or not connected";
        case error_code::sftp_init_failed:
            return "sftp initialization failed";
        case error_code::unknown_error:
            return "unknown error";
        }
        return "unknown error";
    }

    template <typename T>
    using result = std::expected<T, error_code>;

    // =============================================================================
    // command result - stdout, stderr, and exit code
    // =============================================================================

    struct command_result
    {
        std::string stdout_output{};
        std::string stderr_output{};
        int exit_code{-1};

        [[nodiscard]] constexpr auto success() const noexcept -> bool
        {
            return exit_code == 0;
        }
    };

    // =============================================================================
    // ssh session configuration
    // =============================================================================

    struct session_config
    {
        std::string host{};
        std::uint16_t port{22};
        std::string username{};
        std::string password{};               // password auth
        std::string private_key_path{};       // key-based auth (optional)
        std::string private_key_passphrase{}; // passphrase for key (optional)
        std::chrono::seconds connect_timeout{10};
        std::chrono::seconds command_timeout{60};
        bool strict_host_key_checking{false}; // disable for testing, enable in prod
        int verbosity{0};                     // 0-4, higher = more spam
    };

    // =============================================================================
    // ssh session - the actual workhorse
    // =============================================================================

    class session
    {
    public:
        // rule of 5 because we manage raw ssh handles
        session() = default;
        ~session();

        session(session const &) = delete;
        auto operator=(session const &) -> session & = delete;

        session(session &&other) noexcept;
        auto operator=(session &&other) noexcept -> session &;

        // factory method - because constructors can't return errors elegantly
        [[nodiscard]] static auto connect(session_config const &config) -> result<session>;

        // connection management
        [[nodiscard]] auto is_connected() const noexcept -> bool;
        auto disconnect() -> void;

        // command execution
        [[nodiscard]] auto execute(std::string_view command) -> result<command_result>;
        [[nodiscard]] auto execute(std::string_view command, std::chrono::seconds timeout) -> result<command_result>;

        // execute with real-time output streaming
        [[nodiscard]] auto
        execute_streaming(std::string_view command,
                          std::function<void(std::string_view, bool)> const &output_callback // (data, is_stderr)
                          ) -> result<int>;                                                  // returns exit code

        // file transfer
        [[nodiscard]] auto upload_file(std::filesystem::path const &local_path, std::string_view remote_path,
                                       int mode = 0755) -> result<void>;

        [[nodiscard]] auto download_file(std::string_view remote_path, std::filesystem::path const &local_path)
            -> result<void>;

        [[nodiscard]] auto upload_data(std::span<std::uint8_t const> data, std::string_view remote_path,
                                       int mode = 0755) -> result<void>;

        // convenience methods
        [[nodiscard]] auto file_exists(std::string_view remote_path) -> result<bool>;
        [[nodiscard]] auto make_directory(std::string_view remote_path) -> result<void>;
        [[nodiscard]] auto remove_file(std::string_view remote_path) -> result<void>;
        [[nodiscard]] auto get_remote_arch() -> result<std::string>;

        // get last error message from libssh
        [[nodiscard]] auto last_error_message() const -> std::string;

        // accessors
        [[nodiscard]] auto config() const noexcept -> session_config const &
        {
            return config_;
        }

    private:
        explicit session(ssh_session_struct *raw_session, session_config config);

        [[nodiscard]] auto open_channel() -> result<ssh_channel_struct *>;
        auto close_channel(ssh_channel_struct *channel) -> void;

        ssh_session_struct *session_{nullptr};
        session_config config_{};
    };

    // =============================================================================
    // ssh session pool - for when you need multiple concurrent operations
    // =============================================================================

    class session_pool
    {
    public:
        explicit session_pool(session_config config, std::size_t pool_size = 4);
        ~session_pool();

        session_pool(session_pool const &) = delete;
        auto operator=(session_pool const &) -> session_pool & = delete;
        session_pool(session_pool &&) = delete;
        auto operator=(session_pool &&) -> session_pool & = delete;

        // acquire a session from the pool (blocks if none available)
        [[nodiscard]] auto acquire() -> result<session *>;
        auto release(session *sess) -> void;

        // RAII handle for automatic release
        class scoped_session
        {
        public:
            scoped_session(session_pool &pool, session *sess) : pool_{&pool}, session_{sess}
            {
            }
            ~scoped_session()
            {
                if (session_)
                {
                    pool_->release(session_);
                }
            }

            scoped_session(scoped_session const &) = delete;
            auto operator=(scoped_session const &) -> scoped_session & = delete;

            scoped_session(scoped_session &&other) noexcept : pool_{other.pool_}, session_{other.session_}
            {
                other.session_ = nullptr;
            }
            auto operator=(scoped_session &&other) noexcept -> scoped_session &
            {
                if (this != &other)
                {
                    if (session_)
                    {
                        pool_->release(session_);
                    }
                    pool_ = other.pool_;
                    session_ = other.session_;
                    other.session_ = nullptr;
                }
                return *this;
            }

            [[nodiscard]] auto get() const noexcept -> session *
            {
                return session_;
            }
            [[nodiscard]] auto operator->() const noexcept -> session *
            {
                return session_;
            }
            [[nodiscard]] auto operator*() const noexcept -> session &
            {
                return *session_;
            }
            [[nodiscard]] explicit operator bool() const noexcept
            {
                return session_ != nullptr;
            }

        private:
            session_pool *pool_;
            session *session_;
        };

        [[nodiscard]] auto acquire_scoped() -> result<scoped_session>;

    private:
        session_config config_;
        std::vector<std::unique_ptr<session>> sessions_;
        std::vector<bool> in_use_;
        std::mutex mutex_;
        std::condition_variable cv_;
    };

} // namespace l2net::ssh
