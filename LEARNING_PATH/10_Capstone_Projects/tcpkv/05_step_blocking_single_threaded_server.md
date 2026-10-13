# Step 05. A blocking single-threaded server, tested by hand with `nc`

**Time:** ~5 hours   **Week:** 3 (Oct 20 – Oct 26)
**Read first:** 08_Networking_Distributed: the accept loop, `nc`/`netcat`, `ss -tlnp`,
`tcpdump -i lo`, what `ECONNRESET` and `EPIPE` mean. 06_Linux_Toolchain_DevOps: `strace -f`,
signals (`SIGINT`, `SIGPIPE`). 02_Modern_CPP: fold expressions (used in the logger), `std::ostringstream`.

## 1. Goal

The first thing a client can talk to. One listener, an accept loop, and one `Connection`
object that reads bytes, runs them through the parser, executes commands against the store,
and writes replies. Single-threaded and blocking: it serves one client at a time. That is
deliberate. Get the request/response path right in the simplest possible shape, then add
concurrency in step 06 without changing it.

In plain words: wire the parser (step 03) to the store (step 04) through a socket (step 02).

## 2. Design

```
main: listen_tcp(port) -> loop { Socket c = accept(); Connection(c, store).serve(); }

Connection::serve():
   loop:
     n = socket.read_some(buf)        0 -> peer closed -> return
     parser.feed(buf, n)
     out = ""
     for each event in parser:        Command -> out += execute_command(cmd, store)
                                      ParseError -> out += "-ERR ..."; fatal -> close after write
     socket.write_all(out)            one write per read(), so pipelined requests get one reply segment
```

`execute_command` is a free function `(Command, Store&) -> std::string` so it can be tested
without a socket and reused by anything else later (it is the only place that knows what a
command *does*). It takes the `Command` by value so SET can move the key and value into the
store without copying a 1 MiB payload.

A tiny logger goes in now too, because a server that prints nothing is undebuggable. It is
final: step 09 does not change it.

## 3. Files to create

### `include/tcpkv/log.hpp` and `src/log.cpp`

Identical to `code/include/tcpkv/log.hpp` and `code/src/log.cpp`. The interesting bit is
the variadic `format`:

```cpp
template <class... Args>
std::string format(const Args&... args) {
    std::ostringstream os;
    (os << ... << args);   // C++17 fold expression: os << a << b << c ...
    return os.str();
}

template <class... Args>
void info(const Args&... args) {
    if (level() <= Level::Info) write(Level::Info, format(args...));
}
```

No format strings means no `%s`/`%d` mismatches, which `-Wformat` cannot catch for
`std::string` arguments anyway. `write()` builds the whole line, then takes a mutex and
emits it with one `fwrite`, so lines from different threads never interleave. The thread id
printed is the kernel tid (`gettid()`), the same number `top -H` and `gdb` show.

### `include/tcpkv/connection.hpp` (step-05 version)

```cpp
#pragma once

#include <string>

#include "tcpkv/protocol.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

namespace tcpkv {

// Runs one parsed command against the store and returns the encoded reply.
// Takes the Command by value so SET can move the key and value into the store without copying.
// Free function so it can be unit-tested without a socket.
std::string execute_command(Command command, Store& store);

// One client connection. Move-only: it owns the Socket.
class Connection {
public:
    Connection(Socket socket, Store& store) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    ~Connection() = default;

    // Blocks until the peer disconnects, sends QUIT, or triggers a fatal protocol error.
    void serve();

private:
    // Drains every complete event from the parser into `out`. Returns false when the
    // connection must close after `out` has been sent.
    bool process_events(Parser& parser, std::string& out);

    Socket socket_;
    Store* store_;  // pointer, not reference, so the class stays move-assignable
};

}  // namespace tcpkv
```

### `src/connection.cpp` (step-05 version)

