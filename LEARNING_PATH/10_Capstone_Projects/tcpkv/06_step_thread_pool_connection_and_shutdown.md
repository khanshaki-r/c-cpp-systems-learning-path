# Step 06. Thread pool, move-only Connection handles, graceful shutdown

**Time:** ~10 hours   **Week:** 3 (Oct 20 – Oct 26)
**Read first:** 04_Concurrency: `std::condition_variable` and the wait-with-predicate idiom,
spurious wakeups, producer/consumer queues, `std::atomic<bool>` and memory orders (`relaxed`
vs `acquire`/`release`), `std::thread` lifetime rules (a joinable thread's destructor
terminates), thread pools, lost wakeups. 02_Modern_CPP: type erasure, `std::unique_ptr`,
lambdas with init-capture (`[c = std::move(conn)]`), `mutable` lambdas, SFINAE with
`std::enable_if_t`. 06_Linux_Toolchain_DevOps: signals and `sigaction`, async-signal-safe
functions, the self-pipe trick. 08_Networking_Distributed: `poll()` on multiple descriptors.

## 1. Goal

Make the server concurrent and make it stop cleanly.

* A `ThreadPool` with N worker threads and one queue.
* A `Connection` that can be handed to the pool by *moving* it into a task, and that notices
  a shutdown flag within a bounded time.
* A `Server` class that owns the accept loop and can be stopped from a signal handler.
* Integration tests that start a real server on an ephemeral port.

In plain words: in step 05 one waiter served the whole restaurant. Now there are N waiters
(the pool) and a host at the door (the acceptor) who hands each new table to a free waiter.
And when the manager says "we're closing" (SIGINT), the host stops seating, each waiter
finishes the order in hand, and everyone goes home. Nobody is left mid-sentence.

## 2. Design

### 2.1 The thread pool

```
submit(task) --> [ queue (deque<Task>) ] --> worker 1..N: wait(cv) ; pop ; run ; repeat
                        |                       |
                   mutex + work_cv         idle_cv for drain()
```

Three subtleties that separate a toy pool from a usable one:

1. **The task type must be move-only.** `std::function` requires copyable callables, and a
   lambda that captured a `Connection` is not copyable. So `MoveOnlyFunction` (40 lines of
   type erasure) is the task type. C++23 has `std::move_only_function`; we are on C++17.
2. **Stop semantics are explicit.** `stop()` rejects new tasks, *drops* queued ones (their
   destructors run, closing sockets), and lets running tasks finish. The destructor calls
   `stop()` and joins. For a server this is what you want: queued connections that were
   never served are closed, not served after shutdown began.
3. **A task that throws must not kill the worker.** The worker catches, logs, continues.
   An uncaught exception in a `std::thread` calls `std::terminate`.

### 2.2 The connection, revisited

The step-05 `Connection` blocked in `read()` forever. A blocked thread cannot notice a flag.
So the loop becomes: `poll(fd, 250 ms)`; on timeout re-check the stop flag; on readable,
`read()`. Shutdown latency is bounded by the poll interval; 250 ms is invisible to humans
and costs four wakeups per second per idle connection.

### 2.3 The server and the self-pipe

```
signal handler  -->  Server::request_stop():  stop_.store(true); write(wake_pipe, 1 byte)
acceptor loop   -->  poll({listener, wake_pipe_read})  -->  returns at once  -->  loop ends
```

A signal handler may only call async-signal-safe functions: no `malloc`, no mutex, no
`std::cout`, no `condition_variable::notify`. `write(2)` on a pipe and a lock-free atomic
store are allowed. The acceptor polls the pipe's read end alongside the listener, so one
byte wakes it immediately instead of after its next timeout. This is the self-pipe trick,
older than Linux and still the right answer.

### 2.4 Ownership through the pipeline

```
accept()  ->  Socket (acceptor thread)
          ->  Connection{Socket, &store, &stop_flag, config}   (acceptor thread)
          ->  lambda [conn = std::move(connection)]() mutable { conn.serve(); }
          ->  MoveOnlyFunction  ->  queue  ->  worker: task()  ->  ~lambda  ->  ~Connection  ->  ~Socket  ->  close()
```

