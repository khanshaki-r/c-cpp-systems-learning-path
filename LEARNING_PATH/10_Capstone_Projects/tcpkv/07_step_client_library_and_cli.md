# Step 07. Client library and `tcpkv_cli`

**Time:** ~6 hours   **Week:** 4 (Oct 27 – Nov 02)
**Read first:** 08_Networking_Distributed: `04_framing_and_protocol_design.md` (length-prefixed
framing, why a reader needs a buffer), `02_tcp_in_depth.md` (Nagle's algorithm, delayed ACK,
`TCP_NODELAY`, "TCP is a byte stream, not a message stream"). 02_Modern_CPP:
`10_error_handling_exceptions_optional_expected.md` (`std::optional` for "absent" vs an
exception for "failed"), `04_value_semantics_copy_move.md` (defaulted move operations),
`13_cpp17_features_tour.md` (`std::from_chars`, `std::string_view`). 06_Linux_Toolchain_DevOps:
`07_debugging_and_profiling_tools_on_linux.md` (`strace -e trace=network`).

## 1. Goal

Give tcpkv a proper client side.

* A `Client` class in the library: connect, send a command, parse the reply into a typed
  `Reply`, with one method per command (`get`, `set`, `del`, `keys`, ...).
* `tcpkv_cli`, an interactive shell and one-shot tool in the spirit of `redis-cli`.
* The integration tests rewritten on top of `Client`, so they read like the protocol table
  instead of like byte arithmetic. Raw sockets stay only where a test needs to send something
  a well-behaved client never would.

In plain words: until now you talked to the server with `nc`, counting bytes for `SET` by
hand. Now you write the phone handset, not just the exchange. The client is also the code
that `load_gen` (step 08) uses to hammer the server, so it has to be correct before it can be
fast.

## 2. Design

### 2.1 The reply model

The server sends six kinds of reply (spec section 3.2). The client turns each into one
`Reply` value:

```
'+' text      -> Kind::Status   text
"-ERR " text  -> Kind::Error    text  (the "ERR " prefix stripped)
':' n         -> Kind::Integer  integer
'$' len bytes -> Kind::Bulk     text  (binary safe: may hold '\n' and '\0')
"$-1"         -> Kind::Null
'*' n {bulk}  -> Kind::Array    array
```

A plain struct with a `kind` tag, not a `std::variant`. The CLI needs to print every kind
and the typed helpers need to check one kind; a tag and three fields make both a one-line
`switch`. A variant would be equally correct and slightly more ceremony at every use.

### 2.2 Two layers of API, two ways to fail

```
typed helpers      get() set() del() exists() keys() expire() ttl() stats() quit()
                       |  throw ClientError on "-ERR" or on the wrong reply kind
                       v
execute(tokens) / send_raw(bytes)   -> Reply      (errors are data, never thrown)
                       |
read_reply()  read_line()  read_bulk()  fill()    (buffer_ holds unconsumed bytes)
                       |
Socket::write_all / read_some                     (std::system_error on transport failure)
```

* **`execute()` returns error replies as data.** The CLI wants to print `(error) unknown
  command 'FOO'` and carry on. Throwing for that would be using exceptions for control flow.
* **Typed helpers throw `ClientError`.** A program that calls `client.set(k, v)` and gets
  `-ERR` back has a bug or a broken server; returning a status it will forget to check is how
  C code loses errors. Your Extrieve code checks every return code by hand; here the type
  system does it.
* **`std::optional` for "not found".** `GET` on a missing key is not an error, it is an
  answer. `std::optional<std::string>` says that in the signature.
* **Transport failures stay `std::system_error`** from the `Socket` layer, carrying `errno`.
  The caller can tell "the server said no" (`ClientError`) from "the network broke"
  (`system_error`). The CLI uses that difference: the first keeps the session, the second ends it.

### 2.3 Why the client needs its own buffer

