// src/main.cpp - tcpkv server: blocking accept loop, one thread per connection.
//
// Deliberately simple. 08_Networking replaces the thread-per-connection model with epoll;
// here the point is a complete, buildable, testable, containerisable server.
//
// Usage: tcpkv [port]        default port 7000
// Stop:  SIGTERM or SIGINT   (systemd stop, docker stop, Ctrl-C) -> drains and exits 0.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tcpkv/protocol.hpp"
#include "tcpkv/store.hpp"

namespace {

// Written by the signal handler, read by the accept loop. sig_atomic_t + volatile is the only
// thing a signal handler may touch without a data race in C++17 (module 11).
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }   // async-signal-safe: one store to a sig_atomic_t

void log_line(const char* level, const std::string& msg) {
    // One fprintf call so lines from different threads do not interleave mid-line.
    char ts[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    std::fprintf(stderr, "%s %-5s %s\n", ts, level, msg.c_str());
}

// Write all bytes; handle partial writes and EINTR. Returns false if the peer went away.
bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: get EPIPE instead of SIGPIPE if the client closed. Belt and braces with
        // the SIG_IGN below.
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Serve one connection until QUIT, EOF, error or protocol abuse.
void serve_client(int fd, tcpkv::Store& store, std::atomic<int>& live_connections) {
    std::string inbuf;
    char chunk[4096];

    for (;;) {
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n == 0) break;                         // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            break;                                 // ECONNRESET etc.
        }
        inbuf.append(chunk, static_cast<std::size_t>(n));

        // Process every complete line in the buffer; keep the partial tail.
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = inbuf.find('\n', start);
            if (nl == std::string::npos) break;
            const std::string_view line(inbuf.data() + start, nl - start);
            start = nl + 1;

            const tcpkv::ParseResult parsed = tcpkv::parse_command(line);
            if (!parsed.ok()) {
                if (!send_all(fd, tcpkv::format_error(parsed.error))) goto done;
                continue;
            }
            const tcpkv::Command& cmd = *parsed.command;
            std::string reply;
            switch (cmd.type) {
                case tcpkv::CommandType::Ping:
                    reply = tcpkv::format_pong();
                    break;
                case tcpkv::CommandType::Get:
                    if (auto v = store.get(cmd.key)) reply = tcpkv::format_value(*v);
                    else                             reply = tcpkv::format_not_found();
                    break;
                case tcpkv::CommandType::Set:
                    store.set(cmd.key, cmd.value);
                    reply = tcpkv::format_ok();
                    break;
                case tcpkv::CommandType::Del:
                    reply = store.del(cmd.key) ? tcpkv::format_ok() : tcpkv::format_not_found();
                    break;
                case tcpkv::CommandType::Quit:
                    goto done;
            }
            if (!send_all(fd, reply)) goto done;
        }
        inbuf.erase(0, start);

        // A client that sends kMaxLineLength bytes without a newline is attacking or broken.
        if (inbuf.size() > tcpkv::kMaxLineLength) {
            send_all(fd, tcpkv::format_error("line too long"));
            break;
        }
    }
done:
    ::close(fd);
    --live_connections;
}

int make_listen_socket(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return -1; }

    // Allow immediate restart while old connections sit in TIME_WAIT.
    const int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        std::perror("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(0u);   // 0.0.0.0 == INADDR_ANY: every local interface
    addr.sin_port        = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 128) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 7000;
    if (argc > 1) {
        const long p = std::strtol(argv[1], nullptr, 10);
        if (p < 1 || p > 65535) {
            std::fprintf(stderr, "usage: %s [port 1-65535]\n", argv[0]);
            return 2;
        }
        port = static_cast<std::uint16_t>(p);
    }

    // SIGPIPE would kill the process on write to a closed socket; we want EPIPE instead.
    std::signal(SIGPIPE, SIG_IGN);
    // sigaction without SA_RESTART so blocked accept()/poll() return EINTR and we notice g_stop.
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    const int lfd = make_listen_socket(port);
    if (lfd < 0) return 1;

    tcpkv::Store store;
    std::atomic<int> live_connections{0};
    std::vector<std::thread> workers;
    std::mutex workers_mu;

    log_line("INFO", "tcpkv " TCPKV_VERSION " listening on port " + std::to_string(port));

    while (!g_stop) {
        // poll with a timeout so a SIGTERM delivered to another thread is still noticed promptly.
        pollfd pfd{};
        pfd.fd = lfd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 500 /* ms */);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }
        if (pr == 0) continue;  // timeout: loop around and re-check g_stop

        sockaddr_in peer{};
        socklen_t peer_len = sizeof peer;
        const int cfd = ::accept4(lfd, reinterpret_cast<sockaddr*>(&peer), &peer_len, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            std::perror("accept");
            continue;  // EMFILE etc.: log and keep serving
        }

        const int nodelay = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        log_line("INFO", std::string("accepted ") + ip + ":" + std::to_string(ntohs(peer.sin_port)));

        ++live_connections;
        std::lock_guard<std::mutex> lock(workers_mu);
        workers.emplace_back(serve_client, cfd, std::ref(store), std::ref(live_connections));
    }

    log_line("INFO", "shutting down: closing listener, draining " +
                     std::to_string(live_connections.load()) + " connection(s)");
    ::close(lfd);

    // Give in-flight connections a bounded grace period, then join whatever finished.
    // (A production server would also shutdown() idle client sockets to unblock their recv.)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (live_connections.load() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
        std::lock_guard<std::mutex> lock(workers_mu);
        for (auto& t : workers) {
            if (live_connections.load() == 0 && t.joinable()) t.join();
            else if (t.joinable()) t.detach();  // still blocked in recv: let the OS reclaim at exit
        }
    }
    log_line("INFO", "exit 0, store size " + std::to_string(store.size()));
    return 0;
}
