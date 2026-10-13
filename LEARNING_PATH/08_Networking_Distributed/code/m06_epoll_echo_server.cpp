// m06_epoll_echo_server.cpp - single-threaded, edge-triggered epoll echo server (reactor pattern).
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m06_epoll_echo_server.cpp -o epoll_echo
// Run:    ./epoll_echo 7000
// Test:   python3 m05_loadtest.py --echo --port 7000 -c 500 -n 200
//         seq 1 100000 | ./echo_client 127.0.0.1 7000
// Stop:   Ctrl-C => shutdown via eventfd, all connections closed, stats printed.
//
// One thread handles thousands of connections because nothing ever blocks: every socket is
// O_NONBLOCK, and epoll tells us which ones have work. The price is that the per-connection
// "where was I" that a thread kept on its stack must now live in an explicit Conn object:
// the bytes we could not yet write, whether the peer closed, when it was last active.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, std::strerror(errno));
    std::exit(EXIT_FAILURE);
}

// Kept for reference: the portable way to set O_NONBLOCK on an fd you did not create yourself.
// This server uses SOCK_NONBLOCK and accept4 instead, so the function is unused here.
[[maybe_unused]] void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) die("fcntl O_NONBLOCK");
}

int make_listener(const char* port, int backlog) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(nullptr, port, &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        std::exit(EXIT_FAILURE);
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, backlog) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) die("bind/listen");
    return fd;
}

using Clock = std::chrono::steady_clock;

// Everything a blocking thread would have kept on its stack, made explicit.
struct Conn {
    int fd = -1;
    std::string out;            // bytes read but not yet written (the peer was not ready)
    bool peer_closed = false;   // we saw EOF; close once `out` is flushed
    bool want_write = false;    // EPOLLOUT currently registered
    bool reading_paused = false;// EPOLLIN removed because `out` grew too large (backpressure)
    Clock::time_point last_active;
    uint64_t bytes_in = 0, bytes_out = 0;

    ~Conn() {
        if (fd >= 0) close(fd);
    }
};

constexpr size_t kHighWater = 1 << 20; // pause reading when a client will not read our replies
constexpr int kIdleSeconds = 60;

struct Server {
    int epfd = -1;
    int listen_fd = -1;
    int shutdown_efd = -1;
    int timer_fd = -1;
    std::unordered_map<int, std::unique_ptr<Conn>> conns;
    uint64_t accepted = 0, closed = 0, idle_closed = 0;
    bool running = true;

