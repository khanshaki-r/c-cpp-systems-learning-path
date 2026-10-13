#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

#include "tcpkv/metrics.hpp"
#include "tcpkv/protocol.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

namespace tcpkv {

struct ConnectionConfig {
    // How often a blocked handler wakes up to check the shutdown flag. Bounds shutdown latency.
    std::chrono::milliseconds poll_interval{250};
    // Close a connection that has sent nothing for this long. 0 disables the idle timeout.
    std::chrono::seconds idle_timeout{0};
    std::size_t read_buffer_size = 16 * 1024;
};

// Runs one parsed command against the store and returns the encoded reply.
// Takes the Command by value so SET can move the key and value into the store without copying.
// Free function so it can be unit-tested without a socket.
std::string execute_command(Command command, Store& store, Metrics& metrics);

// One client connection, served by one worker thread for its whole lifetime.
//
// Move-only: it owns the Socket. Dependencies are held as pointers (not references) so the
// class stays move-assignable; a moved-from Connection holds an invalid socket and must not be
// served.
class Connection {
public:
    Connection(Socket socket, Store& store, Metrics& metrics, const std::atomic<bool>& stop_flag,
               ConnectionConfig config) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    ~Connection() = default;

    // Blocks until the peer disconnects, sends QUIT, triggers a fatal protocol error,
    // exceeds the idle timeout, or the server asks everyone to stop.
    void serve();

private:
    // Drains every complete event from the parser into `out`. Returns false when the
    // connection must close after `out` has been sent.
    bool process_events(Parser& parser, std::string& out);

    Socket socket_;
    Store* store_;
    Metrics* metrics_;
    const std::atomic<bool>* stop_;
    ConnectionConfig config_;
};

}  // namespace tcpkv