The socket is created on one thread and closed on another; no `shared_ptr`, no reference
counting, no manual `close`. The compiler enforces single ownership at every arrow.

## 3. Files to create

### `include/tcpkv/thread_pool.hpp` and `src/thread_pool.cpp`

Identical to `code/include/tcpkv/thread_pool.hpp` and `code/src/thread_pool.cpp`.
The two pieces to understand completely:

```cpp
class MoveOnlyFunction {
public:
    MoveOnlyFunction() noexcept = default;

    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
                                                std::is_invocable_r_v<void, std::decay_t<F>&>>>
    MoveOnlyFunction(F&& f)
        : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    void operator()() { impl_->call(); }
    explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct Base {
        virtual ~Base() = default;
        virtual void call() = 0;
    };
    template <class F>
    struct Impl final : Base {
        template <class G>
        explicit Impl(G&& g) : f(std::forward<G>(g)) {}
        void call() override { f(); }
        F f;
    };
    std::unique_ptr<Base> impl_;
};
```

This is type erasure: any callable `F` is stored inside an `Impl<F>` behind a `Base`
pointer, so the pool's queue holds one concrete type. The `enable_if` stops the template
constructor from hijacking the move constructor (`is_same` check) and from accepting things
that are not callable with no arguments.

```cpp
void ThreadPool::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        try {
            task();
        } catch (const std::exception& e) {
            log::error("task threw: ", e.what());
        } catch (...) {
            log::error("task threw a non-std exception");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (queue_.empty() && active_ == 0) {
                idle_cv_.notify_all();
            }
        }
    }
}
```

`wait(lock, predicate)` is the only correct way to wait on a condition variable: it handles
spurious wakeups and the lost-wakeup race (the predicate is checked under the lock before
sleeping). The task runs *outside* the lock, or there would be no concurrency.

Note also the constructor: if creating thread number 5 of 8 throws (`EAGAIN`), the first four
are already running. The constructor catches, stops, joins and rethrows, because otherwise
the destructor never runs (the object was never fully constructed) and `std::thread`'s
destructor on a joinable thread calls `std::terminate`.

### `include/tcpkv/connection.hpp` (step-06 version)

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

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

std::string execute_command(Command command, Store& store);

// One client connection, served by one worker thread for its whole lifetime.
//
// Move-only: it owns the Socket. Dependencies are held as pointers (not references) so the
// class stays move-assignable; a moved-from Connection holds an invalid socket and must not be
// served.
class Connection {
public:
    Connection(Socket socket, Store& store, const std::atomic<bool>& stop_flag,
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
    bool process_events(Parser& parser, std::string& out);

    Socket socket_;
    Store* store_;
    const std::atomic<bool>* stop_;
    ConnectionConfig config_;
};

}  // namespace tcpkv
```

### `src/connection.cpp` (step-06 version)

`execute_command` is unchanged from step 05. Replace the constructor and `serve()`:

```cpp
Connection::Connection(Socket socket, Store& store, const std::atomic<bool>& stop_flag,
                       ConnectionConfig config) noexcept
    : socket_(std::move(socket)), store_(&store), stop_(&stop_flag), config_(config) {}

void Connection::serve() {
    using Clock = std::chrono::steady_clock;

    const std::string peer = socket_.peer_address();
    log::debug("connection from ", peer);

    Parser parser;
    std::vector<char> buffer(config_.read_buffer_size);
    std::string out;
    auto last_activity = Clock::now();

    try {
        while (!stop_->load(std::memory_order_relaxed)) {
            if (!socket_.wait_readable(config_.poll_interval)) {
                if (config_.idle_timeout.count() > 0 &&
                    Clock::now() - last_activity >= config_.idle_timeout) {
                    log::debug("connection ", peer, " idle timeout");
                    break;
                }
                continue;  // timed out: loop re-checks the stop flag
            }

            const std::size_t n = socket_.read_some(buffer.data(), buffer.size());
            if (n == 0) {
                break;  // peer closed its side
            }
            last_activity = Clock::now();
            parser.feed(buffer.data(), n);

            out.clear();
            const bool keep_open = process_events(parser, out);
            if (!out.empty()) {
                socket_.write_all(out);
            }
            if (!keep_open) {
                break;
            }
        }
    } catch (const std::system_error& e) {
        log::debug("connection ", peer, " error: ", e.what());
    }

    socket_.shutdown_both();
    log::debug("connection ", peer, " closed");
}
```

(`process_events` is unchanged from step 05. Add `#include <chrono>` at the top.)