TCP delivers a byte stream. One `read()` may return half a reply, or one and a half replies
(if the server batched them). So the client keeps `buffer_`: bytes received but not yet
consumed. `read_line()` looks for `\n` in the buffer and only reads from the socket if there
is none. `read_bulk(n)` makes sure `n + 1` bytes are buffered, then takes them. Bytes left
over after one reply stay in `buffer_` for the next. This is the same idea as the server's
`Parser`, simpler because the client always knows what it is waiting for.

### 2.4 Nagle and `TCP_NODELAY`

The constructor calls `socket_.set_nodelay(true)`. Without it, a request/response client can
stall for tens of milliseconds per command. The mechanism: Nagle's algorithm holds back a
small segment while earlier data is unacknowledged; the receiver's delayed-ACK timer holds
back the ACK hoping to piggyback it on data. Each side waits for the other. Linux's delayed
ACK is typically up to 40 ms; on other stacks it is often 200 ms (see
08_Networking_Distributed/02_tcp_in_depth.md). `SET` with a big value is the classic victim:
the client writes the header and value as one buffer here (`encode_set` builds one string),
but any client that does two writes per request will hit it. `TCP_NODELAY` turns Nagle off.
The cost is more small packets, which is what a request/response protocol sends anyway.

### 2.5 The CLI

```
tcpkv_cli                       interactive: "tcpkv> " prompt, one command per line
tcpkv_cli -p 7400 set k hello   one-shot: runs one command, prints the reply, exits
```

* Inline commands are split on whitespace and sent with `execute()`.
* `SET key value...` is special: everything after the key, spaces included, is the value, and
  it goes through `client.set()` so the length prefix is computed for you. You never count
  bytes again.
* Replies are printed the way `redis-cli` prints them: `(integer) 3`, `(nil)`, `(error) ...`,
  bulk strings quoted with `\n`, `\t` and `\xNN` escapes so binary values cannot wreck your
  terminal.

## 3. Files to create

### `include/tcpkv/client.hpp`

Final version; it does not change in later steps.

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tcpkv/socket.hpp"

namespace tcpkv {

// Thrown for "-ERR ..." replies and for malformed or unexpected replies.
// Transport failures surface as std::system_error from the Socket layer.
class ClientError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Reply {
    enum class Kind { Status, Error, Integer, Bulk, Null, Array };
    Kind kind = Kind::Null;
    std::string text;                // Status, Error, Bulk
    long long integer = 0;           // Integer
    std::vector<std::string> array;  // Array
};

// Blocking, single-connection client. Not thread-safe: one Client per thread.
class Client {
public:
    Client(const std::string& host, std::uint16_t port);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    ~Client() = default;

    std::string ping();
    std::optional<std::string> get(const std::string& key);
    void set(const std::string& key, std::string_view value);
    long long del(const std::vector<std::string>& keys);
    long long exists(const std::vector<std::string>& keys);
    std::vector<std::string> keys(std::string_view prefix = {});
    bool expire(const std::string& key, long long seconds);
    long long ttl(const std::string& key);
    std::string stats();
    void quit();

    // Sends an inline command ("GET key") and returns whatever the server replied,
    // including error replies. Used by the CLI.
    Reply execute(const std::vector<std::string>& tokens);

    // Sends pre-encoded bytes and reads one reply.
    Reply send_raw(std::string_view wire);

private:
    Reply read_reply();
    std::string read_line();
    std::string read_bulk(std::size_t length);
    void fill(std::size_t needed);  // read from the socket until buffer_ holds >= needed bytes
    static Reply expect(Reply reply, Reply::Kind kind);

