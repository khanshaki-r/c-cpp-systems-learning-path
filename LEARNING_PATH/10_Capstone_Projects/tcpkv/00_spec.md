# tcpkv specification

**Status:** this is the contract. Every step builds a piece of it; `code/` implements all of it.
If a step and this document disagree, this document wins and the step has a bug.

## 1. In plain words

tcpkv is a program that listens on a TCP port, keeps a table of keys and values in memory,
and lets many clients at once read and write that table by sending short text commands.
Think of it as the smallest useful cousin of Redis: `SET name shakir`, `GET name`, done.

The point is not the feature list. The point is that every part of a real server is present
and done properly: a wire protocol with limits and error handling, a store that is safe to
share between threads, sockets wrapped so descriptors cannot leak, a pool of worker threads,
a shutdown that does not lose or corrupt anything, tests for each piece, and a build that is
warning-free and sanitizer-clean.

## 2. Goals and non-goals

Goals:

* Correct under concurrent load: no data races, no torn values, no leaked descriptors or memory.
* Small: one library, two executables, two tools, five test binaries. About 3,500 lines.
* Everything explainable: no dependency except GoogleTest, no framework, no magic.

Non-goals (and things to say when asked "why not"):

* Persistence. Memory only. (minidb is the persistence project.)
* Replication, clustering, authentication, TLS.
* Beating Redis on throughput. A thread-per-connection pool cannot; see `INTERVIEW_TALKING_POINTS.md`.

## 3. Protocol

### 3.1 Design decision: text, line-based, length-prefixed values

Three options were considered for values that may contain spaces or newlines:

| option                       | pros                                        | cons                                                      |
|------------------------------|---------------------------------------------|-----------------------------------------------------------|
| quoting and escaping (`"a\nb"`) | readable in `nc`                         | escaping is the classic source of parser bugs; O(n) rescans; still cannot carry arbitrary bytes without a second escape layer |
| binary framing (fixed header)| fastest to parse                            | useless with `nc`/`telnet`; harder to debug by eye        |
| **length prefix** (`SET k 5\nhello\n`) | binary-safe, O(1) to find the end, trivially pipelined, still typeable in `nc` | the client must count bytes (the library does it) |

Length prefix wins. It is what Redis's RESP does for bulk strings and for the same reasons.
Commands themselves stay inline (`GET key`) because keys are restricted to printable ASCII
without whitespace, so there is nothing to escape.

### 3.2 Grammar

```
request   := inline '\n'
           | "SET" SP key SP length '\n' bytes '\n'      ; bytes is exactly `length` octets
inline    := command { SP arg }                          ; blank lines are ignored
SP        := one or more ' ' or '\t'
EOL       := '\n' optionally preceded by '\r'
command   := PING | GET | SET | DEL | EXISTS | KEYS | EXPIRE | TTL | STATS | QUIT   (case-insensitive)
key       := 1..256 octets, each in 0x21..0x7E  (printable ASCII, no space)
prefix    := 0..256 octets, each in 0x21..0x7E
length    := decimal integer 0..1048576
seconds   := decimal integer, optional leading '-'

reply     := '+' text EOL                 ; status        (+PONG, +OK, +BYE)
           | "-ERR " text EOL             ; error
           | ':' integer EOL              ; integer (may be negative)
           | '$' length EOL bytes EOL     ; bulk string (binary safe)
           | "$-1" EOL                    ; null (key not found)
           | '*' count EOL { bulk }       ; array of bulk strings
```

The server always emits `\n` (never `\r\n`). It accepts either from clients.

### 3.3 Commands

