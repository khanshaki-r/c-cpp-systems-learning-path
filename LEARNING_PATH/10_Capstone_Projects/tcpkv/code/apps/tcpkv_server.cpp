// tcpkv_server: the daemon. Parses flags, installs signal handlers, runs the Server.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "tcpkv/log.hpp"
#include "tcpkv/metrics.hpp"
#include "tcpkv/server.hpp"
#include "tcpkv/store.hpp"

namespace {

// The only global: the signal handler needs to find the server. An atomic pointer load is
// async-signal-safe (lock-free atomics are allowed in handlers).
std::atomic<tcpkv::Server*> g_server{nullptr};

void on_signal(int /*signum*/) {
    if (tcpkv::Server* server = g_server.load(std::memory_order_acquire)) {
        server->request_stop();  // atomic store + write(2): both async-signal-safe
    }
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: let poll() return EINTR so loops re-check the flag
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    // Belt and braces: Socket::write_all already uses MSG_NOSIGNAL, but any other write to a
    // closed socket would otherwise kill the process.
    std::signal(SIGPIPE, SIG_IGN);
}

struct Options {
    tcpkv::ServerConfig server;
    tcpkv::log::Level log_level = tcpkv::log::Level::Info;
    bool help = false;
};

template <class T>
bool parse_number(std::string_view text, T& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

void print_usage(std::ostream& os) {
    os << "usage: tcpkv_server [options]\n"
          "  -p, --port <n>           TCP port to listen on (default 7379)\n"
          "      --host <addr>        bind address (default 0.0.0.0)\n"
          "  -t, --threads <n>        worker threads (default: hardware concurrency)\n"
          "      --idle-timeout <s>   close idle connections after s seconds (default 0 = never)\n"
          "      --max-pending <n>    queued connections before replying 'server busy' (default 1024)\n"
          "      --stats-interval <s> log throughput every s seconds (default 10, 0 = off)\n"
          "      --log-level <lvl>    debug | info | warn | error (default info)\n"
          "  -h, --help               show this help\n";
}

bool parse_args(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.help = true;
            return true;
        }
        if (i + 1 >= argc) {
            error = "missing value for " + std::string(arg);
            return false;
        }
        const std::string_view value = argv[++i];

        if (arg == "--port" || arg == "-p") {
            unsigned port = 0;
            if (!parse_number(value, port) || port > 65535) {
                error = "invalid port";
                return false;
            }
            options.server.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--threads" || arg == "-t") {
            unsigned threads = 0;
            if (!parse_number(value, threads) || threads == 0 || threads > 1024) {
                error = "invalid thread count (1..1024)";
                return false;
            }
            options.server.threads = threads;
        } else if (arg == "--host") {
            options.server.host = std::string(value);
        } else if (arg == "--idle-timeout") {
            unsigned seconds = 0;
            if (!parse_number(value, seconds)) {
                error = "invalid idle timeout";
                return false;
            }
            options.server.connection.idle_timeout = std::chrono::seconds(seconds);
        } else if (arg == "--max-pending") {
            unsigned pending = 0;
            if (!parse_number(value, pending)) {
                error = "invalid max-pending";
                return false;
            }
            options.server.max_pending_connections = pending;
        } else if (arg == "--stats-interval") {
            unsigned seconds = 0;
            if (!parse_number(value, seconds)) {
                error = "invalid stats interval";
                return false;
            }
            options.server.stats_log_interval = std::chrono::seconds(seconds);
        } else if (arg == "--log-level") {
            if (!tcpkv::log::parse_level(value, options.log_level)) {
                error = "invalid log level";
                return false;
            }
        } else {
            error = "unknown option " + std::string(arg);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.server.threads = std::max(1u, std::thread::hardware_concurrency());

    std::string error;
    if (!parse_args(argc, argv, options, error)) {
        std::cerr << "error: " << error << "\n\n";
        print_usage(std::cerr);
        return 2;
    }
    if (options.help) {
        print_usage(std::cout);
        return 0;
    }

    tcpkv::log::set_level(options.log_level);

    try {
        tcpkv::Store store;
        tcpkv::Metrics metrics;
        tcpkv::Server server(options.server, store, metrics);

        g_server.store(&server, std::memory_order_release);
        install_signal_handlers();

        server.run();

        g_server.store(nullptr, std::memory_order_release);
        tcpkv::log::info("tcpkv stopped cleanly; ", metrics.commands_total.load(), " commands served");
        return 0;
    } catch (const std::exception& e) {
        tcpkv::log::error("fatal: ", e.what());
        return 1;
    }
}