    Socket socket_;
    std::string buffer_;  // bytes received but not yet consumed
};

}  // namespace tcpkv
```

Points to be able to defend:

* **Move-only, like `Socket`.** A `Client` owns a connection. Copying it would mean two
  objects reading the same byte stream, each consuming the other's replies. The defaulted
  move operations are `noexcept` because `Socket`'s and `std::string`'s are, which lets a
  `std::vector<Client>` move rather than copy when it grows.
* **`expire`, `ttl` and `stats` are declared now** even though the server answers
  `-ERR not implemented yet` until step 09. The client is the protocol's other half; like the
  parser in step 03 it is written complete once. Until step 09 those three helpers throw
  `ClientError`, which is the honest behaviour.
* **"Not thread-safe: one Client per thread"** is a design decision, not laziness. Sharing one
  connection between threads needs either a lock around each request/response pair or request
  ids to match replies. A connection per thread is what `load_gen` does, and it is what
  connection pools in real client libraries hand out.

### `src/client.cpp`

Identical to `code/src/client.cpp`. The pieces to understand line by line:

```cpp
#include "tcpkv/client.hpp"

#include <charconv>
#include <system_error>
#include <utility>

#include "tcpkv/protocol.hpp"

namespace tcpkv {

namespace {

long long parse_integer_or_throw(std::string_view text) {
    long long value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        throw ClientError("malformed integer in reply: '" + std::string(text) + "'");
    }
    return value;
}

}  // namespace

Client::Client(const std::string& host, std::uint16_t port)
    : socket_(Socket::connect_tcp(host, port)) {
    // Request/response protocol: every command is a small write that must leave immediately.
    socket_.set_nodelay(true);
}
```

`std::from_chars` rather than `atoll` or `stoll`: no locale, no exceptions, no silent
acceptance of `"12abc"` (the `result.ptr != end` check rejects trailing junk), and no
undefined behaviour on overflow (it reports `errc::result_out_of_range`). The server's
parser uses the same function for the same reasons.

The typed helpers are one line each. Two examples:

```cpp
std::optional<std::string> Client::get(const std::string& key) {
    Reply reply = execute({"GET", key});
    if (reply.kind == Reply::Kind::Null) {
        return std::nullopt;
    }
    return expect(std::move(reply), Reply::Kind::Bulk).text;
}

void Client::set(const std::string& key, std::string_view value) {
    expect(send_raw(encode_set(key, value)), Reply::Kind::Status);
}
```

`set` reuses `encode_set` from `protocol.hpp` (step 03), the same function the protocol
tests round-trip through the parser. One encoder, tested once, used by both sides.

The reply parser:

```cpp
Reply Client::read_reply() {
    const std::string line = read_line();
    if (line.empty()) {
        throw ClientError("empty reply line");
    }
    const std::string_view payload(line.data() + 1, line.size() - 1);

    Reply reply;
    switch (line[0]) {
        case '+':
            reply.kind = Reply::Kind::Status;
            reply.text = payload;
            break;
        case '-':
            reply.kind = Reply::Kind::Error;
            // Strip the "ERR " prefix so callers see just the message.
            reply.text = payload.substr(0, 4) == "ERR " ? payload.substr(4) : payload;
            break;
        case ':':
            reply.kind = Reply::Kind::Integer;
            reply.integer = parse_integer_or_throw(payload);
            break;
        case '$': {
            const long long length = parse_integer_or_throw(payload);
            if (length < 0) {
                reply.kind = Reply::Kind::Null;
            } else {
                reply.kind = Reply::Kind::Bulk;
                reply.text = read_bulk(static_cast<std::size_t>(length));
            }
            break;
        }
        case '*': {
            const long long count = parse_integer_or_throw(payload);
            if (count < 0) {
                throw ClientError("negative array length in reply");
            }
            reply.kind = Reply::Kind::Array;
            reply.array.reserve(static_cast<std::size_t>(count));
            for (long long i = 0; i < count; ++i) {
                const std::string header = read_line();
                if (header.empty() || header[0] != '$') {
                    throw ClientError("array element is not a bulk string");
                }
                const long long length =
                    parse_integer_or_throw(std::string_view(header).substr(1));
                if (length < 0) {
                    throw ClientError("null element inside array reply");
                }
                reply.array.push_back(read_bulk(static_cast<std::size_t>(length)));
            }
            break;
        }
        default:
            throw ClientError("malformed reply: '" + line + "'");
    }
    return reply;
}
```

`payload` is a `string_view` into `line`, which lives until the end of the function, so the
view never dangles. Every branch that copies out of it (`reply.text = payload`) copies before
`line` dies.

The buffer management:

```cpp
std::string Client::read_line() {
    for (;;) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (buffer_.size() > limits::kMaxLineLength) {
            throw ClientError("reply line too long");
        }
        fill(buffer_.size() + 1);
    }
}

