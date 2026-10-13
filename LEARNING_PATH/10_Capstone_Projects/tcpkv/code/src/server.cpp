#include "tcpkv/server.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <thread>
#include <utility>

#include "tcpkv/log.hpp"

namespace tcpkv {

Server::Server(ServerConfig config, Store& store, Metrics& metrics)
    : config_(std::move(config)),
      store_(store),
      metrics_(metrics),
      listener_(Socket::listen_tcp(config_.host, config_.port, config_.backlog)) {
    // Non-blocking listener: poll() can report readable for a connection that is reset before
    // we call accept(); with a blocking socket that accept() would hang the acceptor thread.
    listener_.set_nonblocking(true);

    std::pair<FileDescriptor, FileDescriptor> wake_pipe = make_pipe();
    wake_read_ = std::move(wake_pipe.first);
    wake_write_ = std::move(wake_pipe.second);
    // The write end must never block: request_stop() may run inside a signal handler.
    set_nonblocking(wake_write_.get(), true);
    set_nonblocking(wake_read_.get(), true);
}

void Server::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    const char byte = 1;
    // write(2) is on the async-signal-safe list. If the pipe is full (stop requested many
    // times) the write fails with EAGAIN, which is fine: one byte is enough to wake poll().
    const ssize_t rc = ::write(wake_write_.get(), &byte, 1);
    (void)rc;
}

void Server::run() {
    using Clock = std::chrono::steady_clock;

    ThreadPool pool(config_.threads);

    pollfd fds[2]{};
    fds[0].fd = listener_.fd();
    fds[0].events = POLLIN;
    fds[1].fd = wake_read_.get();
    fds[1].events = POLLIN;

    auto last_purge = Clock::now();
    auto last_stats = last_purge;
    std::uint64_t last_commands = metrics_.commands_total.load(std::memory_order_relaxed);

    log::info("tcpkv listening on ", config_.host, ":", port(), " with ", pool.size(),
              " worker threads");

    while (!stop_requested()) {
        const int rc = ::poll(fds, 2, 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;  // a signal arrived; the loop condition sees the stop flag
            }
            throw_system_error(errno, "poll(listener)");
        }
        if (rc > 0 && (fds[0].revents & POLLIN) != 0) {
            accept_one(pool);
        }
        // fds[1] (the wake pipe) needs no handling: stop_ is already set when it fires.

        const auto now = Clock::now();
        if (now - last_purge >= std::chrono::seconds(1)) {
            const std::size_t purged = store_.purge_expired();
            if (purged > 0) {
                log::debug("purged ", purged, " expired keys");
            }
            last_purge = now;
        }
        if (config_.stats_log_interval.count() > 0 && now - last_stats >= config_.stats_log_interval) {
            log_throughput(now, last_stats, last_commands);
        }
    }

    log::info("shutdown requested: ", metrics_.connections_active.load(std::memory_order_relaxed),
              " active connections, ", pool.pending(), " queued");
    // Leaving this scope destroys `pool`: queued connections are dropped (their sockets close),
    // workers return as soon as their Connection::serve() loop observes stop_, then are joined.
}

void Server::accept_one(ThreadPool& pool) {
    Socket client;
    try {
        client = listener_.accept();
    } catch (const std::system_error& e) {
        const int code = e.code().value();
        if (code == EAGAIN || code == EWOULDBLOCK || code == ECONNABORTED) {
            return;  // nothing to accept after all
        }
        if (code == EMFILE || code == ENFILE || code == ENOBUFS || code == ENOMEM) {
            // Out of descriptors or memory. Back off briefly; closing a connection frees one.
            log::warn("accept failed (", e.what(), "); backing off 100 ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }
        throw;  // anything else is a bug or a dead listener
    }

    metrics_.connections_accepted.fetch_add(1, std::memory_order_relaxed);

    if (pool.pending() >= config_.max_pending_connections) {
        metrics_.connections_rejected.fetch_add(1, std::memory_order_relaxed);
        log::warn("rejecting ", client.peer_address(), ": ", pool.pending(), " connections queued");
        try {
            client.write_all(reply::error("server busy, try again later"));
        } catch (const std::system_error&) {
            // The client is gone; nothing to do.
        }
        return;  // `client` closes here
    }

    Connection connection(std::move(client), store_, metrics_, stop_, config_.connection);
    if (!pool.submit([conn = std::move(connection)]() mutable { conn.serve(); })) {
        log::warn("pool stopped; dropping new connection");
    }
}

void Server::log_throughput(std::chrono::steady_clock::time_point now,
                            std::chrono::steady_clock::time_point& last_time,
                            std::uint64_t& last_commands) const {
    const std::uint64_t commands = metrics_.commands_total.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    const std::uint64_t delta = commands - last_commands;
    const auto rate = seconds > 0 ? static_cast<std::uint64_t>(static_cast<double>(delta) / seconds) : 0;
    log::info("stats: ", delta, " commands in last ", static_cast<std::uint64_t>(seconds), " s (",
              rate, " ops/s), active connections ",
              metrics_.connections_active.load(std::memory_order_relaxed), ", p99 ",
              metrics_.command_latency.snapshot().percentile_us(0.99), " us");
    last_time = now;
    last_commands = commands;
}

}  // namespace tcpkv
