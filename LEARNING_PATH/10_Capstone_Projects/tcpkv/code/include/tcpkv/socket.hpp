#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "tcpkv/fd.hpp"

namespace tcpkv {

// A TCP socket built on FileDescriptor. Move-only, like the descriptor it owns.
//
// Every blocking call retries on EINTR. write_all() loops until every byte is
// handed to the kernel, because send() on a stream socket may accept fewer bytes
// than asked. All failures are reported as std::system_error carrying errno.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    ~Socket() = default;

    // Resolves `host` (empty means "any address"), binds, listens. `port` 0 picks a free port.
    static Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog);

    // Resolves and connects. Tries every address returned by getaddrinfo in order.
    static Socket connect_tcp(const std::string& host, std::uint16_t port);

    // Blocks until a client connects (unless the listener is non-blocking, in which case
    // it throws std::system_error with EAGAIN when nothing is pending).
    Socket accept();

    // Returns the number of bytes read; 0 means the peer closed its side.
    std::size_t read_some(char* buf, std::size_t len);

    // Writes all bytes or throws. Uses MSG_NOSIGNAL so a dead peer gives EPIPE, not SIGPIPE.
    void write_all(const char* data, std::size_t len);
    void write_all(std::string_view data) { write_all(data.data(), data.size()); }

    // Waits up to `timeout` for the socket to become readable (data, EOF or error).
    // Returns false on timeout. EINTR is reported as a timeout so callers re-check their state.
    bool wait_readable(std::chrono::milliseconds timeout);

    // shutdown(SHUT_RDWR). Errors (e.g. ENOTCONN) are ignored: this is a courtesy to the peer.
    void shutdown_both() noexcept;

    std::uint16_t local_port() const;
    std::string peer_address() const noexcept;  // "ip:port", or "?" if it cannot be determined

    void set_nodelay(bool enabled);
    void set_nonblocking(bool enabled);

    int fd() const noexcept { return fd_.get(); }
    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

private:
    FileDescriptor fd_;
};

}  // namespace tcpkv