std::string Client::read_bulk(std::size_t length) {
    if (length > limits::kMaxValueLength) {
        throw ClientError("bulk reply larger than protocol limit");
    }
    fill(length + 1);
    if (buffer_[length] != '\n') {
        throw ClientError("bulk reply not terminated by newline");
    }
    std::string value = buffer_.substr(0, length);
    buffer_.erase(0, length + 1);
    return value;
}

void Client::fill(std::size_t needed) {
    char chunk[16 * 1024];
    while (buffer_.size() < needed) {
        const std::size_t n = socket_.read_some(chunk, sizeof chunk);
        if (n == 0) {
            throw ClientError("connection closed by server");
        }
        buffer_.append(chunk, n);
    }
}

Reply Client::expect(Reply reply, Reply::Kind kind) {
    if (reply.kind == Reply::Kind::Error) {
        throw ClientError(reply.text);
    }
    if (reply.kind != kind) {
        throw ClientError("unexpected reply type from server");
    }
    return reply;
}
```

Three defensive limits mirror the server's: a reply line longer than 4096 bytes, a bulk
longer than 1 MiB, and a bulk not followed by `\n` are all rejected. A client is also a
network parser and also faces untrusted input (a buggy or hostile server, or the wrong port:
point the CLI at an HTTP server and see what happens). Without the line limit, `read_line`
would grow `buffer_` forever against a peer that never sends `\n`.

`fill(buffer_.size() + 1)` means "at least one more byte than I have". It may get far more;
the extra stays buffered.

`buffer_.erase(0, n)` is O(remaining bytes). For a client that reads one reply at a time the
remainder is usually empty, so this is fine. The server's `Parser` uses a read offset and
compacts occasionally because it may hold many pipelined requests; know the difference.

### `apps/tcpkv_cli.cpp`

Identical to `code/apps/tcpkv_cli.cpp`. The important parts:

```cpp
// Prints bytes the way redis-cli does: quoted, with escapes for anything that is not printable.
std::string quoted(std::string_view bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u >= 0x20 && u < 0x7F) {
                    out += c;
                } else {
                    out += "\\x";
                    out += kHex[u >> 4];
                    out += kHex[u & 0x0F];
                }
        }
    }
    out += '"';
    return out;
}
```

The `static_cast<unsigned char>` matters. `char` is signed on x86 Linux, so byte `0xFF` is
`-1` as a `char`; `-1 >> 4` is `-1` and `kHex[-1]` reads before the array. The same cast
appears in `upper()` because `std::toupper` with a negative argument other than `EOF` is
undefined behaviour. This is a C trap you already know; C++ inherited it unchanged.

```cpp
// Executes one line. Returns false when the session should end.
bool run_line(tcpkv::Client& client, const std::string& line, std::ostream& os) {
    std::istringstream in(line);
    std::string command;
    if (!(in >> command)) {
        return true;  // blank line
    }
    const std::string name = upper(command);
    if (name == "EXIT") {
        try {
            client.quit();
        } catch (const std::exception&) {
            // Leaving anyway.
        }
        return false;
    }

    try {
        if (name == "SET") {
            std::string key;
            if (!(in >> key)) {
                os << "(error) usage: SET key value\n";
                return true;
            }
            std::string value;
            std::getline(in, value);  // the rest of the line, starting with the separating space
            if (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
            client.set(key, value);
            os << "OK\n";
            return true;
        }

        std::vector<std::string> tokens = split(line);
        tokens[0] = name;
        const tcpkv::Reply reply = client.execute(tokens);
        print_reply(reply, os);
        return !(name == "QUIT" && reply.kind == tcpkv::Reply::Kind::Status);
    } catch (const tcpkv::ClientError& e) {
        os << "(error) " << e.what() << '\n';
        return true;
    } catch (const std::system_error& e) {
        os << "(connection error) " << e.what() << '\n';
        return false;
    }
}
```

The two `catch` clauses are the two failure classes from section 2.2: `ClientError` keeps the
session, `std::system_error` ends it. `run_line` takes an `std::ostream&` rather than writing
to `std::cout` directly so it could be unit-tested with an `std::ostringstream` (exercise 3).

In `main`, the option loop stops at the first non-option argument, and one-shot mode glues
the remaining `argv` back into one line, so `tcpkv_cli set greeting hello world` stores
`hello world`:

```cpp
        if (i < argc) {
            // One-shot mode: rebuild the command line so SET's value keeps its spaces.
            std::string line;
            for (int j = i; j < argc; ++j) {
                if (j != i) {
                    line += ' ';
                }
                line += argv[j];
            }
            run_line(client, line, std::cout);
            return 0;
        }
```

Port parsing uses `std::from_chars` into an `unsigned` and rejects values above 65535 before
narrowing to `std::uint16_t`. Narrowing first would turn `-p 70000` into port 4464 silently.

### `tests/integration_test.cpp` (step-07 version: `Client` instead of raw sockets)

The server still takes `(config, store)`; `Metrics` arrives in step 09. Raw sockets remain for
the tests that must send bytes a correct client never sends (oversized header, pipelining,
`\r\n` and blank lines) or must observe the connection closing.

```cpp
// End-to-end tests: a real Server on an ephemeral port, real sockets, real threads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/client.hpp"
#include "tcpkv/log.hpp"
#include "tcpkv/protocol.hpp"
#include "tcpkv/server.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

using tcpkv::Client;
using tcpkv::ClientError;
using tcpkv::Reply;
using tcpkv::Server;
using tcpkv::ServerConfig;
using tcpkv::Socket;
using tcpkv::Store;

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
    Client connect() { return Client("127.0.0.1", port()); }
    Socket raw_connect() { return Socket::connect_tcp("127.0.0.1", port()); }

    Store store_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

}  // namespace

