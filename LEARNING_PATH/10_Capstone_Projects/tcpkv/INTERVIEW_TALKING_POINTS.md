# tcpkv: interview talking points

**Use:** read this after step 10, then practise out loud until you can give the 2-minute and
15-minute versions without notes, with the repo open on a screen share. Every claim below is
true of v1.0.0 as built in steps 01 to 10. Replace every `[N]`, `[X]`, `[Y]` with your own
benchmark numbers (step 10, section 4) before you say them.

## 1. In plain words: what an interviewer wants from a project discussion

They are not checking whether tcpkv is impressive. They are checking three things:

1. **Did you build it?** (You can go to any line and say why it exists.)
2. **Do you understand the trade-offs?** (You know what you chose, what you rejected, and
   when your choice stops being right.)
3. **Can you reason about what you did not build?** (Asked "how would you add X", you design
   out loud instead of reciting.)

So the talk is structured as: what it is, how a request flows, the three or four decisions
that matter, how you know it is correct, what the numbers are, and what you would do next.
Lead with the request flow; it gives the interviewer a map to hang questions on.

## 2. The 2-minute version

Say this at roughly 140 words a minute. It is about 280 words. Practise with a timer.

> "tcpkv is a multithreaded TCP key-value server I wrote in C++17 on Linux to move my
> production C experience onto modern C++. At work I maintain a multithreaded RPC-over-TCP
> server in C on Windows; tcpkv is that kind of system reduced to its skeleton and done with
> the tooling a Linux team expects.
>
> A client sends text commands, like `SET`, `GET`, `DEL`, `EXPIRE`. Values are
> length-prefixed, so they are binary-safe and the parser never has to scan for escapes.
> One acceptor thread polls the listening socket, accepts, wraps the descriptor in a
> move-only `Connection`, and moves it into a fixed thread pool. A worker owns that
> connection for its lifetime: it reads, feeds an incremental parser, executes against an
> in-memory store, and writes all replies for that read in one write. The store is an
> `unordered_map` behind a `std::shared_mutex`, so reads run in parallel and writes are
> exclusive.
>
> Every resource is RAII: file descriptors, sockets, the connection. There is no `close`
> call outside one destructor. Shutdown on SIGINT uses the self-pipe trick: the handler sets
> an atomic flag and writes one byte to a pipe the acceptor polls; workers notice within one
> poll interval and the pool joins them.
>
> For correctness, there are GoogleTest suites for the parser, store, thread pool and
> end-to-end server, and CI runs them under AddressSanitizer, ThreadSanitizer and UBSan,
> plus a job that runs the server under Valgrind while a load generator hammers it. The load
> generator verifies every read. On a 4-vCPU WSL2 VM it does about [N] thousand operations a
> second with sixteen clients at a GET p99 of [X] microseconds.
>
> The main limit is thread-per-connection: concurrent clients are bounded by the thread
> count. The next step would be an epoll event loop."

Then stop and let them choose where to dig. Do not keep talking.

## 3. The 15-minute walkthrough

Screen-share the repo. Each block lists what to say and what to have open. Times are targets.

### 0:00–1:30 The pitch and the README

The 2-minute version, slightly shorter, with the README's architecture diagram on screen.
Point at the diagram as you describe the request flow.

### 1:30–4:00 The protocol (`docs/PROTOCOL.md`, `src/protocol.cpp`)

* Show the grammar and one `SET` on the wire: `SET k 5\nhello\n`.
* Why length-prefix (spec section 3.1): binary-safe, O(1) to find the end, pipelining is
  free, still typeable in `nc`. Quoting would mean an escape layer, the classic source of
  parser bugs. Binary framing would be faster to parse but useless with `nc`.
* The parser is an incremental state machine (`Line`, `Value`, `Dead`): feed it whatever
  `read()` returned, call `next()` until it says "need more". It never touches the socket,
  which is why it has about thirty unit tests (31 in `protocol_test.cpp` at v1.0.0, count
  yours) including byte-by-byte feeding.
* Fatal vs non-fatal errors: an unknown command skips one line; an oversized value or a
  missing terminator is fatal, because after that you cannot know where the next request
  starts. That split is the most important protocol decision; say it clearly.