| request                  | reply                                                            | notes |
|--------------------------|------------------------------------------------------------------|-------|
| `PING`                   | `+PONG`                                                          | |
| `GET key`                | bulk value, or `$-1`                                             | expired keys count as missing |
| `SET key len` + value    | `+OK`                                                            | overwrites; clears any TTL |
| `DEL key [key ...]`      | `:n` number actually removed                                     | 1..64 keys |
| `EXISTS key [key ...]`   | `:n` number present                                              | 1..64 keys |
| `KEYS [prefix]`          | array of matching keys, sorted ascending                         | empty prefix = all keys |
| `EXPIRE key seconds`     | `:1` if the key exists (TTL set), `:0` otherwise                 | `seconds <= 0` deletes the key |
| `TTL key`                | `:s` seconds left (rounded up), `:-1` no TTL, `:-2` missing      | |
| `STATS`                  | bulk string, `name value\n` per line                             | see section 7 |
| `QUIT`                   | `+BYE`, then the server closes the connection                    | |

### 3.4 Errors

Non-fatal: the server replies `-ERR ...` and keeps the connection. The offending line is skipped.

* `unknown command 'NAME'`
* `wrong number of arguments for NAME`, `NAME takes no arguments`
* `invalid key`, `invalid prefix`, `invalid value length`, `invalid expire seconds`
* `too many arguments` (more than 64 arguments)

Fatal: the server replies `-ERR ...` and closes the connection. After these, the server cannot
know where the next request begins, so the only safe action is to hang up.

* `value too large` (declared length > 1 MiB)
* `line too long` (4096 bytes received without a newline)
* `value not terminated by newline` (the byte after the value is neither `\n` nor `\r\n`)

### 3.5 Limits

| limit              | value       | reason |
|--------------------|-------------|--------|
| key length         | 1..256 B    | keys are hashed and copied often; 256 is generous for `type:id` style keys |
| value length       | 0..1 MiB    | bounds per-connection memory to a few MiB; larger blobs belong in a different system |
| line length        | 4096 B      | 64 keys of 60 bytes fits; stops a client from growing our buffer forever |
| arguments          | 64          | caps the per-line work for DEL/EXISTS |
| pipelining         | unbounded   | the parser is incremental; replies are batched per read() |

### 3.6 Session example

```
C: SET greeting 11
C: hello world
S: +OK
C: GET greeting
S: $11
S: hello world
C: GET missing
S: $-1
C: DEL greeting missing
S: :1
C: FOO
S: -ERR unknown command 'FOO'
C: QUIT
S: +BYE
```

## 4. Architecture

```
            main thread                          worker threads (fixed pool, N)
 +-------------------------------+     +----------------------------------------------+
 | tcpkv_server                  |     | Connection::serve()                          |
 |  parse flags                  |     |   loop:                                      |
 |  Store, Metrics, Server       |     |     if stop flag set: break                  |
 |  sigaction(SIGINT, SIGTERM)   |     |     poll(fd, 250 ms) ; timeout -> continue   |
 |  server.run():                |     |     n = read()      ; 0 -> peer closed       |
 |   ThreadPool pool(N)          |     |     parser.feed(buf, n)                      |
 |   loop:                       |     |     while parser.next(): execute -> out      |
 |     poll(listener, wake_pipe) |     |     write_all(out)                           |
 |     accept4()                 |     |   Socket destructor closes the fd            |
 |     Connection c{Socket,...}  |     +----------------------------------------------+
 |     pool.submit(move(c))      |                       |
 |     every 1 s: purge_expired  |                       v
 |     every 10 s: log ops/s     |          Store: unordered_map + shared_mutex
 |   ~ThreadPool: stop + join    |          Metrics: atomics + latency histogram
 +-------------------------------+
   signal handler: server.request_stop()  = atomic store + write(wake_pipe)
```

### 4.1 Components