TEST_F(ServerFixture, PingPong) {
    Client client = connect();
    EXPECT_EQ(client.ping(), "PONG");
}

TEST_F(ServerFixture, SetGetDelRoundTrip) {
    Client client = connect();
    EXPECT_FALSE(client.get("k").has_value());
    client.set("k", "hello");
    EXPECT_EQ(client.get("k").value_or(""), "hello");
    EXPECT_EQ(client.exists({"k", "missing"}), 1);
    EXPECT_EQ(client.del({"k", "missing"}), 1);
    EXPECT_FALSE(client.get("k").has_value());
}

TEST_F(ServerFixture, ValuesAreBinarySafe) {
    Client client = connect();
    const std::string value("line one\nline two\r\n\x00\xff tail", 26);
    client.set("bin", value);
    EXPECT_EQ(client.get("bin").value_or(""), value);

    const std::string big(tcpkv::limits::kMaxValueLength, 'B');
    client.set("big", big);
    EXPECT_EQ(client.get("big").value_or(""), big);
}

TEST_F(ServerFixture, KeysWithPrefix) {
    Client client = connect();
    client.set("user:1", "a");
    client.set("user:2", "b");
    client.set("order:1", "c");
    const std::vector<std::string> users = {"user:1", "user:2"};
    EXPECT_EQ(client.keys("user:"), users);
    EXPECT_EQ(client.keys().size(), 3u);
}

TEST_F(ServerFixture, UnknownCommandIsAnErrorButKeepsTheConnection) {
    Client client = connect();
    const Reply reply = client.execute({"FROB", "x"});
    EXPECT_EQ(reply.kind, Reply::Kind::Error);
    EXPECT_NE(reply.text.find("unknown command"), std::string::npos);
    EXPECT_EQ(client.ping(), "PONG");  // same connection still works
}

