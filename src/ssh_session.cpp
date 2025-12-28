// ssh_session.cpp - SSH session implementation using libssh SFTP
// replaces deprecated SCP API with SFTP for file transfers

#include "l2net/ssh_session.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

// libssh headers - order matters due to internal dependencies
#include <libssh/libssh.h>
#include <libssh/sftp.h>

// some libssh versions have issues with fcntl.h order
#include <fcntl.h>

namespace l2net::ssh
{

    namespace
    {

        // =============================================================================
        // error category for std::error_code integration
        // =============================================================================

        class ssh_error_category_impl : public std::error_category
        {
        public:
            [[nodiscard]] auto name() const noexcept -> char const * override { return "ssh"; }

            [[nodiscard]] auto message(int ev) const -> std::string override
            {
                switch (static_cast<error>(ev))
                {
                case error::success:
                    return "success";
                case error::not_connected:
                    return "SSH session not connected";
                case error::connection_failed:
                    return "SSH connection failed";
                case error::authentication_failed:
                    return "SSH authentication failed";
                case error::channel_open_failed:
                    return "failed to open SSH channel";
                case error::channel_exec_failed:
                    return "failed to execute command on SSH channel";
                case error::sftp_init_failed:
                    return "failed to initialize SFTP session";
                case error::sftp_open_failed:
                    return "failed to open remote file via SFTP";
                case error::sftp_write_failed:
                    return "failed to write to remote file via SFTP";
                case error::sftp_read_failed:
                    return "failed to read from remote file via SFTP";
                case error::sftp_stat_failed:
                    return "failed to stat remote file via SFTP";
                case error::sftp_remove_failed:
                    return "failed to remove remote file via SFTP";
                case error::file_open_failed:
                    return "failed to open local file";
                case error::file_read_failed:
                    return "failed to read local file";
                case error::file_write_failed:
                    return "failed to write to local file";
                case error::timeout:
                    return "SSH operation timed out";
                case error::host_key_verification_failed:
                    return "SSH host key verification failed";
                // legacy SCP errors - kept for ABI compat
                case error::scp_init_failed:
                    return "SCP init failed (deprecated - use SFTP)";
                case error::scp_push_failed:
                    return "SCP push failed (deprecated - use SFTP)";
                case error::scp_pull_failed:
                    return "SCP pull failed (deprecated - use SFTP)";
                case error::scp_write_failed:
                    return "SCP write failed (deprecated - use SFTP)";
                case error::scp_read_failed:
                    return "SCP read failed (deprecated - use SFTP)";
                default:
                    return fmt::format("unknown SSH error ({})", ev);
                }
            }
        };

        [[nodiscard]] auto ssh_error_category() noexcept -> std::error_category const &
        {
            static ssh_error_category_impl const instance;
            return instance;
        }

        // =============================================================================
        // RAII guards
        // =============================================================================

        struct sftp_session_guard
        {
            sftp_session session{nullptr};

            sftp_session_guard() = default;
            explicit sftp_session_guard(sftp_session s) : session(s) {}
            ~sftp_session_guard()
            {
                if (session != nullptr)
                {
                    sftp_free(session);
                }
            }

            sftp_session_guard(sftp_session_guard const &) = delete;
            auto operator=(sftp_session_guard const &) -> sftp_session_guard & = delete;

            sftp_session_guard(sftp_session_guard &&other) noexcept : session(other.session) { other.session = nullptr; }

            auto operator=(sftp_session_guard &&other) noexcept -> sftp_session_guard &
            {
                if (this != &other)
                {
                    if (session != nullptr)
                    {
                        sftp_free(session);
                    }
                    session = other.session;
                    other.session = nullptr;
                }
                return *this;
            }

            [[nodiscard]] auto get() const noexcept -> sftp_session { return session; }
            [[nodiscard]] explicit operator bool() const noexcept { return session != nullptr; }
        };

        struct sftp_file_guard
        {
            sftp_file file{nullptr};