* Limits: 256-byte keys, 1 MiB values, 4 KiB lines, 64 args. Each bounds memory per
  connection against a hostile client.

### 4:00–7:00 Concurrency (`src/server.cpp`, `src/thread_pool.cpp`, `src/connection.cpp`)

* `Server::run`: `poll` on the listener and the wake pipe; `accept4` with `SOCK_CLOEXEC`;
  `EMFILE` back-off (100 ms sleep instead of a 100% CPU spin); busy rejection above
  `max_pending_connections`.
* The ownership pipeline (step 06 section 2.4): socket created on the acceptor, moved into
  `Connection`, moved into a lambda, into `MoveOnlyFunction`, into the queue, out by a
  worker. Closed on whichever thread destroys it. No `shared_ptr`.
* Why `MoveOnlyFunction`: `std::function` requires a copyable callable; a lambda holding a
  move-only `Connection` is not copyable. Forty lines of type erasure; C++23 has
  `std::move_only_function`.
* Worker loop: `wait(lock, predicate)`; task runs outside the lock; exceptions caught so a
  task cannot kill a worker.
* Connection loop: `poll` with a 250 ms timeout so the stop flag is seen; one `write` per
  read, so pipelined requests get one batched reply.
* Store: `shared_lock` for `GET`/`EXISTS`/`KEYS`/`TTL`, `unique_lock` for writes; values
  copied out under the lock, never a reference.

### 7:00–9:00 Shutdown (`apps/tcpkv_server.cpp`, `Server::request_stop`)

* Signal handler: one atomic pointer load, one atomic store, one `write(2)`. All
  async-signal-safe. Why not log or notify a condition variable from the handler (deadlock
  on a lock the interrupted thread holds).
* `sigaction` without `SA_RESTART`, so `poll` returns `EINTR` and loops re-check the flag.
* Order: stop accepting, drop queued connections (their sockets close in destructors), let
  in-flight commands finish, join. The scope of `ThreadPool pool` inside `run()` encodes it.
* Show `ServerShutdown.RequestStopUnblocksRunAndClosesClients`: asserts under 2 s with
  clients connected.

### 9:00–11:30 Correctness evidence (`tests/`, `.github/workflows/ci.yml`, `tools/`)

* Four test layers (spec section 6): pure unit, unit with real descriptors, integration on
  an ephemeral port, system tests with the load generator.
* CI matrix: debug, release, ASan, TSan, UBSan; plus Valgrind on the server under load.
  Why separate jobs: ASan and TSan cannot be combined; Valgrind cannot run a sanitizer build.
* `load_gen` verifies every GET: disjoint key ranges per client, versioned values.
* The `EXPIRE` overflow story (step 09): every sanitizer job green, yet UB on one hostile
  input; found by hand with the UBSan build, fixed in the parser, pinned by a test. Tell it
  as "what sanitizers do not do for you".

### 11:30–13:00 Numbers (README benchmark table)

* Machine, pinning, warm-up, median of three, closed-loop clients.
* Server-side p99 ([Y] us) vs client p99 ([X] us): the store is not the bottleneck; the
  network stack and scheduling are.
* The 64-clients-on-8-threads row: the thread-per-connection limit made visible.

### 13:00–15:00 What I would do next, and questions

Epoll, persistence, replication (section 5). Then stop and invite questions.

## 4. Design decisions and the alternatives

For each: what you chose, why, what it costs, and when you would switch. That last part is
what makes the answer senior.

### 4.1 Thread pool (thread per connection) vs epoll event loop

| | fixed pool, one connection per worker (tcpkv) | epoll event loop(s), non-blocking sockets |
|---|---|---|
| concurrency limit | N connections served at once; the rest queue | thousands to millions of connections per thread |
| per-connection cost | a whole thread: stack reservation (glibc default is 8 MiB of virtual address space, committed lazily), a kernel task, scheduler work | a small state object (buffers, parser) |
| code shape | straight-line: read, parse, execute, write | state machine per connection; partial reads *and* partial writes must be resumed; `EPOLLOUT` handling; output buffering |
| idle cost | each idle connection wakes every 250 ms (our poll interval) | zero |
| latency for few active clients | good: no event dispatch | comparable |
| debuggability | a stack trace shows the whole request | the request is spread across callbacks |

