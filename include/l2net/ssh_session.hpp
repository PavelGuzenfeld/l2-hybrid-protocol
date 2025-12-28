// ssh_session.hpp - SSH session management for remote operations
// uses SFTP for file transfer (libssh deprecated SCP in 0.10.x)

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace l2net::ssh
{

    // =============================================================================
    // error codes
    // =============================================================================

    enum class error
    {
        success = 0,
        not_connected,
        connection_failed,
        authentication_failed,
        channel_open_failed,
        channel_exec_failed,
        sftp_init_failed,
        sftp_open_failed,
        sftp_write_failed,
        sftp_read_failed,
        sftp_stat_failed,
        sftp_remove_failed,
        file_open_failed,
        file_read_failed,
        file_write_failed,
        timeout,
        host_key_verification_failed,

        // legacy SCP errors - DEPRECATED, do not use in new code
        // kept only for ABI compatibility
        scp_init_failed,
        scp_push_failed,
        scp_pull_failed,
        scp_write_failed,
        scp_read_failed,
    };

    [[nodiscard]] auto make_error_code(error e) noexcept -> std::error_code;

    /// @brief Convert error to human-readable string
    [[nodiscard]] auto to_string(error e) -> std::string;

} // namespace l2net::ssh

// enable std::error_code integration
template <>
struct std::is_error_code_enum<l2net::ssh::error> : std::true_type
{
};

namespace l2net::ssh
{

    // =============================================================================
    // result type alias
    // =============================================================================

    template <typename T>
    using result = std::expected<T, error>;

    // =============================================================================
    // session configuration
    // =============================================================================

    struct session_config
    {
        std::string host;
        int port{22};
        std::string username;
        std::string password;                   // optional, prefer key auth
        std::filesystem::path private_key_path; // optional, uses default if empty
        std::string private_key_passphrase;     // optional
        std::chrono::seconds connect_timeout{30};
        std::chrono::seconds command_timeout{60};
        bool strict_host_key_checking{false}; // set true for production
        int verbosity{0};                     // 0=quiet, 1+=verbose
    };

    // =============================================================================
    // command result
    // =============================================================================

    struct command_result
    {
        std::string stdout_output;
        std::string stderr_output;
        int exit_code{0};

        [[nodiscard]] auto success() const noexcept -> bool { return exit_code == 0; }
    };

    // =============================================================================
    // SSH session
    // =============================================================================

    class session
    {
    public:
        session();
        ~session();

        // move-only type
        session(session const &) = delete;
        auto operator=(session const &) -> session & = delete;
        session(session &&) noexcept;
        auto operator=(session &&) noexcept -> session &;

        // -------------------------------------------------------------------------
        // connection management
        // -------------------------------------------------------------------------

        [[nodiscard]] static auto connect(session_config const &config) -> result<session>;

        void disconnect();

        [[nodiscard]] auto is_connected() const noexcept -> bool;
        [[nodiscard]] auto host() const noexcept -> std::string_view;
        [[nodiscard]] auto user() const noexcept -> std::string_view;
        [[nodiscard]] auto port() const noexcept -> int;

        // -------------------------------------------------------------------------
        // command execution
        // -------------------------------------------------------------------------

        [[nodiscard]] auto execute(std::string_view command) -> result<command_result>;
        [[nodiscard]] auto execute_background(std::string_view command) -> result<void>;

        // -------------------------------------------------------------------------
        // file transfer (SFTP)
        // -------------------------------------------------------------------------

        [[nodiscard]] auto upload_file(std::filesystem::path const &local_path,
                                       std::string_view remote_path,
                                       int mode = 0644) -> result<void>;

        [[nodiscard]] auto upload_data(std::span<std::uint8_t const> data,
                                       std::string_view remote_path,
                                       int mode = 0644) -> result<void>;

        [[nodiscard]] auto download_file(std::string_view remote_path,
                                         std::filesystem::path const &local_path) -> result<void>;

        [[nodiscard]] auto remove_file(std::string_view remote_path) -> result<void>;

        // -------------------------------------------------------------------------
        // utility functions
        // -------------------------------------------------------------------------

        [[nodiscard]] auto get_remote_mac(std::string_view interface) -> result<std::string>;
        [[nodiscard]] auto get_remote_hostname() -> result<std::string>;
        [[nodiscard]] auto get_remote_mtu(std::string_view interface) -> result<int>;
        [[nodiscard]] auto get_remote_arch() -> result<std::string>;
        [[nodiscard]] auto check_remote_binary(std::string_view binary_path) -> result<bool>;
        [[nodiscard]] auto kill_remote_process(std::string_view process_pattern) -> result<void>;

    private:
        class impl;
        std::unique_ptr<impl> impl_;

        explicit session(std::unique_ptr<impl> impl);
    };

} // namespace l2net::ssh