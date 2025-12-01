// ssh_session.cpp - SSH wrapper implementation
// wrapping libssh because life is pain

#include "l2net/ssh_session.hpp"

#include <fmt/format.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>

namespace l2net::ssh
{

    // =============================================================================
    // session implementation
    // =============================================================================

    session::session(ssh_session_struct *raw_session, session_config config)
        : session_{raw_session}, config_{std::move(config)}
    {
    }

    session::~session()
    {
        disconnect();
    }

    session::session(session &&other) noexcept
        : session_{other.session_}, config_{std::move(other.config_)}
    {
        other.session_ = nullptr;
    }

    auto session::operator=(session &&other) noexcept -> session &
    {
        if (this != &other)
        {
            disconnect();
            session_ = other.session_;
            config_ = std::move(other.config_);
            other.session_ = nullptr;
        }
        return *this;
    }

    auto session::connect(session_config const &config) -> result<session>
    {
        // create new ssh session
        ssh_session raw = ssh_new();
        if (raw == nullptr)
        {
            return std::unexpected{error_code::unknown_error};
        }

        // set options - libssh uses this archaic C api because why not
        ssh_options_set(raw, SSH_OPTIONS_HOST, config.host.c_str());

        int const port = static_cast<int>(config.port);
        ssh_options_set(raw, SSH_OPTIONS_PORT, &port);

        ssh_options_set(raw, SSH_OPTIONS_USER, config.username.c_str());

        long const timeout = static_cast<long>(config.connect_timeout.count());
        ssh_options_set(raw, SSH_OPTIONS_TIMEOUT, &timeout);

        int const verbosity = config.verbosity;
        ssh_options_set(raw, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

        // disable strict host key checking if requested
        // (for testing - don't do this in production unless you enjoy being pwned)
        if (!config.strict_host_key_checking)
        {
            int const strict = 0;
            ssh_options_set(raw, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);
        }

        // connect to server
        int rc = ssh_connect(raw);
        if (rc != SSH_OK)
        {
            ssh_free(raw);
            return std::unexpected{error_code::connection_failed};
        }

        // verify host key (if strict checking enabled)
        if (config.strict_host_key_checking)
        {
            ssh_key srv_pubkey = nullptr;
            rc = ssh_get_server_publickey(raw, &srv_pubkey);
            if (rc < 0)
            {
                ssh_disconnect(raw);
                ssh_free(raw);
                return std::unexpected{error_code::host_key_failed};
            }

            unsigned char *hash = nullptr;
            std::size_t hlen = 0;
            rc = ssh_get_publickey_hash(srv_pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
            ssh_key_free(srv_pubkey);

            if (rc < 0)
            {
                ssh_disconnect(raw);
                ssh_free(raw);
                return std::unexpected{error_code::host_key_failed};
            }

            // check if host is known
            enum ssh_known_hosts_e const state = ssh_session_is_known_server(raw);
            ssh_clean_pubkey_hash(&hash);

            if (state != SSH_KNOWN_HOSTS_OK && state != SSH_KNOWN_HOSTS_NOT_FOUND)
            {
                ssh_disconnect(raw);
                ssh_free(raw);
                return std::unexpected{error_code::host_key_failed};
            }

            // add host to known hosts if not found
            if (state == SSH_KNOWN_HOSTS_NOT_FOUND)
            {
                ssh_session_update_known_hosts(raw);
            }
        }

        // authenticate - try key-based first if provided
        if (!config.private_key_path.empty())
        {
            ssh_key private_key = nullptr;

            if (config.private_key_passphrase.empty())
            {
                rc = ssh_pki_import_privkey_file(
                    config.private_key_path.c_str(),
                    nullptr,
                    nullptr,
                    nullptr,
                    &private_key);
            }
            else
            {
                rc = ssh_pki_import_privkey_file(
                    config.private_key_path.c_str(),
                    config.private_key_passphrase.c_str(),
                    nullptr,
                    nullptr,
                    &private_key);
            }

            if (rc == SSH_OK && private_key != nullptr)
            {
                rc = ssh_userauth_publickey(raw, nullptr, private_key);
                ssh_key_free(private_key);

                if (rc == SSH_AUTH_SUCCESS)
                {
                    return session{raw, config};
                }
            }
        }

        // fall back to password auth
        if (!config.password.empty())
        {
            rc = ssh_userauth_password(raw, nullptr, config.password.c_str());
            if (rc == SSH_AUTH_SUCCESS)
            {
                return session{raw, config};
            }
        }

        // all auth methods failed
        ssh_disconnect(raw);
        ssh_free(raw);
        return std::unexpected{error_code::authentication_failed};
    }

    auto session::is_connected() const noexcept -> bool
    {
        return session_ != nullptr && ssh_is_connected(session_);
    }

    auto session::disconnect() -> void
    {
        if (session_ != nullptr)
        {
            if (ssh_is_connected(session_))
            {
                ssh_disconnect(session_);
            }
            ssh_free(session_);
            session_ = nullptr;
        }
    }

    auto session::open_channel() -> result<ssh_channel_struct *>
    {
        if (!is_connected())
        {
            return std::unexpected{error_code::session_invalid};
        }

        ssh_channel channel = ssh_channel_new(session_);
        if (channel == nullptr)
        {
            return std::unexpected{error_code::channel_open_failed};
        }

        int const rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK)
        {
            ssh_channel_free(channel);
            return std::unexpected{error_code::channel_open_failed};
        }

        return channel;
    }

    auto session::close_channel(ssh_channel_struct *channel) -> void
    {
        if (channel != nullptr)
        {
            ssh_channel_send_eof(channel);
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        }
    }

    auto session::execute(std::string_view command) -> result<command_result>
    {
        return execute(command, config_.command_timeout);
    }

    auto session::execute(
        std::string_view command,
        std::chrono::seconds timeout) -> result<command_result>
    {
        auto channel_result = open_channel();
        if (!channel_result.has_value())
        {
            return std::unexpected{channel_result.error()};
        }

        ssh_channel channel = *channel_result;

        // request command execution
        int rc = ssh_channel_request_exec(channel, std::string{command}.c_str());
        if (rc != SSH_OK)
        {
            close_channel(channel);
            return std::unexpected{error_code::command_failed};
        }

        command_result result{};
        std::array<char, 4096> buffer{};

        auto const deadline = std::chrono::steady_clock::now() + timeout;

        // read stdout and stderr
        while (!ssh_channel_is_eof(channel))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                close_channel(channel);
                return std::unexpected{error_code::timeout};
            }

            // read stdout
            int nbytes = ssh_channel_read_timeout(
                channel, buffer.data(), buffer.size(), 0, 100);
            if (nbytes > 0)
            {
                result.stdout_output.append(buffer.data(), static_cast<std::size_t>(nbytes));
            }

            // read stderr
            nbytes = ssh_channel_read_timeout(
                channel, buffer.data(), buffer.size(), 1, 100);
            if (nbytes > 0)
            {
                result.stderr_output.append(buffer.data(), static_cast<std::size_t>(nbytes));
            }
        }

