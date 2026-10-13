// m03_socket_fd_raii.cpp
// A move-only RAII wrapper for a POSIX file descriptor, the way tcpkv wraps its sockets.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_socket_fd_raii.cpp
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <system_error>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class SocketFd {
public:
    SocketFd() noexcept = default;                       // empty handle, fd_ == -1
    explicit SocketFd(int fd) noexcept : fd_(fd) {}      // take ownership of an fd returned by socket()/accept()
    ~SocketFd() { reset(); }

    SocketFd(const SocketFd&) = delete;                  // an fd has exactly one owner
    SocketFd& operator=(const SocketFd&) = delete;

    SocketFd(SocketFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    SocketFd& operator=(SocketFd&& o) noexcept {
        if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }

    int  get() const noexcept   { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int  release() noexcept     { return std::exchange(fd_, -1); }   // give the fd back to manual management
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) { ::close(fd_); std::cout << "  close(" << fd_ << ")\n"; }
        fd_ = fd;
    }
private:
    int fd_ = -1;
};

SocketFd make_listener(std::uint16_t port) {
    SocketFd s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) throw std::system_error(errno, std::generic_category(), "socket");

    int yes = 1;
    ::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        throw std::system_error(errno, std::generic_category(), "bind");    // s.~SocketFd() closes the fd for us
    if (::listen(s.get(), 16) != 0)
        throw std::system_error(errno, std::generic_category(), "listen");  // same here
    return s;   // moved (or elided) out to the caller. No close here: ownership travels with the object.
}

int main() {
    try {
        SocketFd listener = make_listener(0);   // port 0: the kernel picks a free port
        std::cout << "listening on fd " << listener.get() << '\n';

        SocketFd other = std::move(listener);   // ownership transfer; exactly one close will happen
        std::cout << "after move: listener.valid()=" << listener.valid() << " other.fd=" << other.get() << '\n';
    } catch (const std::system_error& e) {
        std::cout << "error: " << e.what() << '\n';
    }
    std::cout << "end of main\n";
    return 0;
}
