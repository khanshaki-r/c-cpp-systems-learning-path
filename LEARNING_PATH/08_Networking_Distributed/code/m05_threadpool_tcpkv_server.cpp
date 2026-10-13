// m05_threadpool_tcpkv_server.cpp - tcpkv v1: acceptor thread + fixed worker pool + blocking I/O.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m05_threadpool_tcpkv_server.cpp -o tcpkv
// Run:    ./tcpkv 7000 16          (port, worker threads)
// Test:   printf 'SET a 5\r\nhello\r\nGET a\r\nEXISTS a\r\nDEL a\r\nGET a\r\nQUIT\r\n' | nc -q1 127.0.0.1 7000
//         python3 m05_loadtest.py --port 7000 -c 50 -n 2000
// Stop:   Ctrl-C (SIGINT) or kill -TERM <pid>  => graceful shutdown: stop accepting, drain, close.
//
// Architecture
//   main thread      : blocks in sigwait(SIGINT/SIGTERM); on signal runs the shutdown sequence.
//   acceptor thread  : accept() loop, pushes connections into a bounded queue (or rejects when full).
//   N worker threads : pop a connection, serve it with blocking I/O until it closes or idles out.
//   => at most N connections are served concurrently; up to `queue_cap` wait; the rest get
//      "-ERR_BUSY" and are closed. That is the honest limit of blocking I/O + thread pool.
#include "m03_socket.hpp"
#include "m04_tcpkv_parser.hpp"

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// The key-value store. Reads take a shared lock, writes an exclusive one. One global lock is
// fine for a learning server; module 12 (sharding) is where you split it.
// ---------------------------------------------------------------------------------------------
class Store {
public:
    bool get(const std::string& k, std::string& out) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }
    void set(const std::string& k, std::string v) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        map_[k] = std::move(v);
    }
    bool del(const std::string& k) {
        std::unique_lock<std::shared_mutex> lk(mu_);
        return map_.erase(k) > 0;
    }
    bool exists(const std::string& k) const {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return map_.count(k) > 0;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};

// ---------------------------------------------------------------------------------------------
// Server state shared by all threads.
// ---------------------------------------------------------------------------------------------
struct Server {
    Store store;

    std::atomic<bool> stopping{false};

    // Bounded queue of accepted connections waiting for a worker.
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<net::Fd> queue;
    size_t queue_cap = 256;

    // Connections currently being served, so shutdown can wake their blocked readers.
    std::mutex active_mu;
    std::set<int> active_fds;

    // Counters for the shutdown report (and for /metrics in a real server).
    std::atomic<uint64_t> accepted{0}, rejected{0}, served{0}, commands{0}, idle_timeouts{0}, errors{0};

    std::chrono::seconds idle_timeout{60};
};

std::string execute(Server& srv, const tcpkv::Command& c) {
    using tcpkv::Op;
    switch (c.op) {
    case Op::Ping: return tcpkv::fmt_pong();
    case Op::Get: {
        std::string v;
        return srv.store.get(c.key, v) ? tcpkv::fmt_bulk(v) : tcpkv::fmt_nil();
    }
    case Op::Set:
        srv.store.set(c.key, c.value);
        return tcpkv::fmt_ok();
    case Op::Del: return tcpkv::fmt_int(srv.store.del(c.key) ? 1 : 0);
    case Op::Exists: return tcpkv::fmt_int(srv.store.exists(c.key) ? 1 : 0);
    case Op::Quit: return tcpkv::fmt_ok();
    }
    return tcpkv::fmt_err("ERR_INTERNAL", "unreachable");
}

