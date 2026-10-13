#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "tcpkv/connection.hpp"
#include "tcpkv/fd.hpp"
#include "tcpkv/metrics.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"
#include "tcpkv/thread_pool.hpp"

namespace tcpkv {

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 7379;  // 0 = let the kernel choose (tests use this)
    std::size_t threads = 4;
    int backlog = 128;
    // Connections accepted while every worker is busy wait in the pool queue. Beyond this
    // many waiting, new connections get "-ERR server busy" and are closed immediately.
    std::size_t max_pending_connections = 1024;
    ConnectionConfig connection;
    // Log a one-line throughput summary this often. 0 disables.
    std::chrono::seconds stats_log_interval{10};
};

// Acceptor + fixed thread pool.
//
//   run() thread:        poll(listener, wake_pipe) -> accept -> pool.submit(Connection)
//   worker threads:      Connection::serve() until the client leaves or stop is requested
//   any thread/signal:   request_stop() sets the flag and writes to the wake pipe
//
// The constructor binds and listens, so port() is valid immediately after construction.
class Server {
public:
    Server(ServerConfig config, Store& store, Metrics& metrics);
    ~Server() = default;

    // Not movable: a signal handler may hold a pointer to this object.
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Blocks until request_stop() is called. Workers are joined before it returns.
    void run();

    // Async-signal-safe: an atomic store and one write(2) on a non-blocking pipe.
    void request_stop() noexcept;
    bool stop_requested() const noexcept { return stop_.load(std::memory_order_acquire); }

    std::uint16_t port() const { return listener_.local_port(); }

private:
    void accept_one(ThreadPool& pool);
    void log_throughput(std::chrono::steady_clock::time_point now,
                        std::chrono::steady_clock::time_point& last_time,
                        std::uint64_t& last_commands) const;

    ServerConfig config_;
    Store& store_;
    Metrics& metrics_;
    Socket listener_;
    FileDescriptor wake_read_;
    FileDescriptor wake_write_;
    std::atomic<bool> stop_{false};
};

}  // namespace tcpkv