TEST_F(ServerFixture, ErrorRepliesSurfaceAsClientError) {
    Client client = connect();
    // execute() hands back error replies as data; the typed helpers turn them into exceptions.
    EXPECT_EQ(client.execute({"GET"}).kind, Reply::Kind::Error);  // wrong arity
    EXPECT_THROW(client.ttl("has space"), ClientError);           // becomes "TTL has space": arity error
}

TEST_F(ServerFixture, NotYetImplementedCommandsReplyWithError) {  // deleted in step 09
    Client client = connect();
    const Reply reply = client.execute({"TTL", "k"});
    EXPECT_EQ(reply.kind, Reply::Kind::Error);
    EXPECT_EQ(reply.text, "not implemented yet");
    EXPECT_THROW(client.stats(), ClientError);
}

TEST_F(ServerFixture, OversizedSetHeaderIsFatalForTheConnection) {
    Socket raw = raw_connect();
    raw.write_all("SET big " + std::to_string(tcpkv::limits::kMaxValueLength + 1) + "\n");
    EXPECT_EQ(read_until_eof(raw), "-ERR value too large\n");
}

TEST_F(ServerFixture, QuitClosesTheConnection) {
    Socket raw = raw_connect();
    raw.write_all("QUIT\n");
    EXPECT_EQ(read_until_eof(raw), "+BYE\n");
}

TEST_F(ServerFixture, PipelinedCommandsGetPipelinedReplies) {
    Socket raw = raw_connect();
    raw.write_all("PING\nSET a 1\nx\nGET a\nGET nope\nDEL a\n");
    const std::string expected = "+PONG\n+OK\n$1\nx\n$-1\n:1\n";
    EXPECT_EQ(read_exactly(raw, expected.size()), expected);
}

TEST_F(ServerFixture, BlankLinesAndCrLfAreTolerated) {
    Socket raw = raw_connect();
    raw.write_all("\r\n\nping\r\n");
    EXPECT_EQ(read_exactly(raw, 6), "+PONG\n");
}