        // get exit status
        result.exit_code = ssh_channel_get_exit_status(channel);

        close_channel(channel);
        return result;
    }

    auto session::execute_streaming(
        std::string_view command,
        std::function<void(std::string_view, bool)> const &output_callback) -> result<int>
    {
        auto channel_result = open_channel();
        if (!channel_result.has_value())
        {
            return std::unexpected{channel_result.error()};
        }

        ssh_channel channel = *channel_result;

        int rc = ssh_channel_request_exec(channel, std::string{command}.c_str());
        if (rc != SSH_OK)
        {
            close_channel(channel);
            return std::unexpected{error_code::command_failed};
        }

        std::array<char, 4096> buffer{};

        while (!ssh_channel_is_eof(channel))
        {
            // read stdout
            int nbytes = ssh_channel_read_timeout(
                channel, buffer.data(), buffer.size(), 0, 100);
            if (nbytes > 0)
            {
                output_callback(
                    std::string_view{buffer.data(), static_cast<std::size_t>(nbytes)},
                    false);
            }

            // read stderr
            nbytes = ssh_channel_read_timeout(
                channel, buffer.data(), buffer.size(), 1, 100);
            if (nbytes > 0)
            {
                output_callback(
                    std::string_view{buffer.data(), static_cast<std::size_t>(nbytes)},
                    true);
            }
        }

        int const exit_code = ssh_channel_get_exit_status(channel);
        close_channel(channel);
        return exit_code;
    }

    auto session::upload_file(
        std::filesystem::path const &local_path,
        std::string_view remote_path,
        int mode) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected{error_code::session_invalid};
        }

        // read local file
        std::ifstream file{local_path, std::ios::binary | std::ios::ate};
        if (!file.is_open())
        {
            return std::unexpected{error_code::file_not_found};
        }

        auto const size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char *>(data.data()), size))
        {
            return std::unexpected{error_code::file_not_found};
        }

        return upload_data(data, remote_path, mode);
    }

    auto session::upload_data(
        std::span<std::uint8_t const> data,
        std::string_view remote_path,
        int mode) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected{error_code::session_invalid};
        }

        // use SCP for file upload
        ssh_scp scp = ssh_scp_new(
            session_,
            SSH_SCP_WRITE,
            std::string{remote_path}.c_str());

        if (scp == nullptr)
        {
            return std::unexpected{error_code::scp_init_failed};
        }

        int rc = ssh_scp_init(scp);
        if (rc != SSH_OK)
        {
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_init_failed};
        }

        // extract filename from path
        std::string_view filename = remote_path;
        if (auto const pos = remote_path.rfind('/'); pos != std::string_view::npos)
        {
            filename = remote_path.substr(pos + 1);
        }

        rc = ssh_scp_push_file(
            scp,
            std::string{filename}.c_str(),
            data.size(),
            mode);

        if (rc != SSH_OK)
        {
            ssh_scp_close(scp);
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_push_failed};
        }

        rc = ssh_scp_write(scp, data.data(), data.size());
        if (rc != SSH_OK)
        {
            ssh_scp_close(scp);
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_push_failed};
        }

        ssh_scp_close(scp);
        ssh_scp_free(scp);
        return {};
    }

    auto session::download_file(
        std::string_view remote_path,
        std::filesystem::path const &local_path) -> result<void>
    {
        if (!is_connected())
        {
            return std::unexpected{error_code::session_invalid};
        }

        ssh_scp scp = ssh_scp_new(
            session_,
            SSH_SCP_READ,
            std::string{remote_path}.c_str());

        if (scp == nullptr)
        {
            return std::unexpected{error_code::scp_init_failed};
        }

        int rc = ssh_scp_init(scp);
        if (rc != SSH_OK)
        {
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_init_failed};
        }

        rc = ssh_scp_pull_request(scp);
        if (rc != SSH_SCP_REQUEST_NEWFILE)
        {
            ssh_scp_close(scp);
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_read_failed};
        }

        std::size_t const size = ssh_scp_request_get_size(scp);
        ssh_scp_accept_request(scp);

        std::vector<char> buffer(size);
        rc = ssh_scp_read(scp, buffer.data(), size);
        if (rc == SSH_ERROR)
        {
            ssh_scp_close(scp);
            ssh_scp_free(scp);
            return std::unexpected{error_code::scp_read_failed};
        }

        ssh_scp_close(scp);
        ssh_scp_free(scp);

        // write to local file
        std::ofstream file{local_path, std::ios::binary};
        if (!file.is_open())
        {
            return std::unexpected{error_code::permission_denied};
        }

        file.write(buffer.data(), static_cast<std::streamsize>(size));
        return {};
    }

    auto session::file_exists(std::string_view remote_path) -> result<bool>
    {
        auto result = execute(fmt::format("test -f '{}' && echo 'yes' || echo 'no'", remote_path));
        if (!result.has_value())
        {
            return std::unexpected{result.error()};
        }

        return result->stdout_output.find("yes") != std::string::npos;
    }

    auto session::make_directory(std::string_view remote_path) -> result<void>
    {
        auto result = execute(fmt::format("mkdir -p '{}'", remote_path));
        if (!result.has_value())
        {
            return std::unexpected{result.error()};
        }
        if (!result->success())
        {
            return std::unexpected{error_code::permission_denied};
        }
        return {};
    }

    auto session::remove_file(std::string_view remote_path) -> result<void>
    {
        auto result = execute(fmt::format("rm -f '{}'", remote_path));
        if (!result.has_value())
        {
            return std::unexpected{result.error()};
        }
        return {};
    }

    auto session::get_remote_arch() -> result<std::string>
    {
        auto result = execute("uname -m");
        if (!result.has_value())
        {
            return std::unexpected{result.error()};
        }

        // trim whitespace
        auto arch = result->stdout_output;
        arch.erase(std::remove_if(arch.begin(), arch.end(), ::isspace), arch.end());
        return arch;
    }

    auto session::last_error_message() const -> std::string
    {
        if (session_ == nullptr)
        {
            return "session not initialized";
        }
        char const *msg = ssh_get_error(session_);
        return msg != nullptr ? msg : "unknown error";
    }

    // =============================================================================
    // session_pool implementation
    // =============================================================================

    session_pool::session_pool(session_config config, std::size_t pool_size)
        : config_{std::move(config)}
    {
        sessions_.reserve(pool_size);
        in_use_.resize(pool_size, false);

        for (std::size_t i = 0; i < pool_size; ++i)
        {
            auto result = session::connect(config_);
            if (result.has_value())
            {
                sessions_.push_back(std::make_unique<session>(std::move(*result)));
            }
        }
    }

    session_pool::~session_pool() = default;

    auto session_pool::acquire() -> result<session *>
    {
        std::unique_lock lock{mutex_};

        cv_.wait(lock, [this]
                 {
        for (std::size_t i = 0; i < sessions_.size(); ++i) {
            if (!in_use_[i]) return true;
        }
        return false; });

        for (std::size_t i = 0; i < sessions_.size(); ++i)
        {
            if (!in_use_[i])
            {
                in_use_[i] = true;

                // reconnect if needed
                if (!sessions_[i]->is_connected())
                {
                    auto result = session::connect(config_);
                    if (!result.has_value())
                    {
                        in_use_[i] = false;
                        return std::unexpected{result.error()};
                    }
                    sessions_[i] = std::make_unique<session>(std::move(*result));
                }

                return sessions_[i].get();
            }
        }

        return std::unexpected{error_code::unknown_error};
    }

    auto session_pool::release(session *sess) -> void
    {
        std::lock_guard lock{mutex_};

        for (std::size_t i = 0; i < sessions_.size(); ++i)
        {
            if (sessions_[i].get() == sess)
            {
                in_use_[i] = false;
                cv_.notify_one();
                return;
            }
        }
    }

    auto session_pool::acquire_scoped() -> result<scoped_session>
    {
        auto result = acquire();
        if (!result.has_value())
        {
            return std::unexpected{result.error()};
        }
        return scoped_session{*this, *result};
    }

} // namespace l2net::ssh