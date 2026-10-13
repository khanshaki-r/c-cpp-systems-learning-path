#include "tcpkv/socket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tcpkv {

namespace {

// RAII for the linked list returned by getaddrinfo(). Move-only: a copy would free twice.
struct AddrInfoList {
    addrinfo* head = nullptr;

    AddrInfoList() = default;
    AddrInfoList(const AddrInfoList&) = delete;
    AddrInfoList& operator=(const AddrInfoList&) = delete;
    AddrInfoList(AddrInfoList&& other) noexcept : head(other.head) { other.head = nullptr; }
    AddrInfoList& operator=(AddrInfoList&&) = delete;
    ~AddrInfoList() {
        if (head != nullptr) {
            ::freeaddrinfo(head);
        }
    }
};

AddrInfoList resolve(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);

    const std::string service = std::to_string(port);
    AddrInfoList list;
    const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                                 &list.head);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo(") + host + "): " + ::gai_strerror(rc));
    }
    return list;
}

// Completes a connect() that was interrupted by a signal. The kernel keeps the handshake
// going; we wait for writability and then read the final status from SO_ERROR.
void finish_interrupted_connect(int fd) {
    for (;;) {
        pollfd p{};
        p.fd = fd;
        p.events = POLLOUT;
        const int rc = ::poll(&p, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error(errno, "poll(connect)");
        }
        break;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        throw_system_error(errno, "getsockopt(SO_ERROR)");
    }
    if (err != 0) {
        throw_system_error(err, "connect");
    }
}

}  // namespace

Socket Socket::listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
    const AddrInfoList list = resolve(host, port, /*passive=*/true);
    int last_error = 0;
    for (const addrinfo* ai = list.head; ai != nullptr; ai = ai->ai_next) {
        FileDescriptor fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd) {
            last_error = errno;
            continue;
        }
        // Without SO_REUSEADDR a restart within ~60 s fails with EADDRINUSE while old
        // connections sit in TIME_WAIT. Every real server sets this.
        const int one = 1;
        if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
            last_error = errno;
            continue;
        }
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0) {
            last_error = errno;
            continue;
        }
        if (::listen(fd.get(), backlog) != 0) {
            last_error = errno;
            continue;
        }
        return Socket(std::move(fd));
    }
    throw_system_error(last_error != 0 ? last_error : EADDRNOTAVAIL, "listen_tcp");
}

Socket Socket::connect_tcp(const std::string& host, std::uint16_t port) {
    const AddrInfoList list = resolve(host, port, /*passive=*/false);
    int last_error = 0;
    for (const addrinfo* ai = list.head; ai != nullptr; ai = ai->ai_next) {
        FileDescriptor fd(::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
        if (!fd) {
            last_error = errno;
            continue;
        }
        if (::connect(fd.get(), ai->ai_addr, ai->ai_addrlen) == 0) {
            return Socket(std::move(fd));
        }
        if (errno == EINTR) {
            finish_interrupted_connect(fd.get());
            return Socket(std::move(fd));
        }
        last_error = errno;
    }
    throw_system_error(last_error != 0 ? last_error : EHOSTUNREACH, "connect_tcp");
}

Socket Socket::accept() {
    for (;;) {
        // accept4 lets us set CLOEXEC atomically, so a concurrently fork()ed child never
        // inherits the client socket.
        const int fd = ::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) {
            return Socket(FileDescriptor(fd));
        }
        if (errno == EINTR) {
            continue;
        }
        throw_system_error(errno, "accept");
    }
}

std::size_t Socket::read_some(char* buf, std::size_t len) {
    for (;;) {
        const ssize_t n = ::read(fd_.get(), buf, len);
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        throw_system_error(errno, "read");
    }
}

void Socket::write_all(const char* data, std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ::send(fd_.get(), data + offset, len - offset, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error(errno, "send");
        }
        offset += static_cast<std::size_t>(n);
    }
}

bool Socket::wait_readable(std::chrono::milliseconds timeout) {
    pollfd p{};
    p.fd = fd_.get();
    p.events = POLLIN;
    const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (rc < 0) {
        if (errno == EINTR) {
            return false;
        }
        throw_system_error(errno, "poll");
    }
    // POLLIN, POLLHUP and POLLERR all mean "call read() now": it will return data, 0 or an error.
    return rc > 0;
}

void Socket::shutdown_both() noexcept {
    if (fd_.valid()) {
        ::shutdown(fd_.get(), SHUT_RDWR);
    }
}

std::uint16_t Socket::local_port() const {
    sockaddr_storage storage{};
    socklen_t len = sizeof storage;
    if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
        throw_system_error(errno, "getsockname");
    }
    // memcpy instead of a pointer cast: sockaddr_storage and sockaddr_in are different types
    // and reading one through a pointer to the other is undefined behaviour.
    if (storage.ss_family == AF_INET) {
        sockaddr_in in4{};
        std::memcpy(&in4, &storage, sizeof in4);
        return ntohs(in4.sin_port);
    }
    if (storage.ss_family == AF_INET6) {
        sockaddr_in6 in6{};
        std::memcpy(&in6, &storage, sizeof in6);
        return ntohs(in6.sin6_port);
    }
    throw std::runtime_error("local_port: unsupported address family");
}

std::string Socket::peer_address() const noexcept {
    sockaddr_storage storage{};
    socklen_t len = sizeof storage;
    if (::getpeername(fd_.get(), reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
        return "?";
    }
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), len, host, sizeof host, service,
                      sizeof service, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + service;
}

void Socket::set_nodelay(bool enabled) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof value) != 0) {
        throw_system_error(errno, "setsockopt(TCP_NODELAY)");
    }
}

void Socket::set_nonblocking(bool enabled) { tcpkv::set_nonblocking(fd_.get(), enabled); }

}  // namespace tcpkv
