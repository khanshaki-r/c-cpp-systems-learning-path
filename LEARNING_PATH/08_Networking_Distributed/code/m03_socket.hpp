// m03_socket.hpp - small RAII and helper layer over POSIX sockets.
// Used by modules 03 to 08. Header-only. C++17. Linux / POSIX only.
//
// Design rules:
//  * Every fd is owned by exactly one net::Fd. Destructor closes. No leaks => no CLOSE_WAIT piles.
//  * Every syscall wrapper retries EINTR, because a signal (SIGCHLD, profiler, debugger) can
//    interrupt a blocking call at any time and that is not an error.
//  * Writes use send(MSG_NOSIGNAL) so a dead peer gives EPIPE instead of killing the process.
//  * Errors become std::system_error carrying errno, so callers can inspect .code().value().
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace net {

// ---------------------------------------------------------------------------------------------
// Fd: move-only owner of a file descriptor.
// ---------------------------------------------------------------------------------------------
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // Give up ownership without closing (e.g. handing the fd to another owner).
    int release() noexcept {
        int f = fd_;
        fd_ = -1;
        return f;
    }

    // Close the current fd (if any) and take over `fd`.
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            // close() can fail with EINTR/EIO; the fd is released anyway on Linux, so do not retry.
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------
[[noreturn]] inline void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

[[noreturn]] inline void throw_gai(int rc, const char* what) {
    throw std::runtime_error(std::string(what) + ": " + ::gai_strerror(rc));
}

// Servers must never die from SIGPIPE. We use MSG_NOSIGNAL on send(), but third-party code
// (or write() on a socket) may not, so ignore the signal process-wide as well.
inline void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

// ---------------------------------------------------------------------------------------------
// Socket options
// ---------------------------------------------------------------------------------------------
inline void set_reuseaddr(int fd) {
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) throw_errno("SO_REUSEADDR");
}

inline void set_nodelay(int fd) {
    int one = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0) throw_errno("TCP_NODELAY");
}

inline void set_keepalive(int fd, int idle_s, int intvl_s, int cnt) {
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) < 0) throw_errno("SO_KEEPALIVE");
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s) < 0) throw_errno("TCP_KEEPIDLE");
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof intvl_s) < 0) throw_errno("TCP_KEEPINTVL");
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt) < 0) throw_errno("TCP_KEEPCNT");
}

inline void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) throw_errno("fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw_errno("fcntl(F_SETFL)");
}

// Blocking recv()/send() give up after `ms` milliseconds with EAGAIN (reported as EWOULDBLOCK).
inline void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) throw_errno("SO_RCVTIMEO");
}

inline void set_send_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) throw_errno("SO_SNDTIMEO");
}

// ---------------------------------------------------------------------------------------------
// Address helpers
// ---------------------------------------------------------------------------------------------
struct AddrInfoList {
    addrinfo* head = nullptr;
    ~AddrInfoList() {
        if (head) ::freeaddrinfo(head);
    }
    AddrInfoList() = default;
    AddrInfoList(const AddrInfoList&) = delete;
    AddrInfoList& operator=(const AddrInfoList&) = delete;
};

// host may be nullptr: for listening that means "all interfaces" (with AI_PASSIVE).
inline void resolve(const char* host, const char* port, bool passive, AddrInfoList& out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6, whatever getaddrinfo returns first
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    int rc = ::getaddrinfo(host, port, &hints, &out.head);
    if (rc != 0) throw_gai(rc, "getaddrinfo");
}

// Human-readable "ip:port" of the peer, for logs.
inline std::string peer_name(int fd) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) < 0) return "?";
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    int rc = ::getnameinfo(reinterpret_cast<sockaddr*>(&ss), len, host, sizeof host, serv, sizeof serv,
                           NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) return "?";
    return std::string(host) + ":" + serv;
}

