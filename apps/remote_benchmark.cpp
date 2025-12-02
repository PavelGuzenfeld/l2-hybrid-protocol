// remote_benchmark.cpp - SSH-based remote benchmark orchestrator
// because real engineers test across actual networks, not loopback

#include "l2net/ssh_session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtautological-compare"
#include <fmt/ranges.h>
#pragma GCC diagnostic pop
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

    std::atomic<bool> g_running{true};

    auto signal_handler(int /*signal*/) -> void
    {
        g_running.store(false);
    }

    // =============================================================================
    // configuration
    // =============================================================================

    struct benchmark_config
    {
        // ssh settings
        std::string remote_host;
        std::uint16_t ssh_port{22};
        std::string ssh_username;
        std::string ssh_password;
        std::string ssh_key_path;

        // network settings
        std::string local_interface;
        std::string remote_interface;
        std::string local_mac;  // auto-detected if empty
        std::string remote_mac; // auto-detected if empty

        // benchmark settings
        std::vector<std::size_t> payload_sizes{64, 128, 256, 512, 1024, 1400, 4096, 8192};
        std::uint64_t packets_per_test{10000};
        std::uint64_t warmup_packets{100};
        std::chrono::seconds test_timeout{60};

        // vlan settings
        std::uint16_t vlan_id{0};
        std::uint8_t vlan_priority{0};
        bool use_vlan{false};

        // output settings
        std::string output_file;
        bool verbose{false};
        bool json_output{false};

        // paths
        std::filesystem::path local_binary;
        std::string remote_binary_path{"/tmp/l2net_remote_node"};
    };

    // =============================================================================
    // benchmark results
    // =============================================================================

    struct latency_result
    {
        std::size_t payload_size;
        std::uint64_t packets_sent;
        std::uint64_t packets_received;
        double loss_percent;

        // latency in microseconds
        double min_us;
        double max_us;
        double avg_us;
        double p50_us;
        double p95_us;
        double p99_us;
        double stddev_us;
    };

    struct throughput_result
    {
        std::size_t payload_size;
        std::uint64_t packets_sent;
        std::uint64_t bytes_sent;
        double duration_ms;
        double packets_per_sec;
        double mbps;
        double gbps;
    };

    struct benchmark_results
    {
        std::string timestamp;
        std::string local_host;
        std::string remote_host;
        std::string local_interface;
        std::string remote_interface;
        std::string local_mac;
        std::string remote_mac;

        std::vector<latency_result> latency_results;
        std::vector<throughput_result> throughput_results;
    };

    // =============================================================================
    // result parsing - extracting numbers from remote_node output
    // =============================================================================

    [[nodiscard]] auto parse_latency_output(std::string const &output, std::size_t payload_size)
        -> std::optional<latency_result>
    {
        latency_result result{};
        result.payload_size = payload_size;

        // parse: "X packets transmitted, Y received, Z% packet loss"
        if (auto pos = output.find("packets transmitted"); pos != std::string::npos)
        {
            auto line_start = output.rfind('\n', pos);
            if (line_start == std::string::npos)
            {
                line_start = 0;
            }
            else
            {
                ++line_start;
            }

            auto line = output.substr(line_start, output.find('\n', pos) - line_start);

            if (std::sscanf(line.c_str(), "%lu packets transmitted, %lu received", &result.packets_sent,
                            &result.packets_received) >= 2)
            {

                if (result.packets_sent > 0)
                {
                    result.loss_percent = 100.0 * static_cast<double>(result.packets_sent - result.packets_received) /
                                          static_cast<double>(result.packets_sent);
                }
            }
        }

        // parse: "rtt min/avg/max/p50/p99 = X/X/X/X/X us"
        if (auto pos = output.find("rtt min/avg/max"); pos != std::string::npos)
        {
            auto line_start = output.rfind('\n', pos);
            if (line_start == std::string::npos)
            {
                line_start = 0;
            }
            else
            {
                ++line_start;
            }

            auto line = output.substr(line_start);

            // find the = sign
            auto eq_pos = line.find('=');
            if (eq_pos != std::string::npos)
            {
                auto values_str = line.substr(eq_pos + 1);

                // try parsing 5 values first (min/avg/max/p50/p99)
                if (std::sscanf(values_str.c_str(), " %lf/%lf/%lf/%lf/%lf", &result.min_us, &result.avg_us,
                                &result.max_us, &result.p50_us, &result.p99_us) >= 5)
                {
                    result.p95_us = result.p99_us; // approximate
                }
            }
        }

        // calculate stddev from output if available, otherwise estimate
        result.stddev_us = (result.max_us - result.min_us) / 4.0;

        return result;
    }

    [[nodiscard]] auto parse_throughput_output(std::string const &output, std::size_t payload_size)
        -> std::optional<throughput_result>
    {
        throughput_result result{};
        result.payload_size = payload_size;

        // parse: "Packets sent: X"
        if (auto pos = output.find("Packets sent:"); pos != std::string::npos)
        {
            std::sscanf(output.c_str() + pos, "Packets sent: %lu", &result.packets_sent);
        }

        // parse: "Bytes sent: X"
        if (auto pos = output.find("Bytes sent:"); pos != std::string::npos)
        {
            std::sscanf(output.c_str() + pos, "Bytes sent: %lu", &result.bytes_sent);
        }

        // parse: "Duration: X ms"
        if (auto pos = output.find("Duration:"); pos != std::string::npos)
        {
            std::sscanf(output.c_str() + pos, "Duration: %lf", &result.duration_ms);
        }

        // parse: "Average: X pps, Y Mbps"
        if (auto pos = output.find("Average:"); pos != std::string::npos)
        {
            std::sscanf(output.c_str() + pos, "Average: %lf pps, %lf Mbps", &result.packets_per_sec, &result.mbps);
        }

        result.gbps = result.mbps / 1000.0;

        return result;
    }

    // =============================================================================
    // benchmark orchestrator
    // =============================================================================

    class benchmark_orchestrator
    {
    public:
        explicit benchmark_orchestrator(benchmark_config config) : config_{std::move(config)}
        {
        }

        [[nodiscard]] auto run() -> std::optional<benchmark_results>;

    private:
        [[nodiscard]] auto connect_ssh() -> bool;
        [[nodiscard]] auto deploy_binary() -> bool;
        [[nodiscard]] auto detect_mac_addresses() -> bool;
        [[nodiscard]] auto run_latency_tests() -> std::vector<latency_result>;
        [[nodiscard]] auto run_throughput_tests() -> std::vector<throughput_result>;
        [[nodiscard]] auto run_single_latency_test(std::size_t payload_size) -> std::optional<latency_result>;
        [[nodiscard]] auto run_single_throughput_test(std::size_t payload_size) -> std::optional<throughput_result>;
        auto cleanup_remote() -> void;
        auto kill_remote_processes() -> void;

        auto print_status(std::string_view msg) -> void;
        auto print_error(std::string_view msg) -> void;
        auto print_progress(std::string_view test_type, std::size_t current, std::size_t total) -> void;

        benchmark_config config_;
        std::unique_ptr<l2net::ssh::session> ssh_session_;
        benchmark_results results_{};
    };

    auto benchmark_orchestrator::run() -> std::optional<benchmark_results>
    {
        // setup signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // initialize results
        auto const now = std::chrono::system_clock::now();
        results_.timestamp = fmt::format("{:%Y-%m-%d %H:%M:%S}", now);
        results_.remote_host = config_.remote_host;
        results_.local_interface = config_.local_interface;
        results_.remote_interface = config_.remote_interface;

        // get local hostname
        std::array<char, 256> hostname{};
        if (gethostname(hostname.data(), hostname.size()) == 0)
        {
            results_.local_host = hostname.data();
        }

        print_status("connecting to remote host...");
        if (!connect_ssh())
        {
            print_error("failed to connect via ssh");
            return std::nullopt;
        }

        print_status("deploying benchmark binary...");
        if (!deploy_binary())
        {
            print_error("failed to deploy binary");
            return std::nullopt;
        }

        print_status("detecting mac addresses...");
        if (!detect_mac_addresses())
        {
            print_error("failed to detect mac addresses");
            cleanup_remote();
            return std::nullopt;
        }

        results_.local_mac = config_.local_mac;
        results_.remote_mac = config_.remote_mac;

        fmt::print(fmt::fg(fmt::color::cyan),
                   "\n=== Benchmark Configuration ===\n"
                   "Local:  {} ({}) - {}\n"
                   "Remote: {} ({}) - {}\n"
                   "Payload sizes: {}\n"
                   "Packets per test: {}\n\n",
                   results_.local_host, config_.local_interface, config_.local_mac, config_.remote_host,
                   config_.remote_interface, config_.remote_mac, config_.payload_sizes, config_.packets_per_test);

        // run latency tests
        fmt::print(fmt::fg(fmt::color::yellow), "\n=== Running Latency Tests ===\n\n");
        results_.latency_results = run_latency_tests();

        if (!g_running.load())
        {
            print_status("benchmark interrupted");
            cleanup_remote();
            return results_;
        }

        // run throughput tests
        fmt::print(fmt::fg(fmt::color::yellow), "\n=== Running Throughput Tests ===\n\n");
        results_.throughput_results = run_throughput_tests();

        // cleanup
        cleanup_remote();

        return results_;
    }

    auto benchmark_orchestrator::connect_ssh() -> bool
    {
        l2net::ssh::session_config ssh_config{.host = config_.remote_host,
                                              .port = config_.ssh_port,
                                              .username = config_.ssh_username,
                                              .password = config_.ssh_password,
                                              .private_key_path = config_.ssh_key_path,
                                              .private_key_passphrase = {},
                                              .connect_timeout = std::chrono::seconds{30},
                                              .command_timeout = config_.test_timeout,
                                              .strict_host_key_checking = false,
                                              .verbosity = config_.verbose ? 1 : 0};

        auto result = l2net::ssh::session::connect(ssh_config);
        if (!result.has_value())
        {
            print_error(fmt::format("ssh connection failed: {}", l2net::ssh::to_string(result.error())));
            return false;
        }

        ssh_session_ = std::make_unique<l2net::ssh::session>(std::move(*result));

        // verify we can run commands
        auto test_result = ssh_session_->execute("echo 'ssh connection test'");
        if (!test_result.has_value() || !test_result->success())
        {
            print_error("ssh command execution test failed");
            return false;
        }

        if (config_.verbose)
        {
            print_status(fmt::format("connected to {} as {}", config_.remote_host, config_.ssh_username));
        }

        return true;
    }

    auto benchmark_orchestrator::deploy_binary() -> bool
    {
        // check if local binary exists
        if (!std::filesystem::exists(config_.local_binary))
        {
            print_error(fmt::format("local binary not found: {}", config_.local_binary.string()));
            return false;
        }

        // check remote architecture
        auto arch_result = ssh_session_->get_remote_arch();
        if (!arch_result.has_value())
        {
            print_error("failed to detect remote architecture");
            return false;
        }

        if (config_.verbose)
        {
            print_status(fmt::format("remote architecture: {}", *arch_result));
        }

        // upload binary
        print_status(fmt::format("uploading {} to {}:{}", config_.local_binary.filename().string(), config_.remote_host,
                                 config_.remote_binary_path));

        auto upload_result = ssh_session_->upload_file(config_.local_binary, config_.remote_binary_path, 0755);

        if (!upload_result.has_value())
        {
            print_error(fmt::format("failed to upload binary: {}", l2net::ssh::to_string(upload_result.error())));
            return false;
        }

        // verify upload
        auto verify_result =
            ssh_session_->execute(fmt::format("test -x '{}' && echo 'ok'", config_.remote_binary_path));

        if (!verify_result.has_value() || verify_result->stdout_output.find("ok") == std::string::npos)
        {
            print_error("binary verification failed");
            return false;
        }

        print_status("binary deployed successfully");
        return true;
    }

    auto benchmark_orchestrator::detect_mac_addresses() -> bool
    {
        // detect local mac if not specified
        if (config_.local_mac.empty())
        {
            std::ifstream mac_file{fmt::format("/sys/class/net/{}/address", config_.local_interface)};
            if (mac_file.is_open())
            {
                std::getline(mac_file, config_.local_mac);
                // trim whitespace
                config_.local_mac.erase(std::remove_if(config_.local_mac.begin(), config_.local_mac.end(), ::isspace),
                                        config_.local_mac.end());
            }

            if (config_.local_mac.empty())
            {
                print_error(fmt::format("failed to detect local mac for interface {}", config_.local_interface));
                return false;
            }
        }

        // detect remote mac if not specified
        if (config_.remote_mac.empty())
        {
            auto result = ssh_session_->execute(fmt::format("cat /sys/class/net/{}/address", config_.remote_interface));

            if (!result.has_value() || !result->success())
            {
                print_error(fmt::format("failed to detect remote mac for interface {}", config_.remote_interface));
                return false;
            }

            config_.remote_mac = result->stdout_output;
            config_.remote_mac.erase(std::remove_if(config_.remote_mac.begin(), config_.remote_mac.end(), ::isspace),
                                     config_.remote_mac.end());

            if (config_.remote_mac.empty())
            {
                print_error("remote mac detection returned empty");
                return false;
            }
        }

        if (config_.verbose)
        {
            print_status(fmt::format("local mac: {}", config_.local_mac));
            print_status(fmt::format("remote mac: {}", config_.remote_mac));
        }

        return true;
    }

    auto benchmark_orchestrator::run_latency_tests() -> std::vector<latency_result>
    {
        std::vector<latency_result> results;
        results.reserve(config_.payload_sizes.size());

        for (std::size_t i = 0; i < config_.payload_sizes.size() && g_running.load(); ++i)
        {
            print_progress("latency", i + 1, config_.payload_sizes.size());

            auto result = run_single_latency_test(config_.payload_sizes[i]);
            if (result.has_value())
            {
                results.push_back(*result);

                // print result
                fmt::print("  payload={:>5} bytes | rtt min/avg/max = {:>6.1f}/{:>6.1f}/{:>6.1f} us | "
                           "p99={:>6.1f} us | loss={:.2f}%\n",
                           result->payload_size, result->min_us, result->avg_us, result->max_us, result->p99_us,
                           result->loss_percent);
            }
            else
            {
                print_error(fmt::format("latency test failed for payload size {}", config_.payload_sizes[i]));
            }
        }

        return results;
    }

    auto benchmark_orchestrator::run_throughput_tests() -> std::vector<throughput_result>
    {
        std::vector<throughput_result> results;
        results.reserve(config_.payload_sizes.size());

        for (std::size_t i = 0; i < config_.payload_sizes.size() && g_running.load(); ++i)
        {
            print_progress("throughput", i + 1, config_.payload_sizes.size());

            auto result = run_single_throughput_test(config_.payload_sizes[i]);
            if (result.has_value())
            {
                results.push_back(*result);

                // print result
                fmt::print("  payload={:>5} bytes | {:>10.0f} pps | {:>8.2f} Mbps | {:>6.3f} Gbps\n",
                           result->payload_size, result->packets_per_sec, result->mbps, result->gbps);
            }
            else
            {
                print_error(fmt::format("throughput test failed for payload size {}", config_.payload_sizes[i]));
            }
        }

        return results;
    }

    auto benchmark_orchestrator::run_single_latency_test(std::size_t payload_size) -> std::optional<latency_result>
    {
        // kill any existing remote processes
        kill_remote_processes();

        // build vlan args if needed
        std::string vlan_args{};
        if (config_.use_vlan)
        {
            vlan_args = fmt::format(" --vlan {} --priority {}", config_.vlan_id, config_.vlan_priority);
        }

        // start echo server on remote
        auto server_cmd = fmt::format("sudo {} echo {} --timeout 30000{} &", config_.remote_binary_path,
                                      config_.remote_interface, vlan_args);

        if (config_.verbose)
        {
            print_status(fmt::format("starting remote server: {}", server_cmd));
        }

        // use nohup to keep server running
        auto start_result =
            ssh_session_->execute(fmt::format("nohup {} > /tmp/l2net_server.log 2>&1 & echo $!", server_cmd));

        if (!start_result.has_value())
        {
            print_error("failed to start remote server");
            return std::nullopt;
        }

        // give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});

        // verify server is running
        auto check_result = ssh_session_->execute("pgrep -f l2net_remote_node");
        if (!check_result.has_value() || check_result->stdout_output.empty())
        {
            print_error("remote server failed to start");

            // get server log for debugging
            auto log_result = ssh_session_->execute("cat /tmp/l2net_server.log 2>/dev/null");
            if (log_result.has_value() && !log_result->stdout_output.empty())
            {
                print_error(fmt::format("server log: {}", log_result->stdout_output));
            }
            return std::nullopt;
        }

        // run local ping client
        auto client_cmd = fmt::format("sudo {} ping {} --peer-mac {} --payload-size {} --count {} --quiet{}",
                                      config_.local_binary.string(), config_.local_interface, config_.remote_mac,
                                      payload_size, config_.packets_per_test, vlan_args);

        if (config_.verbose)
        {
            print_status(fmt::format("running local client: {}", client_cmd));
        }

        // execute local command
        std::array<char, 8192> buffer{};
        std::string output{};

        FILE *pipe = popen(client_cmd.c_str(), "r");
        if (pipe == nullptr)
        {
            print_error("failed to execute local client");
            kill_remote_processes();
            return std::nullopt;
        }

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        {
            output += buffer.data();
        }

        int const exit_code = pclose(pipe);

        // kill remote server
        kill_remote_processes();

        if (exit_code != 0 && output.empty())
        {
            print_error("local client failed with no output");
            return std::nullopt;
        }

        if (config_.verbose)
        {
            print_status(fmt::format("client output:\n{}", output));
        }

        return parse_latency_output(output, payload_size);
    }

    auto benchmark_orchestrator::run_single_throughput_test(std::size_t payload_size)
        -> std::optional<throughput_result>
    {
        // kill any existing remote processes
        kill_remote_processes();

        // build vlan args if needed
        std::string vlan_args{};
        if (config_.use_vlan)
        {
            vlan_args = fmt::format(" --vlan {} --priority {}", config_.vlan_id, config_.vlan_priority);
        }

        // start sink server on remote
        auto server_cmd = fmt::format("sudo {} sink {} --timeout 5000{}", config_.remote_binary_path,
                                      config_.remote_interface, vlan_args);

        if (config_.verbose)
        {
            print_status(fmt::format("starting remote sink: {}", server_cmd));
        }

        auto start_result =
            ssh_session_->execute(fmt::format("nohup {} > /tmp/l2net_server.log 2>&1 & echo $!", server_cmd));

        if (!start_result.has_value())
        {
            print_error("failed to start remote sink");
            return std::nullopt;
        }

        // give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds{500});

        // run local flood client
        auto client_cmd =
            fmt::format("sudo {} flood {} --peer-mac {} --payload-size {} --count {}{}", config_.local_binary.string(),
                        config_.local_interface, config_.remote_mac, payload_size, config_.packets_per_test, vlan_args);

        if (config_.verbose)
        {
            print_status(fmt::format("running local flood: {}", client_cmd));
        }

        std::array<char, 8192> buffer{};
        std::string output{};

        FILE *pipe = popen(client_cmd.c_str(), "r");
        if (pipe == nullptr)
        {
            print_error("failed to execute local flood client");
            kill_remote_processes();
            return std::nullopt;
        }

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        {
            output += buffer.data();
        }

        pclose(pipe);

        // kill remote server
        kill_remote_processes();

        if (config_.verbose)
        {
            print_status(fmt::format("flood output:\n{}", output));
        }

        return parse_throughput_output(output, payload_size);
    }

    auto benchmark_orchestrator::kill_remote_processes() -> void
    {
        // intentionally ignoring result - cleanup is best-effort
        (void)ssh_session_->execute("sudo pkill -9 -f l2net_remote_node 2>/dev/null || true");
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    auto benchmark_orchestrator::cleanup_remote() -> void
    {
        print_status("cleaning up remote...");
        kill_remote_processes();
        // intentionally ignoring results - cleanup is best-effort
        (void)ssh_session_->remove_file(config_.remote_binary_path);
        (void)ssh_session_->remove_file("/tmp/l2net_server.log");
    }

    auto benchmark_orchestrator::print_status(std::string_view msg) -> void
    {
        fmt::print(fmt::fg(fmt::color::green), "[*] {}\n", msg);
    }

    auto benchmark_orchestrator::print_error(std::string_view msg) -> void
    {
        fmt::print(fmt::fg(fmt::color::red), "[!] {}\n", msg);
    }

    auto benchmark_orchestrator::print_progress(std::string_view test_type, std::size_t current, std::size_t total)
        -> void
    {
        fmt::print(fmt::fg(fmt::color::cyan), "[{}/{}] {} test:\n", current, total, test_type);
    }

    // =============================================================================
    // result output
    // =============================================================================

    auto print_results_table(benchmark_results const &results) -> void
    {
        fmt::print("\n");
        fmt::print(fmt::fg(fmt::color::yellow),
                   "╔══════════════════════════════════════════════════════════════════════════════╗\n");
        fmt::print(fmt::fg(fmt::color::yellow),
                   "║                           BENCHMARK RESULTS                                  ║\n");
        fmt::print(fmt::fg(fmt::color::yellow),
                   "╚══════════════════════════════════════════════════════════════════════════════╝\n\n");

        fmt::print("Timestamp: {}\n", results.timestamp);
        fmt::print("Local:     {} ({}) - {}\n", results.local_host, results.local_interface, results.local_mac);
        fmt::print("Remote:    {} ({}) - {}\n\n", results.remote_host, results.remote_interface, results.remote_mac);

        // latency results
        if (!results.latency_results.empty())
        {
            fmt::print(fmt::fg(fmt::color::cyan),
                       "┌─ LATENCY RESULTS ─────────────────────────────────────────────────────────────┐\n");
            fmt::print("│ {:>8} │ {:>8} │ {:>8} │ {:>8} │ {:>8} │ {:>8} │ {:>6} │\n", "Payload", "Min(us)", "Avg(us)",
                       "Max(us)", "P50(us)", "P99(us)", "Loss%");
            fmt::print("├──────────┼──────────┼──────────┼──────────┼──────────┼──────────┼────────┤\n");

            for (auto const &r : results.latency_results)
            {
                fmt::print("│ {:>8} │ {:>8.1f} │ {:>8.1f} │ {:>8.1f} │ {:>8.1f} │ {:>8.1f} │ {:>6.2f} │\n",
                           r.payload_size, r.min_us, r.avg_us, r.max_us, r.p50_us, r.p99_us, r.loss_percent);
            }
            fmt::print("└──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴────────┘\n\n");
        }

        // throughput results
        if (!results.throughput_results.empty())
        {
            fmt::print(fmt::fg(fmt::color::cyan),
                       "┌─ THROUGHPUT RESULTS ──────────────────────────────────────────────────────────┐\n");
            fmt::print("│ {:>8} │ {:>12} │ {:>12} │ {:>10} │ {:>10} │\n", "Payload", "Packets/sec", "Mbps", "Gbps",
                       "Duration");
            fmt::print("├──────────┼──────────────┼──────────────┼────────────┼────────────┤\n");

            for (auto const &r : results.throughput_results)
            {
                fmt::print("│ {:>8} │ {:>12.0f} │ {:>12.2f} │ {:>10.3f} │ {:>8.0f}ms │\n", r.payload_size,
                           r.packets_per_sec, r.mbps, r.gbps, r.duration_ms);
            }
            fmt::print("└──────────┴──────────────┴──────────────┴────────────┴────────────┘\n");
        }
    }

    auto write_json_results(benchmark_results const &results, std::string const &filename) -> void
    {
        std::ofstream file{filename};
        if (!file.is_open())
        {
            fmt::print(stderr, "Failed to open {} for writing\n", filename);
            return;
        }

        file << "{\n";
        file << fmt::format("  \"timestamp\": \"{}\",\n", results.timestamp);
        file << fmt::format("  \"local_host\": \"{}\",\n", results.local_host);
        file << fmt::format("  \"remote_host\": \"{}\",\n", results.remote_host);
        file << fmt::format("  \"local_interface\": \"{}\",\n", results.local_interface);
        file << fmt::format("  \"remote_interface\": \"{}\",\n", results.remote_interface);
        file << fmt::format("  \"local_mac\": \"{}\",\n", results.local_mac);
        file << fmt::format("  \"remote_mac\": \"{}\",\n", results.remote_mac);

        // latency results
        file << "  \"latency_results\": [\n";
        for (std::size_t i = 0; i < results.latency_results.size(); ++i)
        {
            auto const &r = results.latency_results[i];
            file << "    {\n";
            file << fmt::format("      \"payload_size\": {},\n", r.payload_size);
            file << fmt::format("      \"packets_sent\": {},\n", r.packets_sent);
            file << fmt::format("      \"packets_received\": {},\n", r.packets_received);
            file << fmt::format("      \"loss_percent\": {:.4f},\n", r.loss_percent);
            file << fmt::format("      \"min_us\": {:.2f},\n", r.min_us);
            file << fmt::format("      \"avg_us\": {:.2f},\n", r.avg_us);
            file << fmt::format("      \"max_us\": {:.2f},\n", r.max_us);
            file << fmt::format("      \"p50_us\": {:.2f},\n", r.p50_us);
            file << fmt::format("      \"p95_us\": {:.2f},\n", r.p95_us);
            file << fmt::format("      \"p99_us\": {:.2f},\n", r.p99_us);
            file << fmt::format("      \"stddev_us\": {:.2f}\n", r.stddev_us);
            file << "    }" << (i < results.latency_results.size() - 1 ? "," : "") << "\n";
        }
        file << "  ],\n";

        // throughput results
        file << "  \"throughput_results\": [\n";
        for (std::size_t i = 0; i < results.throughput_results.size(); ++i)
        {
            auto const &r = results.throughput_results[i];
            file << "    {\n";
            file << fmt::format("      \"payload_size\": {},\n", r.payload_size);
            file << fmt::format("      \"packets_sent\": {},\n", r.packets_sent);
            file << fmt::format("      \"bytes_sent\": {},\n", r.bytes_sent);
            file << fmt::format("      \"duration_ms\": {:.2f},\n", r.duration_ms);
            file << fmt::format("      \"packets_per_sec\": {:.2f},\n", r.packets_per_sec);
            file << fmt::format("      \"mbps\": {:.4f},\n", r.mbps);
            file << fmt::format("      \"gbps\": {:.6f}\n", r.gbps);
            file << "    }" << (i < results.throughput_results.size() - 1 ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";

        fmt::print("Results written to {}\n", filename);
    }

    auto write_csv_results(benchmark_results const &results, std::string const &filename) -> void
    {
        // write latency csv
        {
            auto latency_file = filename + ".latency.csv";
            std::ofstream file{latency_file};
            if (file.is_open())
            {
                file << "payload_size,packets_sent,packets_received,loss_percent,min_us,avg_us,max_us,p50_us,p95_us,"
                        "p99_us,stddev_us\n";
                for (auto const &r : results.latency_results)
                {
                    file << fmt::format("{},{},{},{:.4f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}\n",
                                        r.payload_size, r.packets_sent, r.packets_received, r.loss_percent, r.min_us,
                                        r.avg_us, r.max_us, r.p50_us, r.p95_us, r.p99_us, r.stddev_us);
                }
                fmt::print("Latency results written to {}\n", latency_file);
            }
        }

        // write throughput csv
        {
            auto throughput_file = filename + ".throughput.csv";
            std::ofstream file{throughput_file};
            if (file.is_open())
            {
                file << "payload_size,packets_sent,bytes_sent,duration_ms,packets_per_sec,mbps,gbps\n";
                for (auto const &r : results.throughput_results)
                {
                    file << fmt::format("{},{},{},{:.2f},{:.2f},{:.4f},{:.6f}\n", r.payload_size, r.packets_sent,
                                        r.bytes_sent, r.duration_ms, r.packets_per_sec, r.mbps, r.gbps);
                }
                fmt::print("Throughput results written to {}\n", throughput_file);
            }
        }
    }

    // =============================================================================
    // command line parsing
    // =============================================================================

    auto print_usage(char const *program_name) -> void
    {
        fmt::print(stderr, R"(
Usage: {} [options]

Required:
  --remote-host <ip>        Remote host IP address
  --ssh-user <user>         SSH username
  --local-iface <iface>     Local network interface
  --remote-iface <iface>    Remote network interface
  --binary <path>           Path to local l2net_remote_node binary

Authentication (one required):
  --ssh-pass <pass>         SSH password
  --ssh-key <path>          Path to SSH private key

Optional:
  --ssh-port <port>         SSH port (default: 22)
  --local-mac <mac>         Local MAC address (auto-detected if not specified)
  --remote-mac <mac>        Remote MAC address (auto-detected if not specified)
  --payload-sizes <list>    Comma-separated payload sizes (default: 64,128,256,512,1024,1400,4096,8192)
  --packets <n>             Packets per test (default: 10000)
  --timeout <seconds>       Test timeout (default: 60)
  --vlan <id>               VLAN ID (optional)
  --priority <n>            VLAN priority 0-7 (default: 0)
  --output <file>           Output file prefix for results
  --json                    Output results as JSON
  --verbose                 Verbose output

Example:
  sudo {} \
    --remote-host 192.168.1.100 \
    --ssh-user admin \
    --ssh-pass secret123 \
    --local-iface eth0 \
    --remote-iface eth0 \
    --binary ./build/l2net_remote_node \
    --payload-sizes 64,256,1024,4096 \
    --packets 5000 \
    --output benchmark_results

)",
                   program_name, program_name);
    }

    [[nodiscard]] auto parse_args(int argc, char const *argv[]) -> std::optional<benchmark_config>
    {
        benchmark_config config;

        for (int i = 1; i < argc; ++i)
        {
            std::string_view const arg{argv[i]};

            if (arg == "--remote-host" && i + 1 < argc)
            {
                config.remote_host = argv[++i];
            }
            else if (arg == "--ssh-port" && i + 1 < argc)
            {
                config.ssh_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--ssh-user" && i + 1 < argc)
            {
                config.ssh_username = argv[++i];
            }
            else if (arg == "--ssh-pass" && i + 1 < argc)
            {
                config.ssh_password = argv[++i];
            }
            else if (arg == "--ssh-key" && i + 1 < argc)
            {
                config.ssh_key_path = argv[++i];
            }
            else if (arg == "--local-iface" && i + 1 < argc)
            {
                config.local_interface = argv[++i];
            }
            else if (arg == "--remote-iface" && i + 1 < argc)
            {
                config.remote_interface = argv[++i];
            }
            else if (arg == "--local-mac" && i + 1 < argc)
            {
                config.local_mac = argv[++i];
            }
            else if (arg == "--remote-mac" && i + 1 < argc)
            {
                config.remote_mac = argv[++i];
            }
            else if (arg == "--binary" && i + 1 < argc)
            {
                config.local_binary = argv[++i];
            }
            else if (arg == "--payload-sizes" && i + 1 < argc)
            {
                config.payload_sizes.clear();
                std::string sizes_str{argv[++i]};
                std::stringstream ss{sizes_str};
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    config.payload_sizes.push_back(std::stoull(token));
                }
            }
            else if (arg == "--packets" && i + 1 < argc)
            {
                config.packets_per_test = std::stoull(argv[++i]);
            }
            else if (arg == "--timeout" && i + 1 < argc)
            {
                config.test_timeout = std::chrono::seconds{std::stoul(argv[++i])};
            }
            else if (arg == "--vlan" && i + 1 < argc)
            {
                config.vlan_id = static_cast<std::uint16_t>(std::stoul(argv[++i]));
                config.use_vlan = true;
            }
            else if (arg == "--priority" && i + 1 < argc)
            {
                config.vlan_priority = static_cast<std::uint8_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--output" && i + 1 < argc)
            {
                config.output_file = argv[++i];
            }
            else if (arg == "--json")
            {
                config.json_output = true;
            }
            else if (arg == "--verbose")
            {
                config.verbose = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                return std::nullopt;
            }
            else
            {
                fmt::print(stderr, "Unknown argument: {}\n", arg);
                return std::nullopt;
            }
        }

        // validate required fields
        if (config.remote_host.empty())
        {
            fmt::print(stderr, "Error: --remote-host is required\n");
            return std::nullopt;
        }
        if (config.ssh_username.empty())
        {
            fmt::print(stderr, "Error: --ssh-user is required\n");
            return std::nullopt;
        }
        if (config.ssh_password.empty() && config.ssh_key_path.empty())
        {
            fmt::print(stderr, "Error: --ssh-pass or --ssh-key is required\n");
            return std::nullopt;
        }
        if (config.local_interface.empty())
        {
            fmt::print(stderr, "Error: --local-iface is required\n");
            return std::nullopt;
        }
        if (config.remote_interface.empty())
        {
            fmt::print(stderr, "Error: --remote-iface is required\n");
            return std::nullopt;
        }
        if (config.local_binary.empty())
        {
            fmt::print(stderr, "Error: --binary is required\n");
            return std::nullopt;
        }

        return config;
    }

} // anonymous namespace

auto main(int argc, char const *argv[]) -> int
{
    auto config_opt = parse_args(argc, argv);
    if (!config_opt.has_value())
    {
        print_usage(argv[0]);
        return 1;
    }

    benchmark_orchestrator orchestrator{std::move(*config_opt)};
    auto results = orchestrator.run();

    if (!results.has_value())
    {
        fmt::print(stderr, "Benchmark failed\n");
        return 1;
    }

    // print results
    print_results_table(*results);

    // write output files if requested
    if (!config_opt->output_file.empty())
    {
        if (config_opt->json_output)
        {
            write_json_results(*results, config_opt->output_file + ".json");
        }
        write_csv_results(*results, config_opt->output_file);
    }

    return 0;
}