**Why I chose the pool:** it is the design closest to the server I maintain at work, it is
easy to make correct, and it put the effort where the resume claims are (RAII, move-only
ownership, a correct pool, clean shutdown). It is a real production design for moderate
connection counts with short-lived or busy connections (many database connection handlers
and RPC servers work this way).

**When it stops being right:** many mostly-idle connections (chat, push, IoT, long-poll),
or connection counts above a few thousand. Then threads cost memory and scheduler time for
doing nothing, and N bounds concurrency.

**What epoll would look like here:** K event-loop threads (K = cores), each with its own
epoll instance; the listener either in one loop that hands accepted sockets round-robin to
the others, or `SO_REUSEPORT` with a listener per loop so the kernel balances. Each
connection: non-blocking socket, the existing `Parser`, an output buffer. Edge-triggered:
read until `EAGAIN`; write until `EAGAIN`, then arm `EPOLLOUT` and resume when writable.
Backpressure: stop reading from a connection whose output buffer exceeds a limit. The
`Store` and protocol code do not change, which is the payoff of keeping the parser free of
I/O. Shutdown: an `eventfd` per loop instead of the self-pipe.

### 4.2 One `std::shared_mutex` vs a plain mutex vs sharded locks vs lock-free

* **Plain `std::mutex`:** simplest; but readers serialise, and a KV store is read-heavy.
* **One `std::shared_mutex` (tcpkv):** readers in parallel. Costs: acquiring even a shared
  lock is an atomic read-modify-write on one shared word, so under many cores that cache line
  bounces between them; at high read rates a shared lock can be slower than you hope. On
  glibc it is reader-preferring, so continuous readers can starve writers
  (04_Concurrency/04_shared_mutex_and_readers_writers.md). Critical sections are one hash
  lookup plus a copy, which keeps both effects small at this scale.
* **Sharded (striped) locks:** S independent `{shared_mutex, unordered_map}` shards chosen by
  `hash(key) % S`. Writers to different shards stop contending, and the lock word stops being
  one hot line. Costs: `KEYS` and purge visit every shard and give no consistent snapshot;
  multi-key `DEL` must lock shards in a fixed order to avoid deadlock; more memory. **When:**
  after a profile shows time in `futex` waits on the store lock, not before.
* **Lock-free / RCU-style maps:** readers take no lock at all. Costs: memory reclamation
  (hazard pointers, epochs), much harder to get right, harder to review. Not justified
  without a measured need.

Say: "I would shard when the profile says so; with my numbers, the server-side p99 is [Y]
microseconds while the client sees [X], so the lock is not where the time goes."

### 4.3 Text protocol with length-prefixed values vs binary vs RESP

* **Text inline commands** for debuggability: `nc` and `telnet` work, logs are readable.
* **Length-prefixed values** for correctness: binary-safe, no escaping, O(1) framing,
  pipelining for free. It is the same idea as RESP bulk strings.
* **Not exact RESP:** RESP requests are arrays of bulk strings (`*2\r\n$3\r\nGET\r\n...`),
  which are harder to type by hand. tcpkv's replies are RESP-like (`+`, `-`, `:`, `$`, `*`),
  so a RESP reader would mostly parse them, but tcpkv is not Redis-compatible and should
  never be described that way.
* **Binary framing** (fixed header with opcode and lengths, like memcached's binary protocol
  or a protobuf-over-length-prefix scheme) parses faster and is smaller on the wire. It
  costs the ability to debug by eye. For a service-to-service API with a generated client,
  binary (or gRPC) is the better default.

### 4.4 Bounded `poll` timeout in workers vs other wake-up mechanisms