// ---------------------------------------------------------------------------------------------
// Serve one connection to completion. Blocking I/O, but every wait is bounded by poll() so the
// thread notices `stopping` and the idle deadline.
// ---------------------------------------------------------------------------------------------
void serve_connection(Server& srv, net::Fd conn) {
    const int fd = conn.get();
    {
        std::lock_guard<std::mutex> lk(srv.active_mu);
        srv.active_fds.insert(fd);
    }
    // Make sure the fd leaves the active set on every path, before the Fd destructor closes it.
    struct ActiveGuard {
        Server& s;
        int fd;
        ~ActiveGuard() {
            std::lock_guard<std::mutex> lk(s.active_mu);
            s.active_fds.erase(fd);
        }
    } guard{srv, fd};

    try {
        net::set_nodelay(fd);
        net::set_send_timeout(fd, 10000); // a client that never reads must not pin a worker forever

        tcpkv::Parser parser;
        char buf[16 * 1024];
        std::string out;
        auto deadline = Clock::now() + srv.idle_timeout;

        for (;;) {
            // Wait up to 1 s for data. The short tick lets us re-check `stopping` and the idle
            // deadline without a signal or another thread having to poke this one.
            if (!net::wait_readable(fd, 1000)) {
                if (srv.stopping.load()) {
                    net::write_all(fd, tcpkv::fmt_err("ERR_SHUTDOWN", "server shutting down"));
                    return;
                }
                if (Clock::now() >= deadline) {
                    ++srv.idle_timeouts;
                    return; // idle client: free the worker. Closing is the message.
                }
                continue;
            }

            size_t n = net::read_some(fd, buf, sizeof buf);
            if (n == 0) return; // peer closed
            deadline = Clock::now() + srv.idle_timeout;
            parser.feed(buf, n);

            // Drain everything parsable from this read, batch the replies into ONE send.
            // Pipelined clients get one syscall per batch instead of one per command.
            out.clear();
            bool quit = false;
            for (;;) {
                tcpkv::Command cmd;
                tcpkv::ParseError err;
                tcpkv::ParseStatus st = parser.next(cmd, err);
                if (st == tcpkv::ParseStatus::NeedMore) break;
                if (st == tcpkv::ParseStatus::Error) {
                    out += tcpkv::fmt_err(err.code, err.message);
                    ++srv.errors;
                    if (err.fatal) {
                        quit = true;
                        break;
                    }
                    continue;
                }
                ++srv.commands;
                out += execute(srv, cmd);
                if (cmd.op == tcpkv::Op::Quit) {
                    quit = true;
                    break;
                }
            }
            if (!out.empty()) net::write_all(fd, out);
            if (quit) return;
        }
    } catch (const std::system_error& e) {
        // ECONNRESET, EPIPE, EAGAIN (send timeout): the client is gone or stuck. Log, close.
        std::fprintf(stderr, "[conn %d] %s\n", fd, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[conn %d] %s\n", fd, e.what());
    }
}

void worker_main(Server& srv, int id) {
    (void)id;
    for (;;) {
        net::Fd conn;
        {
            std::unique_lock<std::mutex> lk(srv.q_mu);
            srv.q_cv.wait(lk, [&] { return srv.stopping.load() || !srv.queue.empty(); });
            if (srv.queue.empty()) return; // stopping and nothing left
            conn = std::move(srv.queue.front());
            srv.queue.pop_front();
        }
        ++srv.served;
        serve_connection(srv, std::move(conn));
    }
}

void acceptor_main(Server& srv, int listen_fd) {
    for (;;) {
        net::Fd conn;
        try {
            conn = net::accept_tcp(listen_fd);
        } catch (const std::system_error& e) {
            if (e.code().value() == EMFILE || e.code().value() == ENFILE) {
                // Out of fds: almost always a leak elsewhere. Back off instead of spinning.
                std::fprintf(stderr, "accept: %s (fd limit) - sleeping 100 ms\n", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::fprintf(stderr, "accept: %s\n", e.what());
            continue;
        }
        if (!conn) return; // listener was shut down: stop accepting
        ++srv.accepted;

        std::unique_lock<std::mutex> lk(srv.q_mu);
        if (srv.queue.size() >= srv.queue_cap) {
            lk.unlock();
            ++srv.rejected;
            // Load shedding: tell the client explicitly instead of letting it hang in the queue.
            try {
                net::write_all(conn.get(), tcpkv::fmt_err("ERR_BUSY", "server overloaded, retry later"));
            } catch (...) {
            }
            continue; // conn closes here
        }
        srv.queue.push_back(std::move(conn));
        lk.unlock();
        srv.q_cv.notify_one();
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "7000";
    const int nworkers = argc > 2 ? std::atoi(argv[2]) : 8;
    const std::chrono::seconds drain_timeout{10};

    net::ignore_sigpipe();

    // Block SIGINT/SIGTERM in this thread BEFORE creating others: threads inherit the mask, so
    // the signals are delivered only via sigwait() below, never to a random worker.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
        std::perror("pthread_sigmask");
        return EXIT_FAILURE;
    }

    Server srv;
    net::Fd listener;
    try {
        // backlog 1024: completed handshakes waiting for accept(). Also capped by somaxconn.
        listener = net::listen_tcp(nullptr, port, 1024);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "listen failed: %s\n", e.what());
        return EXIT_FAILURE;
    }
    std::printf("tcpkv v1 on port %s, %d workers, queue cap %zu, idle timeout %llds\n", port, nworkers,
                srv.queue_cap, static_cast<long long>(srv.idle_timeout.count()));

    std::thread acceptor(acceptor_main, std::ref(srv), listener.get());
    std::vector<std::thread> workers;
    for (int i = 0; i < nworkers; ++i) workers.emplace_back(worker_main, std::ref(srv), i);

    // ----- wait for a signal ---------------------------------------------------------------
    int sig = 0;
    sigwait(&set, &sig);
    std::printf("\nsignal %d: graceful shutdown\n", sig);

    // ----- 1. stop accepting: shutdown() on the listener makes accept() fail with EINVAL -----
    ::shutdown(listener.get(), SHUT_RDWR);
    acceptor.join();
    listener.reset(); // port is released; a new instance can bind now (SO_REUSEADDR)

    // ----- 2. tell workers and connections to finish ------------------------------------------
    srv.stopping.store(true);
    srv.q_cv.notify_all(); // idle workers exit; busy ones notice `stopping` within 1 s

    // Queued-but-unserved connections get a clear error instead of a silent close.
    {
        std::lock_guard<std::mutex> lk(srv.q_mu);
        for (auto& c : srv.queue) {
            try {
                net::write_all(c.get(), tcpkv::fmt_err("ERR_SHUTDOWN", "server shutting down"));
            } catch (...) {
            }
        }
        srv.queue.clear();
    }

    // ----- 3. drain: wait up to drain_timeout for active connections to finish --------------
    auto deadline = Clock::now() + drain_timeout;
    for (;;) {
        size_t active;
        {
            std::lock_guard<std::mutex> lk(srv.active_mu);
            active = srv.active_fds.size();
        }
        if (active == 0 || Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ----- 4. force: wake anything still blocked in poll/recv --------------------------------
    {
        std::lock_guard<std::mutex> lk(srv.active_mu);
        for (int fd : srv.active_fds) ::shutdown(fd, SHUT_RDWR); // read() returns 0 => worker exits
    }
    for (auto& w : workers) w.join();

    std::printf("accepted=%llu rejected=%llu served=%llu commands=%llu idle_timeouts=%llu errors=%llu\n",
                static_cast<unsigned long long>(srv.accepted.load()), static_cast<unsigned long long>(srv.rejected.load()),
                static_cast<unsigned long long>(srv.served.load()), static_cast<unsigned long long>(srv.commands.load()),
                static_cast<unsigned long long>(srv.idle_timeouts.load()), static_cast<unsigned long long>(srv.errors.load()));
    return EXIT_SUCCESS;
}
