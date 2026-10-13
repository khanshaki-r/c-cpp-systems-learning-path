# 08. Networking and Distributed Systems

**Sockets, TCP/IP, RPC, HTTP and distributed systems for a server engineer**

## What this course covers

You maintain a multithreaded application server that speaks RPC over TCP on Windows. You migrated a
bank's data into YugabyteDB. You have debugged production connection problems. But you have not yet
written a socket server from a blank file, you have never used `epoll`, and you know distributed
databases as a user, not as someone who can explain why they need Raft.

This course closes those gaps. By the end you will have:

- written a blocking echo server and client with a correct read loop and an RAII socket wrapper;
- designed a text protocol (the `tcpkv` protocol) and written an incremental parser for it that
  survives bytes arriving one at a time;
- built a thread-pool TCP server with timeouts and graceful shutdown;
- built an `epoll` event-loop server and understood when it beats a thread pool;
- implemented a tiny binary RPC layer and mapped your employer's RPC transport onto standard vocabulary;
- written an HTTP/1.1 server by hand and tested it with `curl`;
- learned the reliability toolbox (timeouts, retries with jitter, circuit breakers, rate limiting);
- learned distributed-systems fundamentals: consistency models, replication, quorums, clocks,
  Raft, sharding, 2PC, and how YugabyteDB puts them together;
- learned how to run a system-design interview from requirements to deep dives, with five worked
  examples that fit your profile.

Everything targets **Linux (WSL Ubuntu 24.04), POSIX sockets, C++17, GCC 13**. Where Winsock
differs, the module says so, because your day job is on Windows.

## Why this is on the resume

Resume lines this course backs up (quoted exactly):

- "Socket programming, TCP/IP, RPC, HTTP; client-server architecture, distributed SQL, sharding,
  replication, high availability"
- "RPC-over-TCP client-server transport"
- tcpkv: "Concurrent TCP server ... text protocol"
- "zero-downtime cutover to multi-node production"

An interviewer who reads those lines will ask: "What is TIME_WAIT and why do I have 30,000 of them?",
"How do you know a message is complete when `recv` returns?", "Why does YugabyteDB need Raft when
PostgreSQL does not?", "Design a key-value store that survives a node dying." This course is the
material for those answers.

## Prerequisites

- Comfortable C. Beginner C++ is enough to start; modules 03 to 08 use RAII, `std::thread`,
  `std::mutex`, `std::vector`, `std::string_view`, lambdas. If those are unfamiliar, read
  `04_Concurrency/` modules 01 to 03 in parallel.
- WSL Ubuntu 24.04 with `build-essential`, plus: `sudo apt install net-tools iproute2 dnsutils tcpdump
  curl python3`.
- Knowing what a thread and a mutex are. Knowing what a SQL transaction is.

## Modules

| # | Module | Hours |
|---|--------|-------|
| 01 | [How the internet moves bytes](01_how_the_internet_moves_bytes.md) — layers, IP, ports, NAT, DNS, what happens on connect, `ping`/`ip`/`ss`/`dig`/`tcpdump` | 2.5 |
| 02 | [TCP in depth](02_tcp_in_depth.md) — handshake, sequence numbers, retransmission, windows, congestion control, Nagle, keepalive, the state machine, TIME_WAIT and CLOSE_WAIT, "TCP is a byte stream" | 3 |
| 03 | [Sockets API and a blocking server](03_sockets_api_blocking_server.md) — socket/bind/listen/accept/connect, `getaddrinfo`, errno, partial reads, RAII wrapper, echo server and client, Winsock differences | 3 |
| 04 | [Framing and protocol design](04_framing_and_protocol_design.md) — length-prefix vs delimiter, endianness, the tcpkv protocol, incremental parser, fuzzing, Redis RESP | 3 |
| 05 | [Thread-per-connection and thread pools](05_thread_per_connection_and_thread_pools.md) — C10K, thread pool, connection lifecycle, timeouts, graceful shutdown, tcpkv v1 | 3 |
| 06 | [Non-blocking I/O, epoll and event loops](06_nonblocking_io_epoll_event_loops.md) — `O_NONBLOCK`, select/poll/epoll, reactor, per-connection state, eventfd/timerfd, io_uring, IOCP, Asio | 3.5 |
| 07 | [RPC](07_rpc.md) — stubs, serialization, request ids, timeouts/retries/idempotency, gRPC, a tiny binary RPC layer | 3 |
| 08 | [HTTP and REST](08_http_and_rest.md) — HTTP/1.1 format, keep-alive, chunked, HTTP/2 and /3, TLS, REST design, a minimal HTTP server, `curl` | 3 |
| 09 | [Client-server architecture and reliability](09_client_server_architecture_and_reliability.md) — stateless vs stateful, pooling, load balancers, retries with jitter, circuit breakers, token bucket, observability | 2.5 |
| 10 | [Distributed systems fundamentals](10_distributed_systems_fundamentals.md) — partial failure, CAP/PACELC, consistency models, replication, quorums, exactly-once myth, clocks and HLC | 3 |
| 11 | [Consensus: Raft in plain words](11_consensus_raft_in_plain_words.md) — leader election, log replication, commit index, safety, membership change, YugabyteDB per-tablet Raft, PostgreSQL streaming replication, a Raft election simulation | 3 |
| 12 | [Sharding, partitioning and distributed SQL](12_sharding_partitioning_and_distributed_sql.md) — hash vs range, hotspots, tablet splitting, consistent hashing, 2PC, YugabyteDB transactions, schema design with shard keys | 3 |
| 13 | [High availability and operations](13_high_availability_and_operations.md) — SLO, RTO/RPO, active-passive/active-active, rolling and blue-green deploys, DB failover, RF3, backups, chaos, runbooks, your cutover story | 2.5 |
| 14 | [System design interview walkthroughs](14_system_design_interview_walkthroughs.md) — the method, then KV store, loan-origination backend, rate limiter, URL shortener, notification queue | 4 |

