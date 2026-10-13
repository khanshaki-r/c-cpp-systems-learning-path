# 08. Networking and Distributed Systems: all exercises

Every exercise from every module, in one place, with its difficulty and a hint. The full "done
when" criteria are in each module's section 6. Work them in module order; within a module, easy
first. Suggested minimum before interviews: every easy and medium, plus the hard ones marked with
a star (*), which produce something you can show or talk about.

Build everything on WSL Ubuntu 24.04 with `g++ -std=c++17 -Wall -Wextra -Werror -pthread`, or
`make` in `code/`.

## Module 01: How the internet moves bytes

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | `ip route get` for loopback, the WSL gateway and `1.1.1.1`; explain each decision | Longest-prefix match on the routing table; loopback never leaves the host. |
| 2 | easy | `dig` A records, TTLs and a CNAME chain (`www.github.com`) | `dig +noall +answer`; follow the CNAME until an A record. |
| 3 | medium | Capture a handshake and a request with `tcpdump -w`, read it in Wireshark | Filter `tcp.port == N`; MSS is a TCP option in the SYN and SYN-ACK. |
| 4 | medium | Bandwidth-delay product for 100 Mbit/s at 50 ms RTT vs a 64 KB window | Bytes in flight = (bits/s / 8) x RTT; throughput cap = window / RTT. |
| 5 | hard | Shell script diagnosing DNS, ping and TCP-port failures for a host:port | Test layers bottom-up and stop at the first failure; `getent`, `ping -c1 -W1`, `nc -zv -w2`. |

## Module 02: TCP in depth

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Find who sent FIN first and confirm that side holds TIME_WAIT | The active closer holds TIME_WAIT; `ss -tan state time-wait`. |
| 2 | easy | Draw the TCP state machine from memory | Separate the three-way open, active close and passive close paths. |
| 3 | medium | Reproduce the Nagle plus delayed-ACK stall and fix with `TCP_NODELAY` | Write-write-read pattern with small writes; loopback may hide it. |
| 4 | medium | Reproduce CLOSE_WAIT, then fix the server to close on `read() == 0` | CLOSE_WAIT means your code has not called `close`. |
| 5* | hard | Estimate and measure the max connection rate under TIME_WAIT, then try `tcp_tw_reuse` | Ports in `ip_local_port_range` divided by the TIME_WAIT duration. |
| 6 | hard | `tc netem` loss and delay on loopback; find fast retransmit and RTO retransmits | Fast retransmit follows three duplicate ACKs; RTO shows as a long gap. |

## Module 03: Sockets API and a blocking server

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Run echo server and client, watch `ss -tanp` during and after | Note LISTEN, ESTAB, and who ends in TIME_WAIT. |
| 2 | easy | Remove SIGPIPE protection and kill the client mid-echo | Exit status 141 = 128 + SIGPIPE (13). |
| 3 | medium | Print each `recv` size for `seq 1 100000` | Sizes depend on segment arrival and buffer state, not on the sender's `send` calls. |
| 4 | medium | `connect_with_timeout` with non-blocking connect, `poll` and `SO_ERROR` | `EINPROGRESS` is success-so-far; read `SO_ERROR` when writable. |
| 5 | hard | Winsock version `m03_socket_win.hpp` | `WSAStartup` once, `SOCKET` is unsigned, `closesocket`, `WSAGetLastError`. |
| 6 | hard | `LD_PRELOAD` library that histograms `recv`/`read` sizes | `dlsym(RTLD_NEXT, "recv")`; print in a destructor function. |

## Module 04: Framing and protocol design

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Run parser tests and framing under ASan and UBSan | Add `-fsanitize=address,undefined -g`. |
| 2 | easy | Add `KEYS <prefix>` to the grammar and its reply formatter | Reply is an array header `*n` followed by n bulk strings. |
| 3 | medium | Incremental `ReplyParser` for `+ - : $` replies, tested with random splits | Reuse the request parser's state-machine shape; compare byte-by-byte vs whole. |
| 4 | medium | Tolerate multiple spaces and bare `\n` without breaking tests | Decide what a bare `\n` inside a bulk value means before coding. |
| 5 | hard | libFuzzer target that splits input at data-derived positions | Use the first byte(s) of input as split offsets; build with clang. |
| 6* | hard | One-page spec for a binary tcpkv | Specify every field's size, byte order, and the error/status codes. |