// ---------------------------------------------------------------------------------------------
// Create a listening TCP socket. Tries every address getaddrinfo returns until one binds.
// ---------------------------------------------------------------------------------------------
inline Fd listen_tcp(const char* host, const char* port, int backlog = 128) {
    AddrInfoList list;
    resolve(host, port, /*passive=*/true, list);
    int last_errno = 0;
    for (addrinfo* ai = list.head; ai; ai = ai->ai_next) {
        Fd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd) {
            last_errno = errno;
            continue;
        }
        // Without SO_REUSEADDR a restart within 60 s fails with EADDRINUSE because old
        // connections to this port are still in TIME_WAIT.
        set_reuseaddr(fd.get());
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) < 0) {
            last_errno = errno;
            continue;
        }
        if (::listen(fd.get(), backlog) < 0) {
            last_errno = errno;
            continue;
        }
        return fd;
    }
    errno = last_errno ? last_errno : EADDRNOTAVAIL;
    throw_errno("listen_tcp");
}

// Connect to host:port, trying each resolved address in order (IPv6 then IPv4, or whatever
// the resolver returns). Returns a connected blocking socket.
inline Fd connect_tcp(const char* host, const char* port) {
    AddrInfoList list;
    resolve(host, port, /*passive=*/false, list);
    int last_errno = 0;
    for (addrinfo* ai = list.head; ai; ai = ai->ai_next) {
        Fd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd) {
            last_errno = errno;
            continue;
        }
        int rc;
        do {
            rc = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
        } while (rc < 0 && errno == EINTR);
        if (rc == 0) return fd;
        last_errno = errno;
    }
    errno = last_errno ? last_errno : ECONNREFUSED;
    throw_errno("connect_tcp");
}

// accept() with EINTR retry. Returns an invalid Fd if the listener was closed (EBADF/EINVAL),
// so a shutdown sequence that closes the listening socket ends the accept loop cleanly.
inline Fd accept_tcp(int listen_fd) {
    for (;;) {
        int c = ::accept(listen_fd, nullptr, nullptr);
        if (c >= 0) return Fd(c);
        if (errno == EINTR) continue;
        if (errno == ECONNABORTED) continue; // client gave up while in the accept queue; not fatal
        if (errno == EBADF || errno == EINVAL) return Fd();
        throw_errno("accept");
    }
}

// ---------------------------------------------------------------------------------------------
// I/O. THE read loop and THE write loop. Learn these; you will write them for the rest of
// your career in one form or another.
// ---------------------------------------------------------------------------------------------

// One recv(). Returns >0 bytes, 0 on EOF (peer sent FIN). Throws on error.
// With SO_RCVTIMEO set, a timeout surfaces as std::system_error with EAGAIN/EWOULDBLOCK.
inline size_t read_some(int fd, void* buf, size_t len) {
    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        throw_errno("recv");
    }
}

// Read exactly `len` bytes. Returns false if EOF arrives before the FIRST byte (clean close
// between messages). Throws if EOF arrives in the middle (truncated message = protocol error).
inline bool read_exact(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        size_t n = read_some(fd, p + got, len - got);
        if (n == 0) {
            if (got == 0) return false;
            throw std::runtime_error("read_exact: connection closed mid-message");
        }
        got += n;
    }
    return true;
}

// Write all bytes. A blocking send() may still return a short count (signal after partial
// progress, or SO_SNDTIMEO), so loop. MSG_NOSIGNAL: a reset peer gives EPIPE, not SIGPIPE.
inline void write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("send");
        }
        sent += static_cast<size_t>(n);
    }
}

inline void write_all(int fd, std::string_view s) { write_all(fd, s.data(), s.size()); }

// Wait until fd is readable or `timeout_ms` passes. Returns true if readable, false on timeout.
// Throws on error. Retries EINTR (recomputing the remaining time is left as an exercise).
inline bool wait_readable(int fd, int timeout_ms) {
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    for (;;) {
        int rc = ::poll(&p, 1, timeout_ms);
        if (rc > 0) return true;
        if (rc == 0) return false;
        if (errno == EINTR) continue;
        throw_errno("poll");
    }
}

} // namespace net