    void epoll_update(Conn& c) {
        epoll_event ev{};
        ev.events = EPOLLET | EPOLLRDHUP;
        if (!c.reading_paused) ev.events |= EPOLLIN;
        if (c.want_write) ev.events |= EPOLLOUT;
        ev.data.ptr = &c;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, c.fd, &ev) < 0) die("epoll_ctl MOD");
    }

    void close_conn(Conn& c) {
        // EPOLL_CTL_DEL is implicit on close(), but being explicit avoids surprises with dup'd fds.
        epoll_ctl(epfd, EPOLL_CTL_DEL, c.fd, nullptr);
        ++closed;
        conns.erase(c.fd); // destroys Conn => close(fd). `c` is dangling after this line.
    }

    // Edge-triggered rule #1: after a readiness event you MUST drain until EAGAIN, because you
    // will not be told again until NEW data arrives.
    void on_accept() {
        for (;;) {
            int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return; // queue drained
                if (errno == EINTR || errno == ECONNABORTED) continue;
                if (errno == EMFILE || errno == ENFILE) {
                    std::fprintf(stderr, "accept: out of fds\n");
                    return; // level-triggered listener would spin here; ET at least waits for the next SYN
                }
                std::fprintf(stderr, "accept: %s\n", std::strerror(errno));
                return;
            }
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

            auto c = std::make_unique<Conn>();
            c->fd = cfd;
            c->last_active = Clock::now();
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
            ev.data.ptr = c.get();
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) die("epoll_ctl ADD");
            conns.emplace(cfd, std::move(c));
            ++accepted;
        }
    }

    // Try to push `out` to the kernel. Returns false if the connection died.
    bool flush(Conn& c) {
        while (!c.out.empty()) {
            ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // kernel buffer full: wait for EPOLLOUT
                return false;                                       // EPIPE / ECONNRESET
            }
            c.out.erase(0, static_cast<size_t>(n));
            c.bytes_out += static_cast<uint64_t>(n);
        }
        bool need_out = !c.out.empty();
        bool resume_read = c.reading_paused && c.out.size() < kHighWater / 2;
        if (need_out != c.want_write || resume_read) {
            c.want_write = need_out;
            if (resume_read) c.reading_paused = false;
            epoll_update(c);
        }
        return true;
    }

    void on_readable(Conn& c) {
        char buf[64 * 1024];
        for (;;) {
            ssize_t n = recv(c.fd, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // drained (ET rule #1)
                close_conn(c);
                return;
            }
            if (n == 0) { // EOF: peer finished sending. Flush what we owe, then close.
                c.peer_closed = true;
                break;
            }
            c.bytes_in += static_cast<uint64_t>(n);
            c.last_active = Clock::now();
            c.out.append(buf, static_cast<size_t>(n));
            if (c.out.size() >= kHighWater) {
                // Backpressure: the peer sends but does not read. Stop reading so the kernel's
                // receive window closes and TCP flow control pushes back on the peer.
                c.reading_paused = true;
                epoll_update(c);
                break;
            }
        }
        if (!flush(c)) {
            close_conn(c);
            return;
        }
        if (c.peer_closed && c.out.empty()) close_conn(c);
    }

    void on_writable(Conn& c) {
        if (!flush(c)) {
            close_conn(c);
            return;
        }
        if (c.peer_closed && c.out.empty()) close_conn(c);
    }

    void on_timer() {
        uint64_t expirations;
        if (read(timer_fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN) die("timerfd read");
        auto now = Clock::now();
        std::vector<Conn*> idle;
        for (auto& kv : conns)
            if (now - kv.second->last_active > std::chrono::seconds(kIdleSeconds)) idle.push_back(kv.second.get());
        for (Conn* c : idle) {
            ++idle_closed;
            close_conn(*c);
        }
    }

    void run() {
        epoll_event events[256];
        while (running) {
            int n = epoll_wait(epfd, events, 256, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                die("epoll_wait");
            }
            for (int i = 0; i < n; ++i) {
                void* p = events[i].data.ptr;
                uint32_t ev = events[i].events;
                if (p == &listen_fd) {
                    on_accept();
                } else if (p == &shutdown_efd) {
                    running = false;
                } else if (p == &timer_fd) {
                    on_timer();
                } else {
                    Conn& c = *static_cast<Conn*>(p);
                    // A connection may have been closed earlier in this same batch (e.g. by the
                    // timer). Guard against using a dangling pointer by looking it up again.
                    auto it = conns.find(c.fd);
                    if (it == conns.end() || it->second.get() != &c) continue;
                    if (ev & (EPOLLERR | EPOLLHUP)) {
                        close_conn(c);
                        continue;
                    }
                    if (ev & (EPOLLIN | EPOLLRDHUP)) {
                        const int fd = c.fd; // copy BEFORE the call: on_readable may destroy `c`
                        on_readable(c);
                        if (conns.find(fd) == conns.end()) continue; // closed inside
                    }
                    if (ev & EPOLLOUT) on_writable(c);
                }
            }
        }
    }
};

int g_shutdown_efd = -1;

void on_signal(int) {
    // write() is async-signal-safe; the event loop sees the eventfd become readable.
    uint64_t one = 1;
    ssize_t r = write(g_shutdown_efd, &one, sizeof one);
    (void)r;
}

} // namespace

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "7000";
    signal(SIGPIPE, SIG_IGN);

    Server s;
    s.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (s.epfd < 0) die("epoll_create1");

    s.listen_fd = make_listener(port, 1024);
    {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET; // ET on the listener too: on_accept drains until EAGAIN
        ev.data.ptr = &s.listen_fd;
        if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.listen_fd, &ev) < 0) die("epoll_ctl listener");
    }

    // eventfd: a counter the kernel exposes as an fd. Signal handlers and other threads write to
    // it; the event loop wakes up. This is how you talk to an event loop from outside.
    s.shutdown_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s.shutdown_efd < 0) die("eventfd");
    g_shutdown_efd = s.shutdown_efd;
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &s.shutdown_efd;
        if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.shutdown_efd, &ev) < 0) die("epoll_ctl eventfd");
    }
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // timerfd: a timer the kernel exposes as an fd. Fires every second for idle sweeps.
    s.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (s.timer_fd < 0) die("timerfd_create");
    {
        itimerspec its{};
        its.it_interval.tv_sec = 1;
        its.it_value.tv_sec = 1;
        if (timerfd_settime(s.timer_fd, 0, &its, nullptr) < 0) die("timerfd_settime");
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &s.timer_fd;
        if (epoll_ctl(s.epfd, EPOLL_CTL_ADD, s.timer_fd, &ev) < 0) die("epoll_ctl timerfd");
    }

    std::printf("epoll echo server on port %s (single thread, edge-triggered)\n", port);
    s.run();

    std::printf("shutting down: accepted=%llu closed=%llu idle_closed=%llu still_open=%zu\n",
                static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.closed),
                static_cast<unsigned long long>(s.idle_closed), s.conns.size());
    s.conns.clear(); // closes every connection (RAII in ~Conn)
    close(s.timer_fd);
    close(s.shutdown_efd);
    close(s.listen_fd);
    close(s.epfd);
    return EXIT_SUCCESS;
}