Workers block in `poll(fd, 250 ms)` and re-check an atomic flag. Alternatives:
`shutdown(fd, SHUT_RDWR)` on every client socket from the acceptor (needs a registry of
live sockets and a lock); `pthread_kill` with a signal per worker (fragile); an `eventfd`
polled alongside each socket (one more descriptor per connection, or one shared eventfd).
The timeout costs four wake-ups a second per idle connection and bounds shutdown latency to
250 ms. Simple, and the cost is visible and bounded. With epoll the answer becomes "one
eventfd per loop".

### 4.5 Error handling style

* Network input errors are **expected**, so the parser returns `ParseError` values; no
  exceptions on the per-request path.
* System call failures are **exceptional**: `std::system_error` with `errno`, caught at the
  connection boundary (a client vanishing is logged at debug and the connection ends).
* `std::optional` for "not found" in the store and the client.
* Startup failures (cannot bind) propagate to `main`, which logs and exits 1.

### 4.6 Smaller decisions worth one sentence each

* `unordered_map`, not `map`: O(1) average lookups; ordering is only needed by `KEYS`, which
  sorts its (usually small) result instead.
* `std::from_chars` everywhere: no locale, no exceptions, explicit overflow reporting.
* `MSG_NOSIGNAL` on every send *and* `SIGPIPE` ignored: a dead peer becomes `EPIPE`.
* `memcpy` from `sockaddr_storage` instead of pointer casts: avoids strict-aliasing UB.
* Warnings as an INTERFACE target so GoogleTest's code is not compiled with `-Werror`.
* Sanitizer flags applied to the whole tree so the library, tests and gtest agree.
* Relaxed atomics for metrics: independent counters, nothing published through them.

## 5. What next, and how you would design it

### 5.1 Epoll

Section 4.1. Add: measure first and after. The expected result is that idle connections
stop costing threads and the 64-clients row stops queueing; the per-request latency for a
few busy clients may not improve at all. Saying that shows you understand what epoll buys.

### 5.2 Persistence

Two classic options, both of which Redis offers:

* **Append-only log (AOF):** after executing a write, append the command to a log file;
  on start, replay it. Durability is set by the `fsync` policy: every write (safe, slow:
  each write waits for the disk), every second (lose up to about a second on power loss),
  or never (leave it to the OS). The log grows forever, so it needs rewriting: write a
  compact log from the current state and swap it in atomically (`rename`).
* **Snapshots:** periodically write the whole map. Redis uses `fork()` and copy-on-write so
  the child writes a consistent image while the parent keeps serving. In a multithreaded
  C++ server `fork` is dangerous (only the forking thread survives in the child, and any lock
  another thread held stays locked forever), so you would instead copy under the lock (pause)
  or iterate shard by shard (no consistent snapshot without versioning).

Order of operations for the log, to be able to state: apply to memory and append to the log
under the same ordering, reply to the client only after the append (and after `fsync` if
the policy demands it). Replying first is how acknowledged writes get lost. minidb is where
you built the real version of this: WAL before data, LSNs, checkpoints, recovery.

### 5.3 Replication

* **Leader/follower with log shipping:** followers connect to the leader and receive the
  same append-only stream; they apply it in order. Reads can go to followers if stale reads
  are acceptable.
* **Asynchronous** (reply after local apply): fast, but a leader crash loses writes not yet
  shipped. **Synchronous to a majority** (reply after a quorum acknowledges): no acknowledged
  write lost while a majority survives, at the cost of a network round trip per write.
* **Failover** needs agreement on who is leader, which is a consensus problem: Raft is the
  standard answer, and it also gives you the replicated log itself. Do not claim you would
  "just add Raft"; say you would use the Raft paper's design or an existing library, and
  that the hard parts are membership changes, snapshots, and testing with fault injection.

### 5.4 Other good answers

Sharded locks (4.2); `SCAN` instead of `KEYS`; `maxmemory` with sampled LRU; a Prometheus
endpoint; open-loop load generation to measure without coordinated omission; TLS via a
library such as OpenSSL with non-blocking handshakes (much easier with an event loop);
authentication (a shared secret per connection at minimum).

## 6. Likely questions, with answers

Grouped by topic, roughly easy to hard within each group. Answers are what you say, in two
to six sentences. Practise them aloud.

### Protocol and parsing