            sftp_file_guard() = default;
            explicit sftp_file_guard(sftp_file f) : file(f) {}
            ~sftp_file_guard()
            {
                if (file != nullptr)
                {
                    sftp_close(file);
                }
            }

            sftp_file_guard(sftp_file_guard const &) = delete;
            auto operator=(sftp_file_guard const &) -> sftp_file_guard & = delete;

            sftp_file_guard(sftp_file_guard &&other) noexcept : file(other.file) { other.file = nullptr; }

            auto operator=(sftp_file_guard &&other) noexcept -> sftp_file_guard &
            {
                if (this != &other)
                {
                    if (file != nullptr)
                    {
                        sftp_close(file);
                    }
                    file = other.file;
                    other.file = nullptr;
                }
                return *this;
            }

            [[nodiscard]] auto get() const noexcept -> sftp_file { return file; }
            [[nodiscard]] explicit operator bool() const noexcept { return file != nullptr; }
        };

        struct channel_guard
        {
            ssh_channel channel{nullptr};

            channel_guard() = default;
            explicit channel_guard(ssh_channel c) : channel(c) {}
            ~channel_guard()
            {
                if (channel != nullptr)
                {
                    ssh_channel_close(channel);
                    ssh_channel_free(channel);
                }
            }

            channel_guard(channel_guard const &) = delete;
            auto operator=(channel_guard const &) -> channel_guard & = delete;

            channel_guard(channel_guard &&other) noexcept : channel(other.channel) { other.channel = nullptr; }

            auto operator=(channel_guard &&other) noexcept -> channel_guard &
            {
                if (this != &other)
                {
                    if (channel != nullptr)
                    {
                        ssh_channel_close(channel);
                        ssh_channel_free(channel);
                    }
                    channel = other.channel;
                    other.channel = nullptr;
                }
                return *this;
            }

            [[nodiscard]] auto get() const noexcept -> ssh_channel { return channel; }
            [[nodiscard]] explicit operator bool() const noexcept { return channel != nullptr; }

            auto release() noexcept -> ssh_channel
            {
                auto c = channel;
                channel = nullptr;
                return c;
            }
        };

        // SFTP chunk size - 32KB is safe for most servers
        constexpr std::size_t SFTP_CHUNK_SIZE = 32 * 1024;

    } // namespace

    // =============================================================================
    // public error functions
    // =============================================================================

    auto make_error_code(error e) noexcept -> std::error_code
    {
        return {static_cast<int>(e), ssh_error_category()};
    }

    auto to_string(error e) -> std::string
    {
        return ssh_error_category().message(static_cast<int>(e));
    }

    // =============================================================================
    // session implementation
    // =============================================================================

    class session::impl
    {
    public:
        ssh_session ssh_{nullptr};
        std::string host_;
        std::string user_;
        int port_{22};
        std::chrono::seconds command_timeout_{60};

        impl() = default;

        ~impl()
        {
            if (ssh_ != nullptr)
            {
                ssh_disconnect(ssh_);
                ssh_free(ssh_);
            }
        }

        impl(impl const &) = delete;
        auto operator=(impl const &) -> impl & = delete;
        impl(impl &&) = delete;
        auto operator=(impl &&) -> impl & = delete;
    };

    session::session() : impl_(std::make_unique<impl>()) {}

    session::~session() = default;

    session::session(session &&other) noexcept = default;

    auto session::operator=(session &&other) noexcept -> session & = default;

    session::session(std::unique_ptr<impl> pimpl) : impl_(std::move(pimpl)) {}