| component        | header                | responsibility |
|------------------|-----------------------|----------------|
| `FileDescriptor` | `fd.hpp`              | owns one descriptor; closes exactly once; move-only |
| `Socket`         | `socket.hpp`          | listen/connect/accept/read/write/poll with EINTR and partial-write handling; move-only |
| `Parser`         | `protocol.hpp`        | incremental request parser (state machine); request and reply encoders; limits |
| `Store`          | `store.hpp`           | `unordered_map` behind a `shared_mutex`; lazy expiry + periodic purge |
| `ThreadPool`     | `thread_pool.hpp`     | N workers, one FIFO queue, `MoveOnlyFunction` tasks, drain/stop semantics |
| `Connection`     | `connection.hpp`      | one client's session; move-only; owns the Socket; `execute_command` |
| `Server`         | `server.hpp`          | acceptor loop, wake pipe, submits Connections, periodic maintenance |
| `Client`         | `client.hpp`          | blocking client library; reply parser |
| `Metrics`        | `metrics.hpp`         | atomic counters and a lock-free latency histogram |
| `log`            | `log.hpp`             | levelled, thread-safe stderr logging |

### 4.2 Threading model

* One acceptor thread (the thread that called `Server::run`).
* N worker threads created once. A connection occupies one worker for its whole lifetime.
  N therefore bounds *concurrent* connections; connections beyond N wait in the pool queue
  (bounded by `max_pending_connections`, default 1024; beyond that: `-ERR server busy`, close).
* Shared state: the `Store` (shared_mutex), `Metrics` (atomics), the stop flag (atomic),
  the pool queue (mutex + condition variables). Nothing else is shared.
* Workers never block on each other except at the store lock. Writers hold the exclusive
  lock for one hash-map operation; readers run in parallel.

### 4.3 Ownership and lifetimes

* `Store` and `Metrics` outlive `Server`; `Server` outlives its `ThreadPool`; the pool
  outlives every `Connection` it runs. All of this is enforced by scope in `main` and `run()`.
* A `Connection` is created on the acceptor thread, moved into a lambda, moved into the
  queue, moved out by a worker. The `Socket` travels with it. Nothing is copied; nothing is
  shared; the descriptor closes when the lambda is destroyed, on whichever thread that is.
* A queued connection that is never served (shutdown) is destroyed by `ThreadPool::stop()`
  outside the pool lock, which closes its socket.

### 4.4 Shutdown sequence

1. SIGINT/SIGTERM -> handler -> `Server::request_stop()`: `stop_ = true`, one byte to the wake pipe.
2. The acceptor's `poll()` returns; the loop condition fails; it logs and leaves `run()`.
3. Leaving `run()` destroys the `ThreadPool`: `stop()` marks it stopped, drops queued
   connections (closing their sockets), wakes idle workers.
4. Each busy worker's `Connection::serve()` sees `stop_` within one poll interval (250 ms by
   default), finishes the write it was doing, shuts down its socket and returns.
5. `~ThreadPool` joins all workers. `run()` returns. `main` returns 0.

No command is half-applied: the store lock makes each operation atomic, and a worker that has
read a command executes it and writes the reply before checking the flag again.

## 5. Non-functional requirements

| requirement | how it is met | how it is checked |
|-------------|---------------|-------------------|
| `-Wall -Wextra -Wpedantic -Wshadow -Werror` clean | one `tcpkv_warnings` INTERFACE target linked by every binary | every CI job |
| no leaks, no invalid accesses | RAII for every resource; no raw `new`/`close` | ASan+LSan job; `valgrind_check.sh` under `load_gen` |
| no data races | shared_mutex, atomics, pool mutex; nothing else shared | TSan job running the stress and integration tests |
| no undefined behaviour | `memcpy` for sockaddr casts, `from_chars` for integers, bounds checks on every buffer | UBSan job |
| graceful shutdown on SIGINT/SIGTERM | self-pipe + atomic flag + bounded poll | `ServerShutdown.*` tests; manual Ctrl-C under Valgrind |
| configurable port and thread count | `--port`, `--threads`, `--host`, `--idle-timeout`, `--max-pending`, `--stats-interval`, `--log-level` | `--help`; integration tests use port 0 |
| robust to bad clients | limits + fatal/non-fatal error split; `MSG_NOSIGNAL`; EINTR retries; partial writes | `protocol_test` malformed cases; integration tests |
| observable | `STATS` command; periodic ops/s + p99 log line | `StatsReportsCounters` test |