`memory_order_relaxed` is enough for the stop flag: we only need to *eventually* see the
store, and nothing else is published through it. Connection events moved to `debug` level so
a busy server does not log thousands of lines a second.

### `include/tcpkv/server.hpp` (step-06 version)

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "tcpkv/connection.hpp"
#include "tcpkv/fd.hpp"
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
    Server(ServerConfig config, Store& store);
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

    ServerConfig config_;
    Store& store_;
    Socket listener_;
    FileDescriptor wake_read_;
    FileDescriptor wake_write_;
    std::atomic<bool> stop_{false};
};

}  // namespace tcpkv
```

### `src/server.cpp` (step-06 version)

```cpp
#include "tcpkv/server.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

#include "tcpkv/log.hpp"

namespace tcpkv {

Server::Server(ServerConfig config, Store& store)
    : config_(std::move(config)),
      store_(store),
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
    ThreadPool pool(config_.threads);

    pollfd fds[2]{};
    fds[0].fd = listener_.fd();
    fds[0].events = POLLIN;
    fds[1].fd = wake_read_.get();
    fds[1].events = POLLIN;

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
    }

    log::info("shutdown requested: ", pool.pending(), " connections queued");
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

    if (pool.pending() >= config_.max_pending_connections) {
        log::warn("rejecting ", client.peer_address(), ": ", pool.pending(), " connections queued");
        try {
            client.write_all(reply::error("server busy, try again later"));
        } catch (const std::system_error&) {
            // The client is gone; nothing to do.
        }
        return;  // `client` closes here
    }

    Connection connection(std::move(client), store_, stop_, config_.connection);
    if (!pool.submit([conn = std::move(connection)]() mutable { conn.serve(); })) {
        log::warn("pool stopped; dropping new connection");
    }
}

}  // namespace tcpkv
```

Why `EMFILE` gets a sleep: when the process is out of descriptors, `accept` fails but the
connection stays in the kernel's queue, so `poll` returns readable immediately, `accept`
fails again, and the acceptor spins at 100% CPU. Sleeping 100 ms turns a spin into a
graceful slowdown while connections close and free descriptors. This is a well-known
production failure mode (Nginx, HAProxy and Redis all handle it).

### `apps/tcpkv_server.cpp` (step-06 version)

Identical to `code/apps/tcpkv_server.cpp` **except**: no `Metrics`, `Server server(options.server, store);`
with two arguments, and no `--stats-interval` flag (the `Options` struct, `--port`, `--threads`,
`--host`, `--idle-timeout`, `--max-pending`, `--log-level`, `--help` are all as in the final
version). The signal handling is the essential part:

```cpp
// The only global: the signal handler needs to find the server. An atomic pointer load is
// async-signal-safe (lock-free atomics are allowed in handlers).
std::atomic<tcpkv::Server*> g_server{nullptr};

void on_signal(int /*signum*/) {
    if (tcpkv::Server* server = g_server.load(std::memory_order_acquire)) {
        server->request_stop();  // atomic store + write(2): both async-signal-safe
    }
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: let poll() return EINTR so loops re-check the flag
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}
```

and in `main`:

```cpp
        tcpkv::Store store;
        tcpkv::Server server(options.server, store);

        g_server.store(&server, std::memory_order_release);
        install_signal_handlers();

        server.run();

        g_server.store(nullptr, std::memory_order_release);
        tcpkv::log::info("tcpkv stopped cleanly");
        return 0;