**1. Why a text protocol and not binary?**
Debuggability: I can test with `nc` and read traffic in `tcpdump`. Values are
length-prefixed, so the protocol is still binary-safe and framing is O(1). For a high-volume
internal API with generated clients I would choose binary or gRPC; for a tool people poke at
by hand, text wins.

**2. How does the parser handle a request split across two `read()` calls?**
It is incremental: it buffers bytes and keeps a state (`Line` or `Value`). `next()` returns
an event only when a complete line, or a complete value of the declared length plus its
terminator, is buffered; otherwise it returns "need more" and the connection reads again.
There is a test that feeds a `SET` one byte at a time and expects exactly one command.

**3. What happens if a client sends 10 GB without a newline?**
After 4096 bytes without `\n`, the parser emits a fatal `line too long` error, the server
replies and closes. Memory per connection is bounded by the line limit plus one value
(1 MiB) plus the read buffer. Without the limit, one client could exhaust server memory.

**4. Why are some errors fatal and others not?**
A non-fatal error (unknown command, wrong arity) is confined to one line, so the parser can
skip it and stay in sync. A fatal one (value too large, bad terminator) means the parser can
no longer know where the next request begins, so continuing could interpret value bytes as
commands. Closing is the only safe action.

**5. How does pipelining work?**
The client may send many requests before reading replies. The worker parses every complete
request in its buffer, appends each reply to one output string, and sends it with one
`write_all`. Replies come back in order because one connection is handled by one thread.

### Concurrency

**6. Why `shared_mutex` and not `mutex`?**
The workload is read-heavy and reads do not conflict, so a reader-writer lock lets `GET`s
run in parallel. I would revisit it if profiling showed the shared lock's own atomic
operations or writer starvation mattering; then sharding is the next step.

**7. Can a `GET` return a torn value?**
No. The value is copied out while the shared lock is held, and writers need the exclusive
lock to modify it, so a copy sees either the old or the new value completely. The stress
tests use values that are runs of one repeated letter so a torn read would be detectable,
and they run under TSan in CI.

**8. Why does the pool need `MoveOnlyFunction`?**
`std::function` requires its target to be copy-constructible, and a lambda capturing a
move-only `Connection` is not. `MoveOnlyFunction` is type erasure (a virtual `call` on a
heap-allocated `Impl<F>`) with copying deleted. C++23 adds `std::move_only_function` for
exactly this reason.

**9. Walk me through `wait` on a condition variable. Why the predicate?**
`wait(lock, pred)` loops: check the predicate under the mutex; if false, atomically release
the mutex and sleep; on wake, re-acquire and check again. The predicate handles spurious
wakeups and the case where another worker already took the task. Checking under the same
mutex the producer uses to push prevents the lost-wakeup race.

**10. What memory order is your stop flag, and why is that enough?**
The worker's load is relaxed: it only needs to eventually see `true`, and no other data is
published through the flag. The store in `request_stop` is release and `stop_requested()`
uses acquire, which costs nothing on x86 and documents intent. What makes the shutdown
correct is the join, which is a full synchronisation point.

**11. What happens to a connection that is queued when shutdown starts?**
`ThreadPool::stop()` moves the queued tasks out and destroys them outside the pool lock. The
lambda's destructor destroys the `Connection`, whose `Socket` closes the descriptor. The
client sees the connection close without a reply. There is a test with a `Probe` type that
counts destructions to prove dropped tasks are destroyed.

**12. What is the maximum number of concurrent clients?**
`--threads` clients are served at once. Up to `--max-pending` more wait in the pool queue;
beyond that new connections get `-ERR server busy` and are closed. That limit is the main
weakness of the design and the reason epoll is next.

**13. Why can't the signal handler just log "shutting down" and notify a condition variable?**
Both may take locks or allocate. If the signal interrupts a thread that holds the same
lock (the log mutex, the malloc arena lock), the handler waits for a lock its own thread
holds, forever. Only async-signal-safe functions are allowed: a lock-free atomic store and
`write(2)` are; `malloc`, `printf` and `pthread_cond_signal` are not.

### Resources and system calls