## 6. Testing strategy

Four layers, each catching what the one below cannot.

1. **Unit tests without I/O.** `Parser`, encoders, `Store`, `ThreadPool`, `MoveOnlyFunction`,
   `execute_command`. Fast, deterministic, run under every sanitizer.
   Malformed input is a first-class test subject: every error message in section 3.4 has a test.
2. **Unit tests with real descriptors.** `FileDescriptor` (does the kernel agree it is closed?),
   `Socket` over loopback on an ephemeral port (echo, EOF, timeout, 4 MiB write that forces
   partial sends).
3. **Integration tests.** A real `Server` on port 0 in a thread, real `Client`s and raw
   sockets. Pipelining, fatal errors, 16 clients on 4 workers, shutdown latency, active
   connection accounting.
4. **System tests.** `load_gen` (C++ and Python) hammering the server while it runs under
   Valgrind in CI; TSan and ASan runs of the full test suite. `load_gen` verifies every GET,
   so it is a correctness check, not just a benchmark.

Thread-safety tests cannot prove the absence of races; TSan can prove the presence. So the
stress tests are written to maximise interleavings (many threads, small key space, no sleeps)
and the TSan preset is a required CI job.

## 7. Observability

`STATS` returns, one per line: `uptime_seconds`, `connections_accepted`, `connections_active`,
`connections_rejected`, `commands_total`, `commands_failed`, `bytes_in`, `bytes_out`,
`latency_count`, `latency_mean_us`, `latency_p50_us`, `latency_p99_us`, `latency_p999_us`.

Latency here is server-side execution time of the store operation (bucketed, log scale,
50 us to 5 s). Client-observed round trip is measured by `load_gen`.

Every 10 seconds (configurable) the acceptor logs:
`stats: 48213 commands in last 10 s (4821 ops/s), active connections 8, p99 100 us`.

Log format: `2026-11-03T10:00:00.123 INFO  [tid 4242] message` on stderr; levels debug, info,
warn, error; per-connection events at debug.

## 8. Repository layout

```
tcpkv/
  CMakeLists.txt              library + apps + tools + tests; warnings target; sanitizer option
  CMakePresets.json           debug, release, relwithdebinfo, asan, tsan, ubsan
  .github/workflows/ci.yml    matrix over presets + valgrind job + advisory clang-format
  .clang-format  .gitignore  LICENSE (MIT)  README.md
  docs/PROTOCOL.md
  include/tcpkv/              fd.hpp socket.hpp protocol.hpp store.hpp thread_pool.hpp
                              connection.hpp server.hpp client.hpp metrics.hpp log.hpp
  src/                        one .cpp per header
  apps/                       tcpkv_server.cpp  tcpkv_cli.cpp
  tools/                      load_gen.cpp  load_gen.py  valgrind_check.sh
  tests/                      fd_test.cpp protocol_test.cpp store_test.cpp thread_pool_test.cpp integration_test.cpp
```

## 9. GitHub README outline

1. Title, badges (CI, license, C++17, Linux), one-paragraph description, a 10-line demo session.
2. Why this exists (two sentences; honest).
3. Architecture diagram and five bullets on the key decisions.
4. Protocol table with a link to `docs/PROTOCOL.md`.
5. Build, test, sanitizer and Valgrind commands (copy-pasteable).
6. Load generator usage.
7. Benchmark table with the machine description and your numbers.
8. Layout.
9. What I would do next (epoll, persistence, replication, sharded locks).
10. License.

## 10. Licence

MIT. It is the licence reviewers expect on a portfolio project, it lets anyone read, copy
and learn from the code, and it has no obligations that could complicate a future employer's
view of it. The copyright line uses your name and GitHub handle; replace it with your legal
name before tagging if you prefer.