## Module 05: Thread-per-connection and thread pools

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Two workers, three `nc` clients: see the third starve | It sits in the accept queue or the task queue, depending on the design. |
| 2 | easy | Observe the idle timeout firing | Lower the timeout constant; check the counter at shutdown. |
| 3 | medium | Per-connection command limit or token bucket with `ERR_BUSY` | Reuse `TokenBucket` from `m09_token_bucket.cpp`. |
| 4 | medium | Reproduce the slow-reader deadlock and the send timeout freeing it | Watch Send-Q and Recv-Q both fill in `ss -tanp`. |
| 5* | hard | Request-per-worker with an I/O thread per connection and ordered replies | Tag each request with a per-connection sequence number; reorder on output. |
| 6 | hard | `SIGHUP` config reload and `SIGUSR1` counters dump | Use `signalfd` or a self-pipe; never do real work in a signal handler. |

## Module 06: Non-blocking I/O, epoll and event loops

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Compare RSS and threads of epoll vs thread-pool server at 1000 connections | `ps -o rss,nlwp -p PID`. |
| 2 | easy | Break the edge-triggered drain rule and observe hangs | ET reports a transition once; unread data never re-triggers. |
| 3* | medium | Convert the epoll echo server into tcpkv | One `Parser` and output buffer per `Conn`; replies appended to the buffer. |
| 4 | medium | Non-blocking connect inside the loop with a 3 s timer | Register `EPOLLOUT`, then `SO_ERROR`; use `timerfd` for the timeout. |
| 5 | hard | Worker pool for `SLOW` commands posting replies back via `eventfd` | Mutex-protected completion queue plus `eventfd` write to wake the loop. |
| 6 | hard | Rewrite the echo server with Boost.Asio | One `io_context`, `async_read_some` then `async_write` in a loop. |

## Module 07: RPC

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Add a `Ping` method returning monotonic milliseconds | New method id in the dispatch table; `steady_clock`. |
| 2 | easy | Bump `kVersion` on the server only and get a clear error | The side that reads the header detects it; reply with a status, not garbage. |
| 3 | medium | Pipelined client: `send_async` and `wait(id)` | Map of id to pending result; read replies until yours arrives. |
| 4 | medium | Server-side in-flight limit per connection returning `Overloaded` | Count outstanding ids per connection; decrement on reply. |
| 5* | hard | Multi-threaded client with a demux reader thread and promises | Writer mutex, one reader thread, timer thread failing promises on deadline. |
| 6 | hard | Same service in gRPC; compare p50 latency | Measure many calls after warm-up; explain HTTP/2 and protobuf costs. |

## Module 08: HTTP and REST

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Confirm keep-alive reuses one connection with `tcpdump` | One SYN for two requests with `curl url1 url2`. |
| 2 | easy | `HEAD` support, `ETag` and `304 Not Modified` | ETag = hash of the value; compare with `If-None-Match`. |
| 3 | medium | Chunked request body decoding | State machine: hex size line, data, CRLF, until size 0, then trailers. |
| 4 | medium | `If-Match` on PUT with `412`, plus a two-client race test | Compare-and-set on the ETag under the store's lock. |
| 5* | hard | HTTP server on the epoll loop, benchmark with `wrk` | Per-connection parse state survives partial requests across reads. |
| 6 | hard | TLS with OpenSSL | `SSL_CTX_new(TLS_server_method())`, self-signed cert, `SSL_accept`. |

## Module 09: Client-server architecture and reliability

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Per-connection token bucket in tcpkv | Burst 100, rate 50/s; `-ERR_BUSY` on empty. |
| 2 | easy | `backoff_ms` schedule for ten seeds | Full jitter: uniform in `[0, min(cap, base * 2^attempt)]`. |
| 3* | medium | Tested `CircuitBreaker` with an injectable clock | Test each transition: closed to open, open to half-open, half-open to both. |
| 4 | medium | `X-Request-Id` handling and JSON access log in the HTTP server | Generate an id if absent; log after the response is written. |
| 5 | hard | Connection pool for the RPC client | Bounded size, acquire timeout, validate-on-borrow after idle, max lifetime. |
| 6* | hard | Reliability review of your employer's request path | One row per hop: timeout, retry, idempotency, pool, breaker, failure UX. |