    auto session::connect(session_config const &config) -> result<session>
    {
        auto pimpl = std::make_unique<impl>();

        pimpl->ssh_ = ssh_new();
        if (pimpl->ssh_ == nullptr)
        {
            return std::unexpected(error::connection_failed);
        }

        pimpl->host_ = config.host;
        pimpl->user_ = config.username;
        pimpl->port_ = config.port;
        pimpl->command_timeout_ = config.command_timeout;

        // set connection options
        ssh_options_set(pimpl->ssh_, SSH_OPTIONS_HOST, config.host.c_str());
        ssh_options_set(pimpl->ssh_, SSH_OPTIONS_PORT, &config.port);
        ssh_options_set(pimpl->ssh_, SSH_OPTIONS_USER, config.username.c_str());

        auto timeout_secs = static_cast<long>(config.connect_timeout.count());
        ssh_options_set(pimpl->ssh_, SSH_OPTIONS_TIMEOUT, &timeout_secs);

        if (config.verbosity > 0)
        {
            int verbosity = SSH_LOG_PROTOCOL;
            ssh_options_set(pimpl->ssh_, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
        }

        // host key checking
        if (!config.strict_host_key_checking)
        {
            // accept any host key - use only for development/testing!
            int strict = 0;
            ssh_options_set(pimpl->ssh_, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);
        }

        // connect
        if (ssh_connect(pimpl->ssh_) != SSH_OK)
        {
            return std::unexpected(error::connection_failed);
        }

        // verify host key (if strict checking enabled)
        if (config.strict_host_key_checking)
        {
            auto state = ssh_session_is_known_server(pimpl->ssh_);
            if (state != SSH_KNOWN_HOSTS_OK)
            {
                return std::unexpected(error::host_key_verification_failed);
            }
        }

        // authenticate
        bool authenticated = false;

        // try key-based auth first
        if (!config.private_key_path.empty())
        {
            ssh_key key = nullptr;
            int rc;

            if (config.private_key_passphrase.empty())
            {
                rc = ssh_pki_import_privkey_file(config.private_key_path.c_str(), nullptr, nullptr, nullptr, &key);
            }
            else
            {
                rc = ssh_pki_import_privkey_file(config.private_key_path.c_str(), config.private_key_passphrase.c_str(),
                                                 nullptr, nullptr, &key);
            }

            if (rc == SSH_OK && key != nullptr)
            {
                rc = ssh_userauth_publickey(pimpl->ssh_, nullptr, key);
                ssh_key_free(key);

                if (rc == SSH_AUTH_SUCCESS)
                {
                    authenticated = true;
                }
            }
        }

        // try default keys if no explicit key or key auth failed
        if (!authenticated)
        {
            if (ssh_userauth_publickey_auto(pimpl->ssh_, nullptr, nullptr) == SSH_AUTH_SUCCESS)
            {
                authenticated = true;
            }
        }

        // try password auth as last resort
        if (!authenticated && !config.password.empty())
        {
            if (ssh_userauth_password(pimpl->ssh_, nullptr, config.password.c_str()) == SSH_AUTH_SUCCESS)
            {
                authenticated = true;
            }
        }

        if (!authenticated)
        {
            return std::unexpected(error::authentication_failed);
        }

        return session(std::move(pimpl));
    }

    void session::disconnect()
    {
        if (impl_ && impl_->ssh_ != nullptr)
        {
            ssh_disconnect(impl_->ssh_);
            ssh_free(impl_->ssh_);
            impl_->ssh_ = nullptr;
        }
    }

    auto session::is_connected() const noexcept -> bool
    {
        return impl_ && impl_->ssh_ != nullptr && ssh_is_connected(impl_->ssh_);
    }

    auto session::host() const noexcept -> std::string_view
    {
        return impl_ ? impl_->host_ : std::string_view{};
    }

    auto session::user() const noexcept -> std::string_view
    {
        return impl_ ? impl_->user_ : std::string_view{};
    }

    auto session::port() const noexcept -> int
    {
        return impl_ ? impl_->port_ : 0;
    }

    auto session::execute(std::string_view command) -> result<command_result>
    {
        if (!is_connected())
        {
            return std::unexpected(error::not_connected);
        }

        channel_guard channel{ssh_channel_new(impl_->ssh_)};
        if (!channel)
        {
            return std::unexpected(error::channel_open_failed);
        }

        if (ssh_channel_open_session(channel.get()) != SSH_OK)
        {
            return std::unexpected(error::channel_open_failed);
        }

        if (ssh_channel_request_exec(channel.get(), std::string(command).c_str()) != SSH_OK)
        {
            return std::unexpected(error::channel_exec_failed);
        }

        command_result result;
        std::array<char, 4096> buffer{};

        // read stdout
        int nbytes = 0;
        while ((nbytes = ssh_channel_read(channel.get(), buffer.data(), buffer.size(), 0)) > 0)
        {
            result.stdout_output.append(buffer.data(), static_cast<std::size_t>(nbytes));
        }

        // read stderr
        while ((nbytes = ssh_channel_read(channel.get(), buffer.data(), buffer.size(), 1)) > 0)
        {
            result.stderr_output.append(buffer.data(), static_cast<std::size_t>(nbytes));
        }

        ssh_channel_send_eof(channel.get());
        result.exit_code = ssh_channel_get_exit_status(channel.get());

        return result;
    }

    auto session::execute_background(std::string_view command) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected(error::not_connected);
        }