```

`sigaction`, not `signal()`: `signal()`'s semantics (one-shot or not, `SA_RESTART` or not)
vary between systems; `sigaction` says exactly what you get.

### `tests/thread_pool_test.cpp`

Identical to `code/tests/thread_pool_test.cpp`. Tests: all tasks run; `size()` is at least 1;
tasks run on worker threads not the caller; move-only task; a throwing task does not kill the
worker; `stop()` drops queued tasks, rejects new ones, is idempotent; destructor waits for a
running task; `drain()` returns when idle, repeatedly; dropped tasks' destructors run (a
`Probe` type that counts its own destruction stands in for `Connection`).

### `tests/integration_test.cpp` (step-06 version, raw sockets; the client library comes in step 07)

```cpp
// End-to-end tests: a real Server on an ephemeral port, real sockets, real threads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/log.hpp"
#include "tcpkv/protocol.hpp"
#include "tcpkv/server.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

using tcpkv::Server;
using tcpkv::ServerConfig;
using tcpkv::Socket;
using tcpkv::Store;
namespace reply = tcpkv::reply;

namespace {

constexpr std::size_t kWorkers = 4;

std::string read_exactly(Socket& socket, std::size_t count) {
    std::string out;
    char buf[4096];
    while (out.size() < count) {
        const std::size_t n = socket.read_some(buf, std::min(sizeof buf, count - out.size()));
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

std::string read_until_eof(Socket& socket) {
    std::string out;
    char buf[4096];
    for (;;) {
        std::size_t n = 0;
        try {
            n = socket.read_some(buf, sizeof buf);
        } catch (const std::system_error&) {
            break;  // ECONNRESET counts as "closed" for our purposes
        }
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

// Sends `request` and reads back exactly `expected` bytes; returns what was read.
std::string roundtrip(Socket& socket, const std::string& request, const std::string& expected) {
    socket.write_all(request);
    return read_exactly(socket, expected.size());
}

class ServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        tcpkv::log::set_level(tcpkv::log::Level::Warn);
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;  // ephemeral: tests can run in parallel
        config.threads = kWorkers;
        config.connection.poll_interval = std::chrono::milliseconds(20);
        server_ = std::make_unique<Server>(config, store_);
        thread_ = std::thread([this] { server_->run(); });
    }

    void TearDown() override {
        server_->request_stop();
        thread_.join();
    }

    std::uint16_t port() const { return server_->port(); }
    Socket connect() { return Socket::connect_tcp("127.0.0.1", port()); }

    Store store_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

}  // namespace

TEST_F(ServerFixture, PingPong) {
    Socket s = connect();
    EXPECT_EQ(roundtrip(s, "PING\n", "+PONG\n"), "+PONG\n");
}

TEST_F(ServerFixture, SetGetDelRoundTrip) {
    Socket s = connect();
    EXPECT_EQ(roundtrip(s, "GET k\n", "$-1\n"), "$-1\n");
    EXPECT_EQ(roundtrip(s, tcpkv::encode_set("k", "hello"), "+OK\n"), "+OK\n");
    const std::string bulk = reply::bulk("hello");
    EXPECT_EQ(roundtrip(s, "GET k\n", bulk), bulk);
    EXPECT_EQ(roundtrip(s, "EXISTS k missing\n", ":1\n"), ":1\n");
    EXPECT_EQ(roundtrip(s, "DEL k missing\n", ":1\n"), ":1\n");
    EXPECT_EQ(roundtrip(s, "GET k\n", "$-1\n"), "$-1\n");
}

TEST_F(ServerFixture, ValuesAreBinarySafe) {
    Socket s = connect();
    const std::string value("line one\nline two\r\n\x00\xff tail", 26);
    EXPECT_EQ(roundtrip(s, tcpkv::encode_set("bin", value), "+OK\n"), "+OK\n");
    const std::string bulk = reply::bulk(value);
    EXPECT_EQ(roundtrip(s, "GET bin\n", bulk), bulk);

    const std::string big(tcpkv::limits::kMaxValueLength, 'B');
    EXPECT_EQ(roundtrip(s, tcpkv::encode_set("big", big), "+OK\n"), "+OK\n");
    const std::string big_bulk = reply::bulk(big);
    EXPECT_EQ(roundtrip(s, "GET big\n", big_bulk), big_bulk);
}

TEST_F(ServerFixture, KeysWithPrefix) {
    Socket s = connect();
    roundtrip(s, tcpkv::encode_set("user:1", "a"), "+OK\n");
    roundtrip(s, tcpkv::encode_set("user:2", "b"), "+OK\n");
    roundtrip(s, tcpkv::encode_set("order:1", "c"), "+OK\n");
    const std::string users = reply::array({"user:1", "user:2"});
    EXPECT_EQ(roundtrip(s, "KEYS user:\n", users), users);
}

TEST_F(ServerFixture, UnknownCommandIsAnErrorButKeepsTheConnection) {
    Socket s = connect();
    const std::string error = "-ERR unknown command 'FROB'\n";
    EXPECT_EQ(roundtrip(s, "FROB x\n", error), error);
    EXPECT_EQ(roundtrip(s, "PING\n", "+PONG\n"), "+PONG\n");  // same connection still works
}

TEST_F(ServerFixture, NotYetImplementedCommandsReplyWithError) {
    Socket s = connect();
    const std::string error = "-ERR not implemented yet\n";
    EXPECT_EQ(roundtrip(s, "TTL k\n", error), error);
}

TEST_F(ServerFixture, OversizedSetHeaderIsFatalForTheConnection) {
    Socket s = connect();
    s.write_all("SET big " + std::to_string(tcpkv::limits::kMaxValueLength + 1) + "\n");
    EXPECT_EQ(read_until_eof(s), "-ERR value too large\n");
}

TEST_F(ServerFixture, QuitClosesTheConnection) {
    Socket s = connect();
    s.write_all("QUIT\n");
    EXPECT_EQ(read_until_eof(s), "+BYE\n");
}

TEST_F(ServerFixture, PipelinedCommandsGetPipelinedReplies) {
    Socket s = connect();
    const std::string expected = "+PONG\n+OK\n$1\nx\n$-1\n:1\n";
    EXPECT_EQ(roundtrip(s, "PING\nSET a 1\nx\nGET a\nGET nope\nDEL a\n", expected), expected);
}

TEST_F(ServerFixture, BlankLinesAndCrLfAreTolerated) {
    Socket s = connect();
    EXPECT_EQ(roundtrip(s, "\r\n\nping\r\n", "+PONG\n"), "+PONG\n");
}

TEST_F(ServerFixture, ManyConcurrentClientsSeeTheirOwnWrites) {
    constexpr int kClients = 16;  // more clients than workers: some wait in the pool queue
    constexpr int kOps = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            try {
                Socket s = connect();
                for (int i = 0; i < kOps; ++i) {
                    const std::string key = "t" + std::to_string(c) + ":k" + std::to_string(i);
                    const std::string value = "v" + std::to_string(c * 1000 + i);
                    if (roundtrip(s, tcpkv::encode_set(key, value), "+OK\n") != "+OK\n") {
                        ++failures;
                    }
                    const std::string bulk = reply::bulk(value);
                    if (roundtrip(s, "GET " + key + "\n", bulk) != bulk) {
                        ++failures;
                    }
                }
                s.write_all("QUIT\n");
                read_until_eof(s);
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(store_.size(), static_cast<std::size_t>(kClients * kOps));
}

TEST(ServerShutdown, RequestStopUnblocksRunAndClosesClients) {
    tcpkv::log::set_level(tcpkv::log::Level::Warn);
    Store store;
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.threads = 2;
    config.connection.poll_interval = std::chrono::milliseconds(20);
    Server server(config, store);
    std::thread runner([&] { server.run(); });

    Socket idle_client = Socket::connect_tcp("127.0.0.1", server.port());
    Socket busy_client = Socket::connect_tcp("127.0.0.1", server.port());
    ASSERT_EQ(roundtrip(busy_client, "PING\n", "+PONG\n"), "+PONG\n");

    const auto t0 = std::chrono::steady_clock::now();
    server.request_stop();
    runner.join();  // must return promptly: the wake pipe interrupts poll(), workers see the flag
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_TRUE(server.stop_requested());

    // Both clients were closed by the server.
    EXPECT_EQ(read_until_eof(idle_client), "");
    EXPECT_EQ(read_until_eof(busy_client), "");
}

TEST(ServerShutdown, RequestStopBeforeRunReturnsImmediately) {
    tcpkv::log::set_level(tcpkv::log::Level::Warn);
    Store store;
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    Server server(config, store);
    server.request_stop();
    server.run();  // loop condition is false on entry
    EXPECT_TRUE(server.stop_requested());
}
```

### `CMakeLists.txt`

Add `src/thread_pool.cpp` and `src/server.cpp` to the library; add `thread_pool` and
`integration` to the test list.

## 4. Build and run

```bash
cmake --build --preset debug && ctest --preset debug
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
./build/debug/tcpkv_server --port 7379 --threads 4 --log-level debug
```

## 5. Test by hand

1. Two `nc` sessions at once: both get replies immediately. Four workers, so a fifth `nc`
   waits until one of the first four quits (`--threads 2` makes this easy to see).
2. With a session open, press Ctrl-C on the server. Expected log:
   `shutdown requested: 0 connections queued`, then `tcpkv stopped cleanly`; `nc` reports the
   connection closed within a quarter of a second. Nothing is killed.
3. `kill -TERM $(pgrep tcpkv_server)` behaves the same.
4. `--threads 1 --max-pending 2`, then open five `nc` sessions: the fourth and fifth get
   `-ERR server busy, try again later` and are closed.
5. `top -H -p $(pgrep tcpkv_server)` shows 1 + N threads. Idle, they use ~0% CPU (the 250 ms
   poll is cheap).

## 6. Commit

```bash
git add -A
git commit -m "Add ThreadPool, Server with graceful shutdown, pollable Connection

- ThreadPool: N workers, FIFO queue, MoveOnlyFunction tasks; stop() drops queued tasks,
  running tasks complete, destructor joins; constructor is exception-safe
- Connection: poll with bounded interval, observes shared stop flag, optional idle timeout
- Server: non-blocking listener + self-pipe wake; request_stop() is async-signal-safe;
  EMFILE back-off; busy rejection above max_pending_connections
- tcpkv_server: sigaction for SIGINT/SIGTERM, --threads/--host/--idle-timeout/--log-level
- tests: thread pool semantics; integration suite on an ephemeral port incl. shutdown latency"
git push
```

## 7. What you learned, and what to say in an interview

* **Condition variables** need a mutex, a predicate and a loop (`wait(lock, pred)`), or you
  get lost wakeups and spurious wakeups.
* **`std::function` needs copyable callables.** Type erasure in 40 lines fixes that; so does
  `std::move_only_function` in C++23.
* **A blocked thread cannot be told to stop.** Bound every blocking call (`poll` with a
  timeout) or have a way to wake it (the self-pipe).
* **Signal handlers may only do async-signal-safe things.** Atomic store + `write`. Never
  lock a mutex, never allocate, never log from one.
* **Shutdown order matters**: stop accepting, drop never-served work, let in-flight work
  finish, join. The scope of `pool` inside `run()` encodes that order.
* **Thread-per-connection is a real design** with a real limit (N concurrent connections).
  You know what the next step is (epoll) and why you did not start there.
* **`EMFILE` on accept** is a production incident waiting to happen; back off, do not spin.

Interview sentence: "A `Connection` is created on the acceptor thread, moved into a lambda,
queued, and executed on a worker; the socket closes on whichever thread destroys it, with no
reference counting. Shutdown is a self-pipe write from the signal handler; the acceptor
leaves its loop, the pool drops queued connections and joins workers that each notice the
flag within one poll interval. The integration test asserts that takes under two seconds
with clients connected; in practice it is about 20 milliseconds."