TEST_F(ServerFixture, ManyConcurrentClientsSeeTheirOwnWrites) {
    constexpr int kClients = 16;  // more clients than workers: some wait in the pool queue
    constexpr int kOps = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            try {
                Client client = connect();
                for (int i = 0; i < kOps; ++i) {
                    const std::string key = "t" + std::to_string(c) + ":k" + std::to_string(i);
                    const std::string value = "v" + std::to_string(c * 1000 + i);
                    client.set(key, value);
                    if (client.get(key).value_or("") != value) {
                        ++failures;
                    }
                }
                client.quit();
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
    busy_client.write_all("PING\n");
    ASSERT_EQ(read_exactly(busy_client, 6), "+PONG\n");

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

Why `Client connect()` can return by value: `Client` is move-only and C++17 guarantees copy
elision for a returned prvalue, so `Client client = connect();` constructs once, in place.
That is the same guarantee that lets `Socket::connect_tcp` return a `Socket`.

The `ManyConcurrentClientsSeeTheirOwnWrites` test is now also a test of the client: sixteen
`Client`s on sixteen threads, each with its own buffer, none shared. If you ever make `Client`
share state between instances (a static buffer, say), this test fails under TSan.

### `CMakeLists.txt`

Add `src/client.cpp` to the `tcpkv` library sources, and the CLI executable:

```cmake
add_executable(tcpkv_cli apps/tcpkv_cli.cpp)
target_link_libraries(tcpkv_cli PRIVATE tcpkv tcpkv_warnings)
```

Optionally add the install rule from the final tree now:
`install(TARGETS tcpkv_server tcpkv_cli RUNTIME DESTINATION bin)`.

## 4. Build and run (WSL Ubuntu 24.04)

Work in your repo under `/home/shakir/dev/tcpkv`, not under `/mnt/d`: the Windows file system
through WSL is many times slower for builds and `git status`.

```bash
cd ~/dev/tcpkv
cmake --build --preset debug && ctest --preset debug
cmake --build --preset asan && ctest --preset asan

# terminal 1
./build/debug/tcpkv_server --port 7379 --threads 4

# terminal 2
./build/debug/tcpkv_cli
./build/debug/tcpkv_cli -p 7379 set greeting hello world
./build/debug/tcpkv_cli get greeting
```

## 5. Test by hand

1. Interactive session. Expected output, exactly:

   ```
   tcpkv> set greeting hello world
   OK
   tcpkv> get greeting
   "hello world"
   tcpkv> get nope
   (nil)
   tcpkv> exists greeting nope
   (integer) 1
   tcpkv> keys gr
   1) "greeting"
   tcpkv> frob
   (error) unknown command 'FROB'
   tcpkv> ttl greeting
   (error) not implemented yet
   tcpkv> quit
   BYE
   ```

2. Binary value from `nc`, displayed by the CLI:
   `printf 'SET bin 5\na\tb\x01c\n' | nc -q1 127.0.0.1 7379`, then
   `./build/debug/tcpkv_cli get bin` prints `"a\tb\x01c"`.
3. Nagle off: `strace -f -e trace=setsockopt ./build/debug/tcpkv_cli ping` shows
   `setsockopt(3, SOL_TCP, TCP_NODELAY, [1], 4) = 0`.
4. One write per command: `strace -f -e trace=sendto,read ./build/debug/tcpkv_cli set k v`
   shows one `sendto` carrying `"SET k 1\nv\n"` (glibc implements `send()` with the `sendto`
   syscall) and one `read` on the socket returning `"+OK\n"` (`read_some` uses plain `read`).
5. Server dies mid-session: start the CLI, Ctrl-C the server, type `ping`. Expected:
   `(error) connection closed by server` (the read saw EOF) or `(connection error) ...`
   (the write saw `EPIPE`/`ECONNRESET`), depending on timing. Know why both are possible:
   the first write after the peer closed usually succeeds into the local send buffer, and
   only the next read or write learns about the RST.
6. Wrong port: `./build/debug/tcpkv_cli -p 1 ping` prints
   `could not connect to 127.0.0.1:1: ...Connection refused` and exits 1.

## 6. Commit

```bash
git add -A
git commit -m "Add blocking Client library and tcpkv_cli; port integration tests to Client

- Client: typed helpers per command (optional for GET, ClientError on -ERR), execute() and
  send_raw() return replies as data; buffered reply parser with protocol limits;
  TCP_NODELAY; move-only
- tcpkv_cli: interactive and one-shot modes, SET takes the rest of the line as the value,
  redis-cli style output with escaped binary
- integration tests use Client; raw sockets only for malformed input and close semantics"
git push
```

## 7. What you learned, and what to say in an interview

* **TCP is a byte stream.** Both ends need a buffer and a framing rule. The client keeps
  unconsumed bytes between replies exactly as the server's parser does.
* **Two failure classes deserve two types.** Protocol errors (`ClientError`) and transport
  errors (`std::system_error`) need different handling; one exception type would force string
  matching.
* **`std::optional` for absence, exceptions for failure, data for the CLI.** Each API layer
  picks the error style that fits its caller.
* **`TCP_NODELAY`** for request/response protocols, and you can explain the Nagle +
  delayed-ACK stall from first principles.
* **A client is a parser of untrusted input too.** Limits on line and bulk length are not
  optional on either side.
* **`static_cast<unsigned char>` before `toupper` and table lookups** because `char` is
  signed on x86.

Interview sentence: "The client library is the protocol's other half: a buffered reply
parser with the same limits as the server, typed helpers that return `std::optional` for a
missing key and throw `ClientError` for an error reply, and a lower `execute()` layer that
returns errors as data, which is what the CLI and the load generator use. It is move-only
because it owns a connection, not thread-safe by design (one client per thread, like a pool),
and it sets `TCP_NODELAY` because a request/response protocol over Nagle and delayed ACK can
stall for the delayed-ACK timeout on every request."