Total: about 42 hours. At 6 to 8 hours per week this is a 6-week course studied alongside others.

Suggested order: 01 to 06 in sequence (each builds on the previous code). 07 and 08 can be swapped.
09 to 13 in sequence. 14 last, after everything else, because it uses all the vocabulary.

## Code

`code/` holds complete programs. Build everything on WSL with:

```bash
cd code
make            # builds every C++ program into ./bin
make test       # runs the parser tests and the consistent-hash and Raft simulations
```

Or compile one file by hand:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m03_echo_server.cpp -o echo_server
```

| File | Used by | What it is |
|------|---------|------------|
| `m02_tcp_options.cpp` | 02 | Reads and sets `TCP_NODELAY`, keepalive, buffer sizes on a socket and prints them |
| `m03_socket.hpp` | 03 to 08 | RAII `Fd` wrapper, `read_exact`/`write_all`, `getaddrinfo` helpers, listening-socket factory |
| `m03_echo_server.cpp` | 03 | Blocking, one-client-at-a-time echo server |
| `m03_echo_client.cpp` | 03 | Echo client that sends lines and verifies the echo |
| `m04_tcpkv_parser.hpp` | 04, 05 | Incremental state-machine parser for the tcpkv text protocol |
| `m04_tcpkv_parser_test.cpp` | 04 | Assert-based tests including a random-split fuzz loop |
| `m04_framing.cpp` | 04 | Length-prefixed binary framing with `htonl`/`ntohl`, encode and decode round trip |
| `m05_threadpool_tcpkv_server.cpp` | 05 | tcpkv v1: acceptor + thread pool + blocking I/O + timeouts + graceful shutdown |
| `m05_loadtest.py` | 05, 06 | Python load-test client that opens N connections and hammers the server |
| `m06_epoll_echo_server.cpp` | 06 | Edge-triggered epoll echo server with per-connection buffers and eventfd shutdown |
| `m07_rpc.hpp` | 07 | Tiny binary RPC: frame header, request ids, dispatch table, error propagation |
| `m07_rpc_demo.cpp` | 07 | RPC server and client in one process over a real TCP socket |
| `m08_http_server.cpp` | 08 | Minimal HTTP/1.1 server: request line, headers, keep-alive, JSON response |
| `m09_token_bucket.cpp` | 09 | Thread-safe token-bucket rate limiter with a simulation |
| `m10_hlc.cpp` | 10 | Hybrid logical clock simulation across three nodes with skew |
| `m11_raft_election_sim.cpp` | 11 | Single-process Raft leader-election simulation with message delays and partitions |
| `m12_consistent_hash.cpp` | 12 | Consistent hashing ring with virtual nodes and a key-movement measurement |
| `Makefile` | all | Builds everything with the required flags |

## You are done when you can...

- [ ] Draw the TCP state machine from memory and explain who holds TIME_WAIT and why, and what a
      pile of CLOSE_WAIT sockets tells you about your own code.
- [ ] Explain why `recv` returning 1 byte is correct behaviour, and write the read loop that handles it.
- [ ] Write a blocking echo server and client in C++17 without looking anything up.
- [ ] Design a framed protocol and justify length-prefix vs delimiter for a given use case.
- [ ] Write an incremental parser that is correct when input arrives one byte at a time.
- [ ] Explain what `EPIPE`, `ECONNRESET`, `EAGAIN` and `EINTR` mean and what to do for each.
- [ ] Build a thread-pool server with graceful shutdown and explain its limit (C10K).
- [ ] Build an edge-triggered epoll server and explain why you must drain until `EAGAIN`.
- [ ] Explain reactor vs proactor and place epoll, IOCP, io_uring, libuv and Asio in that picture.
- [ ] Describe your employer's RPC-over-TCP transport using the words framing, request id, dispatch,
      timeout, idempotency, backpressure, versioning.
- [ ] Hand-parse an HTTP/1.1 request and explain keep-alive and chunked encoding.
- [ ] Explain retries with exponential backoff and jitter, circuit breakers, and token buckets, and
      say which problem each solves.
- [ ] Define linearizable, sequential, causal and eventual consistency with one example each.
- [ ] Explain Raft leader election and log replication, and why a majority quorum prevents split brain.
- [ ] Explain why YugabyteDB needs Raft per tablet and why PostgreSQL streaming replication is not consensus.
- [ ] Explain hash vs range sharding, hotspots, tablet splitting and consistent hashing.
- [ ] Explain 2PC and how a distributed SQL database makes a cross-shard transaction atomic.
- [ ] Define SLO, RTO, RPO, and describe a zero-downtime rolling deploy and a database failover.
- [ ] Lead a 45-minute system design discussion using the method in module 14.

## Resume line this course backs up

> "Socket programming, TCP/IP, RPC, HTTP; client-server architecture, distributed SQL, sharding,
> replication, high availability"

> "RPC-over-TCP client-server transport"

> "Concurrent TCP server ... text protocol" (tcpkv)

> "zero-downtime cutover to multi-node production"

Module 03 and 05 produce the tcpkv server. Module 07 gives the RPC vocabulary. Modules 10 to 13
give the distributed SQL, sharding, replication and HA vocabulary. Module 13 section 5 gives you the
exact words for the cutover story.
