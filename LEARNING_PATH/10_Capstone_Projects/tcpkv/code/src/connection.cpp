#include "tcpkv/connection.hpp"

#include <charconv>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "tcpkv/log.hpp"

namespace tcpkv {

namespace {

// Increments connections_active for the lifetime of serve(), whatever path leaves it.
class ActiveConnectionGuard {
public:
    explicit ActiveConnectionGuard(Metrics& metrics) noexcept : metrics_(metrics) {
        metrics_.connections_active.fetch_add(1, std::memory_order_relaxed);
    }
    ~ActiveConnectionGuard() { metrics_.connections_active.fetch_sub(1, std::memory_order_relaxed); }
    ActiveConnectionGuard(const ActiveConnectionGuard&) = delete;
    ActiveConnectionGuard& operator=(const ActiveConnectionGuard&) = delete;

private:
    Metrics& metrics_;
};

long long parse_seconds(const std::string& text) {
    long long value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    // The parser already validated this token; this guard only protects against misuse of
    // execute_command() from code that bypassed the parser (e.g. a test).
    return (result.ec == std::errc() && result.ptr == end) ? value : 0;
}

}  // namespace

std::string execute_command(Command command, Store& store, Metrics& metrics) {
    switch (command.type) {
        case CommandType::Ping:
            return reply::status("PONG");

        case CommandType::Get: {
            const std::optional<std::string> value = store.get(command.args[0]);
            return value ? reply::bulk(*value) : reply::null();
        }

        case CommandType::Set:
            store.set(std::move(command.args[0]), std::move(command.value));
            return reply::status("OK");

        case CommandType::Del: {
            long long removed = 0;
            for (const std::string& key : command.args) {
                removed += store.del(key) ? 1 : 0;
            }
            return reply::integer(removed);
        }

        case CommandType::Exists: {
            long long present = 0;
            for (const std::string& key : command.args) {
                present += store.exists(key) ? 1 : 0;
            }
            return reply::integer(present);
        }

        case CommandType::Keys:
            return reply::array(store.keys(command.args.empty() ? std::string_view{}
                                                                : std::string_view{command.args[0]}));

        case CommandType::Expire: {
            const auto ttl = std::chrono::seconds(parse_seconds(command.args[1]));
            return reply::integer(store.expire(command.args[0], ttl) ? 1 : 0);
        }

        case CommandType::Ttl:
            return reply::integer(store.ttl(command.args[0]));

        case CommandType::Stats:
            return reply::bulk(metrics.render());

        case CommandType::Quit:
            return reply::status("BYE");
    }
    return reply::error("internal error: unhandled command");  // unreachable; keeps -Wreturn-type quiet
}

Connection::Connection(Socket socket, Store& store, Metrics& metrics,
                       const std::atomic<bool>& stop_flag, ConnectionConfig config) noexcept
    : socket_(std::move(socket)),
      store_(&store),
      metrics_(&metrics),
      stop_(&stop_flag),
      config_(config) {}

void Connection::serve() {
    using Clock = std::chrono::steady_clock;

    const ActiveConnectionGuard active_guard(*metrics_);
    const std::string peer = socket_.peer_address();
    log::debug("connection from ", peer);

    Parser parser;
    std::vector<char> buffer(config_.read_buffer_size);
    std::string out;
    auto last_activity = Clock::now();

    try {
        while (!stop_->load(std::memory_order_relaxed)) {
            if (!socket_.wait_readable(config_.poll_interval)) {
                if (config_.idle_timeout.count() > 0 &&
                    Clock::now() - last_activity >= config_.idle_timeout) {
                    log::debug("connection ", peer, " idle timeout");
                    break;
                }
                continue;  // timed out: loop re-checks the stop flag
            }

            const std::size_t n = socket_.read_some(buffer.data(), buffer.size());
            if (n == 0) {
                break;  // peer closed its side
            }
            last_activity = Clock::now();
            metrics_->bytes_in.fetch_add(n, std::memory_order_relaxed);
            parser.feed(buffer.data(), n);

            out.clear();
            const bool keep_open = process_events(parser, out);
            if (!out.empty()) {
                // One write for every reply produced by this read: pipelined clients get
                // one segment back instead of one per command.
                socket_.write_all(out);
                metrics_->bytes_out.fetch_add(out.size(), std::memory_order_relaxed);
            }
            if (!keep_open) {
                break;
            }
        }
    } catch (const std::system_error& e) {
        // ECONNRESET and EPIPE are normal here: the client went away mid-conversation.
        log::debug("connection ", peer, " error: ", e.what());
    }

    socket_.shutdown_both();
    log::debug("connection ", peer, " closed");
}

bool Connection::process_events(Parser& parser, std::string& out) {
    using Clock = std::chrono::steady_clock;

    while (std::optional<ParseEvent> event = parser.next()) {
        if (const ParseError* error = std::get_if<ParseError>(&*event)) {
            metrics_->commands_failed.fetch_add(1, std::memory_order_relaxed);
            out += reply::error(error->message);
            if (error->fatal) {
                return false;
            }
            continue;
        }

        Command& command = std::get<Command>(*event);
        const bool quit = command.type == CommandType::Quit;
        const auto start = Clock::now();
        out += execute_command(std::move(command), *store_, *metrics_);
        metrics_->command_latency.record(Clock::now() - start);
        metrics_->commands_total.fetch_add(1, std::memory_order_relaxed);
        if (quit) {
            return false;
        }
    }
    return true;
}

}  // namespace tcpkv
