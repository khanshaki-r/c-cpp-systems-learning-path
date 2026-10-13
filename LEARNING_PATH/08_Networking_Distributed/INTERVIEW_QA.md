# 08. Networking and Distributed Systems: interview questions

38 questions with model answers, ordered from basic to senior. Answer out loud in 30 to 90 seconds
each. The module in brackets has the depth behind the answer.

## Part A: TCP/IP and sockets

**1. What happens in the TCP three-way handshake, and why three messages? [02]**
The client sends SYN with its initial sequence number; the server replies SYN-ACK with its own ISN
and acknowledges the client's; the client ACKs the server's ISN. Three messages are the minimum for
both sides to choose a sequence number and confirm the other received it. The server's half-open
connections wait in the SYN queue; completed ones wait in the accept queue until `accept()`.

**2. What does "TCP is a byte stream" mean for application code? [02, 04]**
TCP preserves the order and content of bytes, not the boundaries of your `send` calls. One `send`
can arrive as several `recv`s and several `send`s can arrive in one `recv`. The application must
frame messages itself (length prefix or delimiter) and keep a buffer that accumulates bytes until
a complete message is present.

**3. Length-prefix or delimiter framing: how do you choose? [04]**
Length prefix makes the receiver know exactly how many bytes to wait for, handles binary payloads,
and never scans the body; it needs a maximum length check to stop a hostile 4 GB prefix. Delimiters
(like `\r\n`) are human-readable and easy to debug with `nc` but need escaping or a length for
binary data and require scanning. Text protocols often mix both, as Redis and tcpkv do (`$len`).

**4. `recv` returned fewer bytes than you asked for. Bug? [03]**
No, it is normal: `recv` returns whatever is in the socket buffer, at least one byte. Loop until
you have the bytes the frame needs (`read_exact`), treat 0 as orderly close by the peer, retry
on `EINTR`, and on non-blocking sockets stop on `EAGAIN` and wait for readiness. The same applies
to `send`: it may write only part of the buffer, so loop (`write_all`).

**5. What are TIME_WAIT and CLOSE_WAIT, and what does a pile of each mean? [02]**
TIME_WAIT is held by the side that closed first, for twice the maximum segment lifetime (60 s on
Linux), so late duplicates of the old connection cannot corrupt a new one with the same port pair
and the final ACK can be resent. Thousands of them are normal on a busy client and can exhaust
ephemeral ports. CLOSE_WAIT means the peer closed and *your* program has not called `close`: a pile
of them is a file descriptor leak in your code.

**6. Why does a server set `SO_REUSEADDR`? [03]**
So it can bind its listening port again immediately after a restart while old connections on that
port are still in TIME_WAIT. Without it, `bind` fails with `EADDRINUSE` for up to a minute.
`SO_REUSEPORT` is different: it lets several sockets listen on the same port and the kernel
spreads incoming connections among them.

**7. What are Nagle's algorithm and delayed ACKs, and when do you set `TCP_NODELAY`? [02]**
Nagle holds small writes while unacknowledged data is in flight, to coalesce them; delayed ACK
holds ACKs briefly hoping to piggyback them. Together, a write-write-read pattern can stall for
tens of milliseconds. Request/response protocols with small messages set `TCP_NODELAY` and, better,
write each message with a single `send`.

**8. How do you detect a dead peer on an idle TCP connection? [02, 09]**
TCP sends nothing on an idle connection, so a crashed peer or a dropped NAT mapping goes unnoticed
until you write. Use TCP keepalive with short, tuned intervals (`TCP_KEEPIDLE`, `TCP_KEEPINTVL`,
`TCP_KEEPCNT`) or, better, application-level heartbeats with a read timeout, which also detect a
hung peer process.

**9. Why ignore SIGPIPE in a server? [03]**
Writing to a socket whose peer has reset the connection raises SIGPIPE, whose default action kills
the process. Ignore it (or use `MSG_NOSIGNAL`) and handle the `EPIPE` error from `send` by closing
that connection.

## Part B: Server concurrency models

**10. Thread-per-connection versus a thread pool versus an event loop? [05, 06]**
Thread-per-connection is simple but each thread costs memory and scheduling, so it falls over in
the thousands of connections. A pool bounds threads but a blocking read on an idle connection still
pins a worker. An event loop (epoll) handles many connections per thread by doing only
non-blocking I/O when the kernel says a socket is ready; it needs per-connection state machines and
must never block, so slow work goes to a separate pool.

**11. epoll level-triggered versus edge-triggered? [06]**
Level-triggered reports a socket as long as it is readable, so reading part of the data is fine;
the next `epoll_wait` reports it again. Edge-triggered reports only when the state changes (new
data arrived), so you must read until `EAGAIN` or you will never be told about the remaining bytes.
ET saves wakeups under load and requires non-blocking sockets and careful drain loops.

