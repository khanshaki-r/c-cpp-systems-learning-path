# tcpkv

[![CI](https://github.com/khanshaki-r/tcpkv/actions/workflows/ci.yml/badge.svg)](https://github.com/khanshaki-r/tcpkv/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-informational)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey)

A multithreaded TCP key-value server in C++17. Text protocol, in-memory store guarded by a
`std::shared_mutex`, fixed thread pool, RAII socket wrappers, GoogleTest suite, CI that runs the
tests under AddressSanitizer, ThreadSanitizer, UndefinedBehaviorSanitizer and Valgrind.

```
$ ./build/release/tcpkv_server --port 7379 --threads 8
2026-11-03T10:00:00.000 INFO  [tid 4242] tcpkv listening on 0.0.0.0:7379 with 8 worker threads

$ ./build/release/tcpkv_cli
tcpkv> set greeting hello world
OK
tcpkv> get greeting
"hello world"
tcpkv> expire greeting 60
(integer) 1
tcpkv> ttl greeting
(integer) 60
tcpkv> keys gr
1) "greeting"
tcpkv> stats
"uptime_seconds 12\nconnections_accepted 1\n..."
```

## Why this exists

I wanted a server small enough to hold in my head and complete enough to be judged like
production code: every byte of the protocol specified, every failure path tested, every build
`-Werror` clean, and nothing the sanitizers or Valgrind can complain about under concurrent load.

## Architecture

```
                 +-----------------------------------------------------------+
   clients       |                      tcpkv_server                         |
                 |                                                           |
  nc / cli ----->|  acceptor thread                    worker pool (N)       |
  load_gen ----->|  poll(listener, wake_pipe)          +-----------------+   |
  Client lib --->|    accept4()                        | Connection      |   |
                 |    Connection{Socket}  --submit-->  |  poll/read      |   |
                 |    pool.submit(move)                |  Parser.feed    |   |
                 |                                     |  execute -------+---+---> Store
                 |  every 1 s: store.purge_expired()   |  write_all      |   |     unordered_map
                 |  every 10 s: log ops/s, p99         +-----------------+   |     + shared_mutex
                 |                                                           |
   SIGINT/TERM ->|  request_stop(): atomic flag + write(wake_pipe)           |
                 +-----------------------------------------------------------+
```

* **One connection = one worker thread for its lifetime.** Simple, debuggable, and fast enough
  for a few thousand connections. The handler blocks in `poll()` with a short timeout so it
  can notice shutdown; `epoll` would be the next step (see "What I would do next").
* **`Store`** is a `std::unordered_map<std::string, Entry>` behind one `std::shared_mutex`:
  GET/EXISTS/KEYS/TTL share the lock, SET/DEL/EXPIRE take it exclusively. Expiry is lazy
  on read plus a one-second purge from the acceptor thread.
* **RAII everywhere.** `FileDescriptor` owns exactly one descriptor and closes it once;
  `Socket` adds the syscalls with EINTR and partial-write handling; `Connection` is move-only
  and owns its `Socket`. There is no `close()` call outside `FileDescriptor::reset`.
* **`ThreadPool`** uses a hand-rolled `MoveOnlyFunction` so a lambda that captured a
  `Connection` by move can be queued (`std::function` requires copyable callables).
* **Graceful shutdown** uses the self-pipe trick: the signal handler does an atomic store and
  one `write(2)` to a non-blocking pipe that the acceptor polls; workers finish their current
  command and exit within one poll interval; the pool joins them.

## Protocol

Line-based text; values are length-prefixed so they can contain spaces, newlines and NUL.

| Request                          | Reply                                        |
|----------------------------------|----------------------------------------------|
| `PING`                           | `+PONG`                                      |
| `GET key`                        | `$<len>\n<bytes>` or `$-1` if missing        |
| `SET key <len>\n<bytes>`         | `+OK`                                        |
| `DEL key [key ...]`              | `:<number removed>`                          |
| `EXISTS key [key ...]`           | `:<number present>`                          |
| `KEYS [prefix]`                  | `*<n>` followed by n bulk strings, sorted    |
| `EXPIRE key seconds`             | `:1` or `:0` if the key does not exist       |
| `TTL key`                        | `:<seconds>`, `:-1` no expiry, `:-2` missing |
| `STATS`                          | bulk string, `name value` per line           |
| `QUIT`                           | `+BYE`, then the server closes               |
| anything else                    | `-ERR <message>`                             |

Limits: keys 1..256 bytes of printable ASCII, values up to 1 MiB, lines up to 4096 bytes.
Unknown commands and arity errors are reported and the connection continues; an oversized
value or an unterminated line is fatal and the connection is closed after the error reply.
The full grammar is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Build

Requirements: Linux, GCC 13 (or Clang 16), CMake 3.25+, Ninja. GoogleTest is fetched automatically.

```bash
cmake --preset release && cmake --build --preset release
./build/release/tcpkv_server --port 7379
./build/release/tcpkv_cli -p 7379 ping
```

Tests and sanitizers:

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan  && ctest --preset tsan
cmake --preset ubsan && cmake --build --preset ubsan && ctest --preset ubsan
bash tools/valgrind_check.sh build/relwithdebinfo    # memcheck under load
```

## Load generator

```bash
./build/release/load_gen --port 7379 --clients 8 --duration 10 --value-size 64 --read-ratio 0.8
python3 tools/load_gen.py --port 7379 --clients 8 --duration 10
```

Every client owns a disjoint key range and verifies each GET against its last SET, so the load
generator is also a correctness check. Exit status is 1 on any mismatch.

## Benchmark

Measured on: `<CPU model, cores, RAM, kernel version, WSL2 or bare metal>`, server and load
generator on the same host over loopback, `release` preset. Fill in your own numbers; the
procedure is in step 10 of the build guide.

| clients | threads | value size | read ratio | ops/s | GET p50 | GET p99 | SET p99 |
|--------:|--------:|-----------:|-----------:|------:|--------:|--------:|--------:|
|       1 |       1 |       64 B |       0.80 |       |         |         |         |
|       4 |       4 |       64 B |       0.80 |       |         |         |         |
|       8 |       8 |       64 B |       0.80 |       |         |         |         |
|      16 |      16 |       64 B |       0.80 |       |         |         |         |
|      16 |      16 |      4 KiB |       0.80 |       |         |         |         |
|      16 |      16 |       64 B |       0.50 |       |         |         |         |

Always start the server with `--threads` at least equal to `--clients`. A connection holds its
worker thread until it disconnects, so extra clients wait in the queue and the row would really
measure fewer clients than it says. This is the main limit of the thread-per-connection design,
and the reason `epoll` is the first item under "What next".

Latencies are client-observed round trips from `load_gen` (bucket upper bounds, see
`LatencyHistogram`). Server-side `STATS` latencies measure only the store operation.

## Layout

```
include/tcpkv/   public headers (fd, socket, protocol, store, thread_pool, connection, server, client, metrics, log)
src/             implementations
apps/            tcpkv_server, tcpkv_cli
tools/           load_gen.cpp, load_gen.py, valgrind_check.sh
tests/           GoogleTest: fd, protocol, store, thread_pool, integration
.github/         CI: debug/release/asan/tsan/ubsan matrix + valgrind job
```

## What I would do next

* `epoll` with edge-triggered non-blocking sockets and a small number of event-loop threads,
  so idle connections stop costing a thread each.
* Persistence: append-only log with periodic snapshots, then a real storage engine
  (see my `minidb` project for the B+tree and WAL version of that idea).
* Replication: leader/follower with the same log shipped to followers.
* Sharded locks (N `shared_mutex`es keyed by hash) if profiling shows writer contention.

## License

MIT. See [LICENSE](LICENSE).