```cpp
#include "tcpkv/connection.hpp"

#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "tcpkv/log.hpp"

namespace tcpkv {

std::string execute_command(Command command, Store& store) {
    switch (command.type) {
        case CommandType::Ping:
            return reply::status("PONG");

        case CommandType::Get: {
            const std::optional<std::string> value = store.get(command.args[0]);
            return value ? reply::bulk(*value) : reply::null();
        }

        case CommandType::Set:
            store.set(std::move(command.args[0]), std::move(command.value));
            return reply::status("OK");

        case CommandType::Del: {
            long long removed = 0;
            for (const std::string& key : command.args) {
                removed += store.del(key) ? 1 : 0;
            }
            return reply::integer(removed);
        }

        case CommandType::Exists: {
            long long present = 0;
            for (const std::string& key : command.args) {
                present += store.exists(key) ? 1 : 0;
            }
            return reply::integer(present);
        }

        case CommandType::Keys:
            return reply::array(store.keys(command.args.empty() ? std::string_view{}
                                                                : std::string_view{command.args[0]}));

        case CommandType::Expire:
        case CommandType::Ttl:
        case CommandType::Stats:
            return reply::error("not implemented yet");  // step 09

        case CommandType::Quit:
            return reply::status("BYE");
    }
    return reply::error("internal error: unhandled command");  // unreachable; keeps -Wreturn-type quiet
}

Connection::Connection(Socket socket, Store& store) noexcept
    : socket_(std::move(socket)), store_(&store) {}

void Connection::serve() {
    const std::string peer = socket_.peer_address();
    log::info("connection from ", peer);

    Parser parser;
    std::vector<char> buffer(16 * 1024);
    std::string out;

    try {
        for (;;) {
            const std::size_t n = socket_.read_some(buffer.data(), buffer.size());
            if (n == 0) {
                break;  // peer closed its side
            }
            parser.feed(buffer.data(), n);

            out.clear();
            const bool keep_open = process_events(parser, out);
            if (!out.empty()) {
                // One write for every reply produced by this read: pipelined clients get
                // one segment back instead of one per command.
                socket_.write_all(out);
            }
            if (!keep_open) {
                break;
            }
        }
    } catch (const std::system_error& e) {
        // ECONNRESET and EPIPE are normal here: the client went away mid-conversation.
        log::info("connection ", peer, " error: ", e.what());
    }

    socket_.shutdown_both();
    log::info("connection ", peer, " closed");
}

bool Connection::process_events(Parser& parser, std::string& out) {
    while (std::optional<ParseEvent> event = parser.next()) {
        if (const ParseError* error = std::get_if<ParseError>(&*event)) {
            out += reply::error(error->message);
            if (error->fatal) {
                return false;
            }
            continue;
        }

        Command& command = std::get<Command>(*event);
        const bool quit = command.type == CommandType::Quit;
        out += execute_command(std::move(command), *store_);
        if (quit) {
            return false;
        }
    }
    return true;
}

}  // namespace tcpkv
```

Why a `switch` with every enumerator and no `default`: GCC's `-Wswitch` (in `-Wall`) then
warns when a new `CommandType` is added and not handled. A `default:` would silence that
safety net. The `return` after the switch exists because the compiler cannot prove the switch
is exhaustive for an `enum class` with an out-of-range value.

### `apps/tcpkv_server.cpp` (step-05 version)

```cpp
// tcpkv_server, step 05: single-threaded, blocking. One client at a time.

#include <charconv>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>

#include "tcpkv/connection.hpp"
#include "tcpkv/log.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

int main(int argc, char** argv) {
    std::uint16_t port = 7379;
    if (argc == 3 && std::string_view(argv[1]) == "--port") {
        const std::string_view text = argv[2];
        unsigned value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value > 65535) {
            std::cerr << "invalid port\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(value);
    } else if (argc != 1) {
        std::cerr << "usage: tcpkv_server [--port N]\n";
        return 2;
    }

    // A write to a socket whose peer vanished would otherwise kill the process.
    std::signal(SIGPIPE, SIG_IGN);

    try {
        tcpkv::Store store;
        tcpkv::Socket listener = tcpkv::Socket::listen_tcp("0.0.0.0", port, 128);
        tcpkv::log::info("tcpkv (single-threaded) listening on port ", listener.local_port());

        for (;;) {
            tcpkv::Socket client = listener.accept();
            tcpkv::Connection connection(std::move(client), store);
            connection.serve();  // blocks until this client leaves: one at a time (fixed in step 06)
        }
    } catch (const std::exception& e) {
        tcpkv::log::error("fatal: ", e.what());
        return 1;
    }
}
```