**14. How do you guarantee no descriptor leaks?**
One class, `FileDescriptor`, owns every descriptor and closes it in its destructor;
it is move-only so ownership is unique. `Socket` and `Connection` hold it by value. There is
no `close` call anywhere else in the code. Descriptors are created with `SOCK_CLOEXEC` /
`O_CLOEXEC` so they do not leak into child processes either. Valgrind's `--track-fds=yes`
confirms only stdin, stdout and stderr are open at exit.

**15. What does `write_all` handle that a single `send` does not?**
`send` on a stream socket may accept fewer bytes than asked (the socket buffer is full),
so `write_all` loops until every byte is sent. It retries on `EINTR`. It uses `MSG_NOSIGNAL`
so writing to a closed peer returns `EPIPE` instead of killing the process with `SIGPIPE`.
There is a test that writes 4 MiB to force partial sends.

**16. What happens when the process runs out of file descriptors?**
`accept` fails with `EMFILE`, but the pending connection stays in the kernel's accept queue,
so `poll` immediately reports the listener readable again: a busy loop at 100% CPU. The
acceptor logs and sleeps 100 ms instead, so existing connections can finish and free
descriptors.

**17. Why `TCP_NODELAY` in the client?**
Nagle's algorithm delays small sends while data is unacknowledged, and the peer's delayed
ACK delays the acknowledgement, so a request/response exchange can stall for the
delayed-ACK timeout. Disabling Nagle makes each request leave immediately, which is what a
request/response protocol wants.

### Testing and tooling

**18. How do you know there are no data races?**
I do not know there are none; I know TSan has not found any in the runs CI does, which are
designed to maximise interleavings (many threads, a small key space, no sleeps). TSan proves
the presence of races, not their absence. The design also keeps shared state minimal: the
store behind one lock, metrics as atomics, the pool queue behind its mutex, and nothing else.

**19. Why both Valgrind and ASan?**
They catch different things. ASan is fast enough for the whole test suite and finds stack and
global overflows that memcheck cannot. Valgrind needs no special build, checks the binary as
shipped, and finds reads of uninitialised memory, which ASan does not. They cannot run
together, so they are separate CI jobs.

**20. Tell me about a bug the tools found.**
The best one is the one they missed. `EXPIRE key 10000000000` converted `std::chrono::seconds`
to the steady clock's nanoseconds, multiplying by 10^9 in a signed 64-bit integer: signed
overflow, undefined behaviour from one line of client input. Every CI job was green because
no test sent a big number. I found it by sending hostile inputs to the UBSan build by hand,
bounded the argument in the parser, and added a test. (Replace or supplement this with
whatever you actually hit while building; a real story you lived beats a good one you read.)

**21. How did you test graceful shutdown?**
An integration test starts the server in a thread with an idle client and a busy one,
calls `request_stop`, and asserts `run()` returns within two seconds and both clients see
the connection closed. A second test calls `request_stop` before `run` and expects an
immediate return. Manually: Ctrl-C under Valgrind, which also proves the shutdown path frees
everything.

### Performance

**22. What is your throughput and what limits it?**
About [N] thousand operations a second with sixteen clients on sixteen worker threads, on a
4-vCPU WSL2 VM with the server and load generator pinned to separate cores. Server-side
execution is [Y] us at p99 while clients see [X] us, so the time is in syscalls, loopback
TCP and scheduling, not the store. One request per round trip per client is the real
limit; pipelining in the client or more connections would raise throughput.

**23. Is your p99 trustworthy?**
It is a bucket upper bound from a closed-loop load generator. Closed loop under-samples slow
periods (coordinated omission): a stalled request delays that client's next request, so the
stall is counted once instead of for every request that would have arrived. For a stricter
number I would use an open-loop generator with a fixed arrival rate.

**24. Where would you look first if latency spiked in production?**
`STATS` for server-side p99 versus what clients report: if the server-side number is flat,
the time is outside the store (queueing for a worker, network, GC-like pauses in clients).
Then `connections_active` against `--threads` (all workers busy means requests queue),
`top -H` for a hot thread, `perf top` for where CPU goes, and off-CPU analysis for time
spent waiting in `futex` on the store lock. A large `KEYS` or the per-second purge holding the
exclusive lock is a prime suspect.