        // wrap command with nohup and redirect to avoid blocking
        auto bg_command = fmt::format("nohup sh -c '{}' </dev/null >/dev/null 2>&1 &", command);

        channel_guard channel{ssh_channel_new(impl_->ssh_)};
        if (!channel)
        {
            return std::unexpected(error::channel_open_failed);
        }

        if (ssh_channel_open_session(channel.get()) != SSH_OK)
        {
            return std::unexpected(error::channel_open_failed);
        }

        if (ssh_channel_request_exec(channel.get(), bg_command.c_str()) != SSH_OK)
        {
            return std::unexpected(error::channel_exec_failed);
        }

        // don't wait for output - it's backgrounded
        ssh_channel_send_eof(channel.get());

        return {};
    }

    auto session::upload_file(std::filesystem::path const &local_path, std::string_view remote_path, int mode)
        -> result<void>
    {
        // read local file
        std::ifstream file(local_path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return std::unexpected(error::file_open_failed);
        }

        auto const size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char *>(buffer.data()), size))
        {
            return std::unexpected(error::file_read_failed);
        }

        return upload_data(buffer, remote_path, mode);
    }

    auto session::upload_data(std::span<std::uint8_t const> data, std::string_view remote_path, int mode) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected(error::not_connected);
        }

        // create SFTP session
        sftp_session_guard sftp{sftp_new(impl_->ssh_)};
        if (!sftp)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        if (sftp_init(sftp.get()) != SSH_OK)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        // open remote file for writing
        sftp_file_guard file{sftp_open(sftp.get(), std::string(remote_path).c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC, static_cast<mode_t>(mode))};
        if (!file)
        {
            return std::unexpected(error::sftp_open_failed);
        }

        // write in chunks
        std::size_t offset = 0;
        while (offset < data.size())
        {
            auto const chunk_size = std::min(SFTP_CHUNK_SIZE, data.size() - offset);
            auto const written = sftp_write(file.get(), data.data() + offset, chunk_size);

            if (written < 0)
            {
                return std::unexpected(error::sftp_write_failed);
            }

            offset += static_cast<std::size_t>(written);
        }

        return {};
    }

    auto session::download_file(std::string_view remote_path, std::filesystem::path const &local_path) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected(error::not_connected);
        }

        // create SFTP session
        sftp_session_guard sftp{sftp_new(impl_->ssh_)};
        if (!sftp)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        if (sftp_init(sftp.get()) != SSH_OK)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        // get file size
        auto const remote_str = std::string(remote_path);
        sftp_attributes attrs = sftp_stat(sftp.get(), remote_str.c_str());
        if (attrs == nullptr)
        {
            return std::unexpected(error::sftp_stat_failed);
        }
        auto const file_size = attrs->size;
        sftp_attributes_free(attrs);

        // open remote file for reading
        sftp_file_guard file{sftp_open(sftp.get(), remote_str.c_str(), O_RDONLY, 0)};
        if (!file)
        {
            return std::unexpected(error::sftp_open_failed);
        }

        // read into buffer
        std::vector<std::uint8_t> buffer;
        buffer.reserve(static_cast<std::size_t>(file_size));

        std::array<std::uint8_t, SFTP_CHUNK_SIZE> chunk{};
        ssize_t nbytes = 0;

        while ((nbytes = sftp_read(file.get(), chunk.data(), chunk.size())) > 0)
        {
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + nbytes);
        }

        if (nbytes < 0)
        {
            return std::unexpected(error::sftp_read_failed);
        }

        // write to local file
        std::ofstream out(local_path, std::ios::binary);
        if (!out)
        {
            return std::unexpected(error::file_open_failed);
        }

        if (!out.write(reinterpret_cast<char const *>(buffer.data()), static_cast<std::streamsize>(buffer.size())))
        {
            return std::unexpected(error::file_write_failed);
        }

        return {};
    }

    auto session::remove_file(std::string_view remote_path) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected(error::not_connected);
        }

        // create SFTP session
        sftp_session_guard sftp{sftp_new(impl_->ssh_)};
        if (!sftp)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        if (sftp_init(sftp.get()) != SSH_OK)
        {
            return std::unexpected(error::sftp_init_failed);
        }

        if (sftp_unlink(sftp.get(), std::string(remote_path).c_str()) != SSH_OK)
        {
            return std::unexpected(error::sftp_remove_failed);
        }

        return {};
    }

    auto session::get_remote_mac(std::string_view interface) -> result<std::string>
    {
        auto cmd = fmt::format("cat /sys/class/net/{}/address 2>/dev/null", interface);
        auto result = execute(cmd);

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        if (!result->success() || result->stdout_output.empty())
        {
            return std::unexpected(error::channel_exec_failed);
        }

        // trim whitespace
        auto mac = result->stdout_output;
        while (!mac.empty() && (mac.back() == '\n' || mac.back() == '\r' || mac.back() == ' '))
        {
            mac.pop_back();
        }

        return mac;
    }

    auto session::get_remote_hostname() -> result<std::string>
    {
        auto result = execute("hostname");

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        if (!result->success())
        {
            return std::unexpected(error::channel_exec_failed);
        }

        // trim whitespace
        auto hostname = result->stdout_output;
        while (!hostname.empty() && (hostname.back() == '\n' || hostname.back() == '\r' || hostname.back() == ' '))
        {
            hostname.pop_back();
        }

        return hostname;
    }

    auto session::get_remote_mtu(std::string_view interface) -> result<int>
    {
        auto cmd = fmt::format("cat /sys/class/net/{}/mtu 2>/dev/null", interface);
        auto result = execute(cmd);

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        if (!result->success() || result->stdout_output.empty())
        {
            return std::unexpected(error::channel_exec_failed);
        }

        try
        {
            return std::stoi(result->stdout_output);
        }
        catch (...)
        {
            return std::unexpected(error::channel_exec_failed);
        }
    }

    auto session::get_remote_arch() -> result<std::string>
    {
        auto result = execute("uname -m");

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        if (!result->success())
        {
            return std::unexpected(error::channel_exec_failed);
        }

        // trim whitespace
        auto arch = result->stdout_output;
        while (!arch.empty() && (arch.back() == '\n' || arch.back() == '\r' || arch.back() == ' '))
        {
            arch.pop_back();
        }

        return arch;
    }

    auto session::check_remote_binary(std::string_view binary_path) -> result<bool>
    {
        auto cmd = fmt::format("test -x '{}' && echo 'exists'", binary_path);
        auto result = execute(cmd);

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        return result->stdout_output.find("exists") != std::string::npos;
    }

    auto session::kill_remote_process(std::string_view process_pattern) -> result<void>
    {
        auto cmd = fmt::format("pkill -f '{}' 2>/dev/null || true", process_pattern);
        auto result = execute(cmd);

        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }

        // pkill returns non-zero if no processes matched, but we ignore that
        return {};
    }

} // namespace l2net::ssh