**12. Reactor versus proactor? [06]**
A reactor tells you when an operation can be done without blocking, and you then do the read or
write yourself: epoll, kqueue, poll, libuv on Unix, Asio on Linux. A proactor performs the
operation for you and tells you when it has completed: Windows IOCP and Linux io_uring. Asio
presents a proactor-style API on top of either.

**13. Why must an event-loop thread never block, and how do you run slow work? [06]**
Every connection on that loop waits while one handler blocks, so latency for all of them jumps.
Hand slow work (database calls, CPU-heavy parsing) to a worker pool, and have the worker post the
result back to the loop through a queue plus an `eventfd` wake-up so the reply is written on the
loop thread.

**14. What is the C10K problem and what solved it? [05, 06]**
Handling ten thousand concurrent connections on one server, which thread-per-connection and
`select` (O(n) scanning, fd limit 1024) could not do efficiently. Readiness APIs with O(ready)
cost (epoll, kqueue, IOCP), non-blocking I/O and event loops solved it; today the question is
millions of connections, addressed with the same design plus kernel tuning.

## Part C: RPC and HTTP

**15. What does an RPC framework do for you? [07]**
It turns a typed function call into bytes and back: the stub serialises arguments, the transport
frames and sends them with a request id, the server dispatches by method id, runs the handler and
returns a status and result with the same id. It also handles deadlines, errors, versioning and
often connection management. gRPC does this over HTTP/2 with protobuf.

**16. Why do RPC messages carry a request id? [07]**
To match replies to requests when several calls are outstanding on one connection or replies come
back out of order. After a timeout, you either reserve that id forever or close the connection, so
a late reply cannot be matched to a newer request.

**17. A call timed out. Should the client retry? [07, 09]**
Only if the operation is idempotent or carries an idempotency key, because a timeout is an unknown
outcome: the server may have done the work. Retry only transient errors, with capped attempts,
exponential backoff with full jitter, a retry budget, and at one layer of the stack only.

**18. How do you make a non-idempotent operation safe to retry? [07, 10]**
The client attaches a unique idempotency key per logical operation (for example branch code plus
branch transaction number). The server stores the key with a unique constraint in the same
transaction as the effect, and a repeat returns the stored result instead of applying it again.

**19. What is HTTP keep-alive and why does it matter? [08]**
HTTP/1.1 connections are persistent by default: many requests reuse one TCP connection, avoiding a
handshake, TLS negotiation and slow start per request. The server must know where each request
body ends (Content-Length or chunked encoding) and should have an idle timeout. HTTP/1.1 still
serves one request at a time per connection; HTTP/2 multiplexes many streams on one connection.

**20. When would you choose HTTP/REST and when a custom binary RPC over TCP? [07, 08]**
HTTP/REST for external and cross-team APIs: universal tooling, proxies, caches, browsers, easy
debugging. A binary RPC (or gRPC) for internal, high-rate, low-latency calls with typed schemas.
A hand-rolled binary protocol is justified only when you control both ends and need the last bit of
performance or have a legacy client base, and then it needs versioning and framing discipline.

## Part D: Reliability

**21. Explain exponential backoff with jitter. [09]**
After each failure wait longer (base times 2 to the attempt, capped), and randomise the wait
(full jitter: uniform between 0 and that value) so that many clients that failed together do not
retry together and recreate the overload. Combine with a retry budget to cap the extra load.

**22. What is a circuit breaker? [09]**
A per-dependency state machine: closed passes calls and counts failures; when failures cross a
threshold it opens and fails calls immediately; after a cool-down it goes half-open and lets a
trial call through, closing on success. It saves threads from waiting on a dead dependency and stops
failures cascading.

**23. How does a token bucket rate limiter work? [09]**
A bucket of capacity C refills at R tokens per second; each request takes a token or is rejected
when empty. C is the burst allowance and R the sustained rate. Implement lazily: on each call add
elapsed time times R, capped at C, then try to take a token.

## Part E: Distributed systems

**24. State the CAP theorem correctly. [10]**
During a network partition a replicated system must choose between linearizable consistency and
availability of every non-failed node. Partitions happen regardless, so the choice is how to behave
during one: refuse requests on the minority side (CP, like YugabyteDB or etcd) or answer with
possibly stale or conflicting data (AP, like Cassandra).

**25. What does PACELC add to CAP? [10]**
Even without a partition there is a trade-off between latency and consistency, because strongly
consistent writes wait for a quorum. YugabyteDB and Spanner are PC/EC; Cassandra is PA/EL.

**26. Explain linearizable, sequential, causal and eventual consistency with an example. [10]**
Linearizable: a deposit confirmed at 10:00:00 is visible to any reader anywhere at 10:00:01.
Sequential: everyone sees the same order consistent with each client's own order, but it can lag
real time. Causal: if "rent paid" was written after seeing "salary credited", nobody sees the
first without the second; unrelated writes may appear in different orders. Eventual: replicas
converge once writes stop, like DNS after a change.