### Design and C++

**25. Why is `Connection` move-only? Why not `shared_ptr<Connection>`?**
A connection has exactly one owner at a time: the acceptor, then the queue, then a worker.
Move-only makes the compiler enforce that and makes the close happen exactly once, on
whichever thread destroys it. `shared_ptr` would add atomic reference counting and hide the
question of who closes the socket.

**26. Why does `Connection` hold pointers to `Store` and the stop flag, not references?**
Reference members delete the assignment operators, so the class would not be
move-assignable. Pointers keep it a regular movable value. The lifetimes are guaranteed by
scope in `main` and `run()`: the store outlives the server, which outlives the pool, which
outlives every connection.

**27. Why is `Server` not movable?**
The signal handler holds a pointer to it (`g_server`). If it could move, the pointer could
dangle. Deleting copy and not declaring move operations makes it immovable, and `main`
clears `g_server` before the server is destroyed.

**28. Exceptions in a server: where do you throw and where do you catch?**
System call failures throw `std::system_error`; they are caught at the connection boundary,
so one broken client cannot affect others, and in the worker loop as a backstop, so an
exception can never terminate a worker thread. Protocol errors are values, because bad input
from the network is normal, not exceptional.

**29. How would you make `KEYS` safe on a million keys?**
Today it scans every key under the shared lock, blocking writers for the whole scan. I would
add `SCAN` with a cursor and a count, so each call holds the lock briefly, or keep an ordered
index so a prefix query is a `lower_bound` plus a walk. Redis documents `KEYS` as unsafe in
production for exactly this reason.

**30. If you rewrote it tomorrow, what would you change?**
I would start with the event loop design so the connection limit goes away, keep the parser
and store as they are (they are I/O-free and well tested), and add a windowed latency
histogram so the periodic log line describes the last interval rather than all time. I would
also write the hostile-input tests (huge numbers, every limit plus one) on day one rather
than discovering the `EXPIRE` overflow late.

## 7. Weaknesses to state before they are found

Saying these first is a strength. Have each one ready in one sentence.

* Thread per connection: concurrency bounded by `--threads`; idle connections cost threads.
* No persistence, no replication, no auth, no TLS. Memory only, single node.
* `KEYS` and the TTL purge are O(n) under a lock.
* One `shared_mutex` for the whole store.
* The p99 in the log is since start, not windowed; percentiles are bucket bounds.
* Benchmarks are on a WSL2 VM over loopback; no cross-machine numbers.

## 8. Mapping to your experience at work

When asked "how does this relate to what you do?":

* The application server at work is a multithreaded RPC-over-TCP server in C on Win32;
  tcpkv is the same architecture with RAII instead of `goto cleanup`, move-only ownership
  instead of conventions about who frees what, and sanitizers instead of finding the bug in
  a crash dump at a bank.
* The crash-dump and RCA work maps to step 08: the bug classes ASan and TSan catch are the
  ones you have diagnosed after the fact in WinDbg. Now you can say how you prevent them.
* The Berkeley DB to YugabyteDB migration maps to minidb and to the persistence answer in
  section 5.2: you know what a storage engine promises because you moved data between two.

Keep the employer's code, data and client names out of every example. Talk about the shape
of the problems, never the specifics.

## 9. Recap: ten sentences to know cold

1. One acceptor, a fixed pool, one connection per worker for its lifetime.
2. Length-prefixed values: binary-safe, O(1) framing, typeable in `nc`.
3. Incremental parser with fatal and non-fatal errors; limits on every buffer.
4. `unordered_map` behind one `shared_mutex`; values copied out under the lock.
5. RAII for every descriptor; `Connection` is move-only and travels by move into the pool.
6. Self-pipe shutdown: atomic flag plus one `write` from the handler; bounded `poll` in workers.
7. Tests in four layers; CI under ASan, TSan, UBSan and Valgrind under load.
8. A load generator that verifies every read.
9. [N]k ops/s at GET p99 <= [X] us, with the conditions.
10. The limit is thread-per-connection; next is epoll, then persistence, then replication.