### `CMakeLists.txt`

Add `src/log.cpp` and `src/connection.cpp` to the library, and the executable:

```cmake
add_executable(tcpkv_server apps/tcpkv_server.cpp)
target_link_libraries(tcpkv_server PRIVATE tcpkv tcpkv_warnings)
```

## 4. Build and run

```bash
cmake --build --preset debug
./build/debug/tcpkv_server --port 7379
```

Expected: `... INFO  [tid NNNN] tcpkv (single-threaded) listening on port 7379`.

## 5. Test by hand

In a second terminal:

```bash
nc localhost 7379
```

Type (each line followed by Enter):

```
PING
SET greeting 11
hello world
GET greeting
GET missing
DEL greeting missing
KEYS
FOO
SET k 2
abc
```

Expected replies: `+PONG`, `+OK`, `$11` + `hello world`, `$-1`, `:1`, `*0`,
`-ERR unknown command 'FOO'`, then `-ERR value not terminated by newline` and `nc` exits
because the server closed the connection (fatal error). The server log shows the connection
open and close.

Now the limits of this step, on purpose:

1. Open two `nc` sessions. The second one connects (the kernel accepts into the backlog) but
   gets no reply until the first one quits. That is the single thread. Step 06 fixes it.
2. Press Ctrl-C on the server. It dies immediately, mid-connection, and `nc` reports the
   connection closed. Not graceful. Step 06 fixes that too.
3. Run `strace -f -e trace=accept4,read,sendto,close -p $(pgrep tcpkv_server)` while using
   `nc`. Watch one `read`, one `sendto` per command. Pipelining check: paste three lines at
   once into `nc`; you should see one `read` and one `sendto` with all three replies.
4. `ss -tlnp | grep 7379` shows the listener; `ss -tnp | grep 7379` shows connections.

Optional: `python3 -c "import socket; s=socket.create_connection(('127.0.0.1',7379)); s.sendall(b'SET k 3\nabc\nGET k\n'); print(s.recv(100))"`
gives `b'+OK\n$3\nabc\n'`.

## 6. Commit

```bash
git add -A
git commit -m "Add blocking single-threaded server: Connection, execute_command, logger

- Connection owns its Socket (move-only), feeds the Parser, batches replies per read
- execute_command(Command, Store&) is a free function: testable without a socket
- fatal parse errors reply then close; non-fatal reply and continue
- log: levelled, thread-safe, one fwrite per line, kernel tid
- tcpkv_server --port N; serves one client at a time (thread pool comes next)"
git push
```

## 7. What you learned, and what to say in an interview

* The **request/response loop** of every TCP server: read, parse incrementally, execute,
  batch replies, write, repeat; EOF ends it; any socket error ends it.
* **Pipelining falls out** of an incremental parser plus "one write per read".
* **Separation**: the parser knows bytes, `execute_command` knows semantics, `Connection`
  knows the socket. Each is testable alone.
* **`SIGPIPE`** is the classic way a server dies without a stack trace; ignore it or use
  `MSG_NOSIGNAL`. tcpkv does both.
* **Exhaustive `switch` without `default`** turns "forgot to handle the new command" into a
  compiler error under `-Werror`.

Interview sentence: "I built the server single-threaded first so the request path could be
verified with `nc` and `strace` before any concurrency was added. The connection handler
did not change shape afterwards; the thread pool just runs many of them."