**27. Why can't you order events across machines by wall-clock time? What do you use? [10]**
Clocks drift and are corrected imperfectly; NTP is typically millisecond-accurate on a LAN and can
step backwards, so a later event can carry an earlier timestamp. Use Lamport or vector clocks for
causality, or hybrid logical clocks, which respect causality and stay close to physical time;
YugabyteDB and CockroachDB timestamp transactions with HLCs.

**28. What is split brain and how do you prevent it? [10]**
Two nodes both believe they are the leader and accept writes, so data diverges. Prevent it with
majority quorums (two majorities cannot coexist), fencing tokens such as Raft terms that let
downstream reject a stale leader, STONITH, and leases. Never build a two-node cluster without a
third vote.

**29. Is exactly-once delivery possible? [10]**
Not over an unreliable network with failures, because the sender can never know whether its last
attempt was processed. Systems give exactly-once effect: at-least-once delivery plus idempotent
processing or deduplication committed atomically with the side effect.

## Part F: Consensus, sharding, distributed SQL

**30. Explain Raft leader election. [11]**
Followers wait for heartbeats; one whose randomised election timeout expires increments the term,
votes for itself and requests votes. Each node votes once per term, persisted, and only for a
candidate whose log is at least as up to date as its own. A majority makes a leader, which
immediately sends heartbeats; split votes are retried with new random timeouts.

**31. How does Raft replicate and commit a log entry? [11]**
The leader appends the entry and sends AppendEntries with the previous index and term; a follower
accepts only if its log matches at that point, otherwise the leader backs up and retries, and the
follower truncates conflicts. Once a current-term entry is stored on a majority, the leader
advances the commit index, applies it and replies; followers apply up to the commit index they
learn from later messages.

**32. Why does YugabyteDB use Raft per tablet when PostgreSQL uses streaming replication? [11]**
PostgreSQL has one writable primary and ships WAL to standbys, leaving leader election and fencing
to external tools like Patroni with etcd. YugabyteDB shards tables into tablets led by different
nodes, so each tablet needs its own automatic, safe leader election and majority-acknowledged
replication; that is a Raft group per tablet, and its Raft log is the tablet's WAL.

**33. Hash versus range sharding, and how do you pick a shard key? [12]**
Hash spreads load evenly and avoids hot tails for sequential keys but makes range scans scatter;
range keeps order for cheap scans but monotonic keys hit one shard. Pick the key from the hot access
path and cardinality: for banking, hash on account number with transaction time as a range
component inside the account, hash sequence-generated numbers, never shard only on a skewed code
like branch, and colocate small reference tables.

**34. How does consistent hashing work and why use virtual nodes? [12]**
Nodes and keys hash onto a ring, and each key belongs to the next node clockwise, so adding or
removing a node moves only about 1/N of the keys instead of nearly all with `hash % N`. Virtual
nodes give each server many ring positions, which evens out load and spreads a failed node's keys
across many survivors.

**35. Explain two-phase commit and how distributed SQL avoids its blocking problem. [12]**
The coordinator asks every participant to prepare (persist the writes, hold locks, vote); if all
vote yes it records commit and tells them, otherwise abort. If the coordinator dies after the votes,
participants are stuck holding locks. YugabyteDB records the decision as one Raft-replicated write
to a transaction status tablet, with the writes held as replicated intents, so the decision
survives any single node failure and readers can resolve intents by looking it up.

## Part G: High availability and design

**36. Define SLO, RTO and RPO, and how replication choices affect RPO. [13]**
An SLO is the internal target for a user-facing indicator, like 99.9% of postings succeeding within
500 ms over 30 days (43.2 minutes of error budget). RTO is the allowed downtime after a failure;
RPO is the allowed data loss, measured in time. Quorum or synchronous replication gives RPO 0;
async replication gives RPO equal to the lag; backups alone give RPO equal to the backup interval.

**37. What happens when one node of a three-node, RF3 YugabyteDB cluster dies? [13]**
Tablets it led elect new leaders among their two surviving peers once the leader lease expires,
so their writes pause for a few seconds; other tablets just lose a follower. Nothing committed is
lost, because every commit was on a majority. The cluster is under-replicated until the node
returns or replicas are re-created elsewhere, which needs a spare node.

**38. How would you describe a zero-downtime cutover to a new database? [13]**
Bulk-load while the old system serves, catch up deltas through a change log or timestamps, verify
with a field-level parity check as a go/no-go gate, quiesce briefly for the final delta, flip the
application's configuration with rolling restarts, keep a rehearsed rollback path with a defined
point of no return, and monitor closely afterwards. Use your real numbers for deployments, window
length and mismatches found.