## Module 10: Distributed systems fundamentals

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Explain one `[RAW CLOCK WRONG]` line from `m10_hlc` | Identify which of the four receive branches fired from the printed `l` values. |
| 2 | easy | Read your clock offset and frequency error from chrony | ppm x 1e-6 = seconds of drift per second. |
| 3 | medium | Add a Lamport clock beside the HLC; find concurrent events it orders | Concurrent = neither happened-before the other; Lamport still gives them an order. |
| 4 | medium | Consistency model needed by each banking screen | Ask: what goes wrong if this read is one second stale? |
| 5 | hard | Leaderless N=3 store with W, R, read repair and random failures | Version every value; reader returns the highest version and repairs the others. |
| 6* | hard | Unknown-outcome decision table for your posting RPC | Four failure points times what client sees, server state, retry effect. |

## Module 11: Consensus: Raft in plain words

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Run `m11_raft_election_sim` with three seeds; compare first election and terms | Terms grow with every election attempt, including split votes. |
| 2 | easy | Four nodes partitioned 2-2 | Majority of 4 is 3; neither side has it. |
| 3 | medium | Remove timeout randomisation; observe split votes | Set `kElectionMin == kElectionMax`; count terms before a stable leader. |
| 4 | medium | Add check-quorum to the leader | Track the last successful reply time per follower; step down without a recent majority. |
| 5 | hard | Add pre-vote | A `PreVote` round with `term + 1` that does not change anyone's persistent state. |
| 6* | hard | Single-server membership change with learners | Configuration takes effect on append; promote a learner when caught up. |

## Module 12: Sharding, partitioning and distributed SQL

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Measure ring balance for 1, 5, 20, 500 virtual nodes | Relative imbalance shrinks roughly as 1/sqrt(vnodes). |
| 2 | easy | Choose shard keys for five tables | Name the hot access path and the key's cardinality for each. |
| 3 | medium | Add jump consistent hash and compare | Jump hash needs no memory but only adds/removes buckets at the end. |
| 4* | medium | YugabyteDB: compare `EXPLAIN (ANALYZE, DIST)` for two primary-key designs | `(account_no) HASH, txn_ts DESC` vs `txn_id HASH` for "last 20 of one account". |
| 5 | hard | 2PC simulation with coordinator crash, then a replicated decision record | Participants that voted yes must wait; a majority-replicated decision unblocks them. |
| 6* | hard | Sharding and colocation plan for a 12-table loan-origination schema | Every hot query should touch one tablet per table, or say why not. |

## Module 13: High availability and operations

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Error budgets for 99.5/99.9/99.95% and a series availability | 30 days = 43,200 minutes; series availabilities multiply. |
| 2 | easy | Your employer's RTO and RPO and their mechanisms | If you do not know, the mechanism tells you: backup interval, replication mode. |
| 3* | medium | Kill a yugabyted node under load; measure the blip | Expect a few seconds on tablets led by the dead node; zero lost commits. |
| 4 | medium | Expand/contract plan for renaming a column under rolling deploys | Never drop the old column in the release that stops reading it. |
| 5* | hard | Runbook for "YugabyteDB node down" | Start from the alert; exact commands; how to verify re-replication finished. |
| 6 | hard | Chaos experiment on three tcpkv instances behind HAProxy | `kill -9` one, `tc netem` another; record errors and p99 per fault. |

## Module 14: System design interview walkthroughs

| # | Level | Exercise | Hint |
|---|-------|----------|------|
| 1 | easy | Redo the estimates for all five walkthroughs on paper | 1 M/day is about 12/s; multiply by a peak factor. |
| 2 | easy | `base62` encode and decode with round-trip asserts | Divide by 62 repeatedly; decode multiplies and adds. |
| 3* | medium | Present the loan-origination design aloud in 45 minutes | Follow the method table; keep a timer visible. |
| 4 | medium | Hybrid token-leasing rate limiter in one process | Central `TokenBucket::try_acquire(20)` per lease; local counters spend it. |
| 5 | hard | Design an exactly-once-per-business-date batch scheduler | Exactly-once effect = lease + idempotent job keyed by (job, business date). |
| 6* | hard | Turn the Raft simulation into a replicated KV store with client dedupe | State machine keeps `last request id per client`; skip duplicates on apply. |
