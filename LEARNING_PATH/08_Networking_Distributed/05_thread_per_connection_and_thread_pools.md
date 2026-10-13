# 05. Thread-per-connection and thread pools

**Time:** ~3 hours   **Prerequisites:** 03, 04, and 04_Concurrency modules on threads, mutexes and condition variables

## 1. In plain words

The echo server in module 03 serves one client at a time. The second client waits in the accept
queue until the first hangs up. To serve many clients you need many "operators". The simplest way
is to start a new thread for every connection: each thread runs the same blocking read-parse-write
loop and the operating system interleaves them. This is **thread-per-connection**. It works well up
to a few hundred or a few thousand connections and then runs into memory and scheduling costs.

The next step is to hire a fixed staff instead of a new person per caller: a **thread pool** of N
workers and a queue of waiting connections. N bounds your resource use; the queue absorbs bursts;
anything beyond that is turned away politely. This is how your employer's RPC server is built
(a listener thread plus worker threads), how PostgreSQL works (one process per connection, which
is why it needs pgbouncer), and how most Java servers worked for twenty years.

The hard parts are not the threads. They are the edges: a client that connects and never sends,
a client that sends and never reads, and stopping the server without cutting off requests
mid-flight. This module builds tcpkv v1 with all three handled.

## 2. The real thing

### 2.1 Thread-per-connection

```cpp
for (;;) {
    net::Fd c = net::accept_tcp(listener.get());
    std::thread(serve_connection, std::move(c)).detach();   // or keep and join at shutdown
}
```

Costs per thread on Linux:
- **Stack**: 8 MB virtual by default (`ulimit -s`), but only touched pages are physical, typically
  tens of KB. 10,000 threads reserve 80 GB of address space, fine on 64-bit, impossible on 32-bit.
  You can lower it with `pthread_attr_setstacksize` (not through `std::thread`).
- **Kernel structures**: a `task_struct` and kernel stack (16 KB), a few KB more.
- **Creation**: `clone()` costs tens of microseconds. Creating a thread per connection for
  short-lived connections is noticeable; for long-lived connections it is nothing.
- **Scheduling**: 10,000 runnable threads make the scheduler work hard; context switches cost about
  1 to 5 microseconds each plus cache pollution. 10,000 *idle* threads blocked in `recv` cost almost
  nothing in CPU; they are just memory.

**The C10K problem** (Dan Kegel, 1999) asked how to serve 10,000 simultaneous connections on one
machine. Thread-per-connection was the first answer to break, mainly from memory and scheduler
overhead on machines of that time. The solutions were event loops (module 06). Today a 64-bit Linux
box handles 10,000 mostly-idle threads without drama, and the practical limit of
thread-per-connection is more like "when connections are many and mostly idle, you are wasting
memory; when they are few and busy, threads are fine". Say that in an interview rather than "threads
do not scale".

### 2.2 Thread pool + blocking I/O

```
                 +-----------+     bounded queue      +----------+
  clients  --->  | acceptor  | ---> [c][c][c][ ] ---> | worker 1 |  blocking read/parse/write
                 |  thread   |                        | worker 2 |
                 +-----------+                        |   ...    |
                       |  queue full?                 | worker N |
                       +--> reply -ERR_BUSY, close    +----------+
```

- N workers = N concurrently served connections. For CPU-bound work, N near the core count. For
  work that blocks on a database, N much higher (hundreds) because most workers are waiting.
- The queue bounds how many accepted-but-unserved connections wait. **Bound it.** An unbounded queue
  turns an overload into a memory and latency disaster; a bounded queue turns it into fast, explicit
  rejections that the client can retry elsewhere. This is **load shedding**.
- Hand-off granularity: this design hands a whole *connection* to a worker, which then serves all
  its requests. Alternative: hand each *request* to the pool (requires an I/O thread that frames
  requests, which is module 06's design). Connection-per-worker is simpler and is what blocking I/O
  allows.

### 2.3 Connection lifecycle

```
accept -> [queue] -> worker takes it -> set options (NODELAY, timeouts)
  -> loop { wait readable (poll, 1 s tick) ; read ; parse ; execute ; write }
  -> ends on: EOF (client FIN) | QUIT | idle timeout | fatal protocol error | I/O error | shutdown
  -> remove from active set -> close (RAII)
```

Each exit reason is a counter. Operators look at those counters.

### 2.4 Timeouts: never block without a bound

A worker blocked forever on one client is a worker lost. Sources of forever:
- Client connects and sends nothing (half-open, or a port scanner). **Idle timeout** on read.
- Client sends half a message and stops. Same idle timeout; the parser just has partial state.
- Client stops reading replies; our `send` fills the socket buffer and blocks. **Send timeout.**
- The peer host died silently. Keepalive (module 02) or the idle timeout.

Two mechanisms:
1. `SO_RCVTIMEO` / `SO_SNDTIMEO`: the blocking call returns `EAGAIN` after the timeout. Simple.
   Downside: the timeout applies per call, you cannot wake the thread early for shutdown, and
   `SO_RCVTIMEO` is ignored by some wrappers.
2. `poll(fd, POLLIN, timeout_ms)` before each `read`: returns 0 on timeout without touching the
   socket. You can use a short tick (1 s) and check a `stopping` flag and the real deadline on each
   tick. The server uses this for reads and `SO_SNDTIMEO` for writes.

Deadlines, not durations: compute `deadline = now + idle_timeout` once per received byte, compare
`now >= deadline` on each tick. Repeated "sleep 30 s" style logic drifts and makes shutdown slow.

### 2.5 Graceful shutdown

The sequence that avoids cutting requests in half:

1. **Stop accepting.** Close or `shutdown()` the listener. New connection attempts get RST
   (`ECONNREFUSED`), which is a clear signal to a load balancer or retrying client. If a load
   balancer is in front, first fail the health check and wait for it to stop routing (module 13).
2. **Tell the workers.** Set `stopping`. Idle workers exit. Busy workers finish the current
   request, then stop at the next tick (they may send a final "shutting down" error to an idle client).
3. **Drain** with a deadline (10 to 30 s). Wait for the active-connection count to reach zero.
4. **Force** what remains: `shutdown(fd, SHUT_RDWR)` on each active fd makes its blocked `recv`
   return 0 and its blocked `send` fail, so the worker exits. Then `join` everything.
5. Report counters, flush logs, exit 0.

Signal handling in a multithreaded process: block `SIGINT`/`SIGTERM` in the main thread before
creating any other thread (they inherit the mask), then `sigwait` in main. No async-signal-safety
worries, no `volatile sig_atomic_t` flags, no self-pipe. This is the cleanest pattern on Linux.
(Windows: `SetConsoleCtrlHandler` or a service control handler sets an event; the main thread waits
on it.)

Why `shutdown()` wakes a blocked reader when `close()` does not: `close()` only drops this fd; if
another thread is inside `recv` on it, that thread holds a reference and keeps sleeping (and the fd
number can be reused by a new `accept`, so the sleeping thread now reads from a different
connection, which is a nasty bug). `shutdown()` acts on the socket itself.

### 2.6 Backlog and `somaxconn`

`listen(fd, backlog)`: the accept queue holds completed handshakes until you call `accept`. If the
acceptor thread is slow (or you have one thread doing accept and serve, like module 03), the queue
fills, new SYNs are dropped, clients retry after 1 s and 3 s: "connecting takes exactly 1 or 3
seconds sometimes". Set backlog to 1024 or more on a busy server, and keep the acceptor thread
doing nothing but `accept` and enqueue. The kernel caps it at `net.core.somaxconn` (4096 on
kernels since 5.4; 128 on older ones, which bit many people).

### 2.7 File descriptor limits

Each connection is an fd. `ulimit -n` (soft limit, 1024 by default on many systems) caps the
process. At the limit, `accept` fails with `EMFILE`; the pending connection stays in the accept
queue and the listener stays readable, so a naive loop spins. Handle `EMFILE` by sleeping briefly
and logging loudly. Raise the limit with `ulimit -n 65536` in the start script, or
`LimitNOFILE=65536` in a systemd unit, or `setrlimit(RLIMIT_NOFILE)` at startup up to the hard limit.
Also check `/proc/sys/fs/file-max` (system-wide) and `/proc/sys/fs/nr_open`.

Windows has no fd limit of this kind; sockets are kernel handles, limited by non-paged pool and
the `MaxUserPort`/dynamic port range for outbound connections.

## 3. Code

### 3.1 tcpkv v1 (`code/m05_threadpool_tcpkv_server.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m05_threadpool_tcpkv_server.cpp -o tcpkv
./tcpkv 7000 16 &
printf 'SET a 5\r\nhello\r\nGET a\r\nEXISTS a\r\nDEL a\r\nGET a\r\nQUIT\r\n' | nc -q1 127.0.0.1 7000
# +OK  $5 hello  :1  :1  $-1  +OK
python3 m05_loadtest.py --port 7000 -c 50 -n 2000
python3 m05_loadtest.py --port 7000 -c 50 -n 2000 --pipeline 32     # far higher throughput: why?
kill -TERM %1                                                       # graceful shutdown + report
```

The store: `std::unordered_map` behind a `std::shared_mutex` (readers share, writers exclude).

The worker loop:

```cpp
void worker_main(Server& srv, int) {
    for (;;) {
        net::Fd conn;
        {
            std::unique_lock<std::mutex> lk(srv.q_mu);
            srv.q_cv.wait(lk, [&] { return srv.stopping.load() || !srv.queue.empty(); });
            if (srv.queue.empty()) return;              // stopping and drained
            conn = std::move(srv.queue.front()); srv.queue.pop_front();
        }
        serve_connection(srv, std::move(conn));
    }
}
```

Serving one connection, with every wait bounded:

```cpp
auto deadline = Clock::now() + srv.idle_timeout;
for (;;) {
    if (!net::wait_readable(fd, 1000)) {                 // 1 s tick: re-check flags, no busy loop
        if (srv.stopping) { net::write_all(fd, fmt_err("ERR_SHUTDOWN", "...")); return; }
        if (Clock::now() >= deadline) { ++srv.idle_timeouts; return; }
        continue;
    }
    size_t n = net::read_some(fd, buf, sizeof buf);
    if (n == 0) return;                                  // client FIN
    deadline = Clock::now() + srv.idle_timeout;
    parser.feed(buf, n);
    out.clear();
    for (;;) {                                           // drain all complete commands
        auto st = parser.next(cmd, err);
        if (st == NeedMore) break;
        if (st == Error) { out += fmt_err(err.code, err.message); if (err.fatal) {quit = true; break;} continue; }
        out += execute(srv, cmd);
        if (cmd.op == Op::Quit) { quit = true; break; }
    }
    if (!out.empty()) net::write_all(fd, out);           // ONE send for the whole batch
    if (quit) return;
}
```

Notice the batching: all replies for one `read` go out in one `send`. With `--pipeline 32` the
load tester shows the effect: fewer syscalls, fewer packets, much higher throughput. This is the
cheapest optimisation in network programming and the reason Redis pipelining exists.

The acceptor with load shedding:

```cpp
std::unique_lock<std::mutex> lk(srv.q_mu);
if (srv.queue.size() >= srv.queue_cap) {
    lk.unlock(); ++srv.rejected;
    net::write_all(conn.get(), fmt_err("ERR_BUSY", "server overloaded, retry later"));
    continue;                                            // conn closes; client knows why
}
srv.queue.push_back(std::move(conn)); lk.unlock(); srv.q_cv.notify_one();
```

The shutdown sequence in `main` is section 2.5 line by line: `sigwait`, `shutdown(listener)`,
join acceptor, `stopping = true`, `notify_all`, reject queued connections with a message, drain
with a 10 s deadline, `shutdown(SHUT_RDWR)` the stragglers, join workers, print counters.

### 3.2 The load tester (`code/m05_loadtest.py`)

Python with one thread per connection. Each round sends `SET k v` and `GET k`, parses the
replies with an incremental `ReplyParser` (yes, the Python client also needs one), and checks the
value round-trips. `--pipeline N` sends N rounds before reading. Reports ops/s and p50/p90/p99.

Experiments worth running:
- `-c 4 -n 5000` vs `-c 64 -n 500` with 8 workers: see queueing when connections exceed workers
  (connections beyond 8 wait in the queue until an earlier one finishes all its rounds).
- `-c 300` with `queue_cap` 256 and 8 workers: see `ERR_BUSY` rejections in the error list.
- Connect with `nc` and type nothing: watch it get closed after the idle timeout.
- `kill -TERM` during a load test: the test reports a few `ERR_SHUTDOWN` errors, no hangs, no
  truncated replies.

## 4. Under the hood (senior-level)

**What a blocked thread costs.** A thread in `recv` is in state S (interruptible sleep) on the
socket's wait queue. It consumes no CPU. Its kernel stack (16 KB) and touched user stack pages stay
resident. When data arrives, the softirq handler wakes it (`wake_up_interruptible`), the scheduler
puts it on a run queue, and it runs on the next tick or immediately if a CPU is idle. Wake-up latency
is typically a few microseconds to tens of microseconds; with thousands of threads waking for small
messages, this scheduler work dominates, which is the real C10K cost.

**Condition variable hand-off.** `notify_one` wakes one worker; it must re-acquire the mutex, so
the acceptor should unlock before notifying (the code does) to avoid the woken thread immediately
blocking on the mutex ("hurry up and wait"). With many idle workers and bursts of connections,
`notify_one` per connection is right; `notify_all` would cause a thundering herd.

**`shared_mutex` trade-off.** Reader-writer locks cost more than a plain mutex when the critical
section is tiny (a hash lookup) because they touch more atomics. They win when reads are long or
heavily concurrent. For a real KV store you shard the map into 16 or 64 stripes each with its own
mutex (module 12 at micro scale). Measure with `perf` before choosing.

**False sharing in counters.** Six `std::atomic<uint64_t>` counters declared next to each other
share a cache line; every increment from any worker bounces that line between cores. For a
learning server it does not matter. In a hot path, pad counters to 64 bytes or keep per-thread
counters and sum on read.

**Why the 1 s poll tick instead of blocking forever.** A thread blocked in `recv` with no timeout
can be woken only by data, a signal, or `shutdown()` from another thread. Periodic polling costs
one syscall per idle connection per second, trivial at thousands of connections, and makes
deadlines and shutdown checks local to the thread with no cross-thread signalling. At 100,000 idle
connections it becomes measurable and you want epoll with a timer wheel (module 06).

**The fd-reuse race.** Thread A is blocked in `recv(fd 7)`. Thread B calls `close(7)`. The kernel
keeps the socket alive until A's `recv` returns (it holds a reference), but the *number* 7 is free
and the next `accept` may return 7 for a new client. If A then wakes and the code uses fd 7 again,
it talks to the wrong client. Rules: only the owning thread closes; other threads use `shutdown()`
to wake it; the active-set bookkeeping in the server removes the fd before `close` runs.

**Windows mapping.** The same architecture maps directly: a listener thread calling `accept`,
worker threads with `recv`/`send`, `WSAPoll` or `select` with timeout for the tick, `shutdown(SD_BOTH)`
to wake a blocked reader, an event set by the console control handler for shutdown. Windows also
has a built-in thread pool (`CreateThreadpoolIo`) that integrates with IOCP, which is module 06's
territory.

## 5. In production

- **The slow-reader deadlock.** Client sends many requests without reading replies. Server replies
  fill the client's receive buffer, then the server's send buffer; the server's `send` blocks. The
  client is blocked in its own `send` because the server stopped reading (its worker is stuck in
  `send`). Both wait forever. This is why the server has a send timeout and why robust clients read
  and write on separate threads or use non-blocking I/O. Your RPC transport: does the server ever
  block on `send` to a client? What bounds it?
- **Worker starvation by idle connections.** A monitoring tool opened a connection per check and
  never closed them. With 16 workers and 16 idle monitors, real clients waited in the queue forever.
  The idle timeout fixed it; the queue-wait metric found it.
- **Unbounded queue, 40 GB RSS.** A server queued incoming requests without limit during a
  downstream slowdown. Memory climbed until the OOM killer took the process, which dropped every
  queued request anyway. A bounded queue with fast rejection would have shed 10% of the load and
  kept 90% working. Bound every queue.
- **Shutdown that took 15 minutes.** `SIGTERM` handler set a flag; workers checked it only between
  connections, and connections were long-lived. systemd waited 90 s then `SIGKILL`ed, cutting
  requests in half. The fix is this module's sequence: tick-based checks, drain deadline, forced
  `shutdown()` on stragglers.
- **`somaxconn` 128 on an old kernel.** During a traffic spike the accept queue overflowed;
  clients saw 1 to 3 s connect delays; `nstat -az TcpExtListenOverflows` was climbing. Raising
  backlog in the code and `somaxconn` on the host fixed it.
- **`ulimit -n 1024` in the systemd unit.** A service tested fine, then in production hit `EMFILE`
  at 1,000 connections and spun at 100% CPU logging accept errors. `LimitNOFILE=65536` and an
  `EMFILE` backoff fixed both halves.
- **How this maps to the banking server.** Listener thread, worker pool, blocking I/O per
  connection, a per-connection write mutex: tcpkv v1 is a small version of it. The questions to
  take back: what is the queue bound, where are the timeouts, what does shutdown do with in-flight
  loans, and what counters exist.

## 6. Try it

1. **(easy)** Build and run tcpkv with 2 workers. Connect three `nc` sessions. Observe that the
   third gets no replies until one of the first two quits. Done when: you can explain which queue
   it sat in and how you would detect this in production.

2. **(easy)** Connect with `nc`, type nothing, wait for the idle timeout (lower it to 5 s for the
   test). Done when: `nc` exits and the server's `idle_timeouts` counter increments at shutdown.

3. **(medium)** Add a per-connection `max_commands` or a per-connection rate limit (module 09 has
   a token bucket). Done when: a load test with `--pipeline 100` gets `ERR_BUSY` replies for the
   excess instead of unbounded service.

4. **(medium)** Reproduce the slow-reader deadlock: write a Python client that sends 100,000 `SET`
   commands without ever reading. Observe the server worker blocked in `send`, then observe the send
   timeout free it. Remove `set_send_timeout` and observe the permanent hang. Done when: you have
   seen both behaviours in `ss -tanp` (Send-Q full on both sides).

5. **(hard)** Replace connection-per-worker with request-per-worker: one I/O thread per connection
   that only reads and frames, a shared request queue, a pool that executes and writes replies.
   Keep replies in order per connection. Done when: the load test passes and you can state what
   got better and what got more complicated.

6. **(hard)** Add `SIGHUP` handling that re-reads a config file (idle timeout, queue cap) without
   restarting, and `SIGUSR1` that prints the counters. Done when: both work during a load test with
   no dropped connections.

## 7. Interview questions

**Q: What is the C10K problem and does it still matter?**
A: Serving 10,000 concurrent connections on one machine, which broke thread-per-connection
designs in 1999 because of memory and scheduler overhead. Modern 64-bit Linux handles 10,000
mostly-idle threads, but the lesson stands: many idle connections waste memory and wake-up work
with threads, and event loops (epoll) are the answer at 100K+ connections.

**Q: Thread per connection versus thread pool: when each?**
A: Thread per connection for few, long-lived, busy connections; it is simple and the OS schedules
fairly. Thread pool when you must bound resources and shed load: N workers, bounded queue, explicit
rejection beyond that. Both block one thread per active connection; event loops decouple that.

**Q: How do you shut down a server gracefully?**
A: Stop accepting (close the listener, fail the health check first if behind a load balancer),
signal workers to finish current requests, drain with a deadline, force-close stragglers with
`shutdown()` so blocked reads return, join threads, flush logs, exit 0. Handle signals via a
blocked mask and `sigwait` in the main thread.

**Q: Why `shutdown()` and not `close()` to interrupt a thread blocked in `recv`?**
A: `close` only drops the caller's reference; the blocked thread holds another and keeps sleeping,
and the fd number can be reused by a new `accept`, so the sleeper may later read the wrong
connection. `shutdown` acts on the socket and makes the blocked `recv` return 0 immediately.

**Q: What does the `backlog` argument do?**
A: Caps the queue of completed connections waiting for `accept` (also capped by `somaxconn`). When
full, new SYNs are dropped and clients retry after 1 s and 3 s, showing up as sporadic connect
delays. Set it high and keep the accept loop fast.

**Q: Why bound the work queue?**
A: An unbounded queue converts overload into unbounded memory growth and latency; eventually
everything fails. A bounded queue rejects the excess fast with a clear error, so most requests
still succeed and clients can retry elsewhere. Load shedding beats collapse.

**Q: Why timeouts on both read and write?**
A: Read timeout frees workers from idle or half-sending clients. Write timeout frees workers from
clients that stopped reading, which otherwise causes a deadlock when both sides are blocked in
`send`. Any unbounded wait is a resource leak waiting for the right client.

## 8. Recap

- Thread-per-connection: simple, fine for hundreds of busy connections; costs stack, kernel
  structures and scheduler work per thread.
- Thread pool + bounded queue: N concurrent connections, explicit rejection beyond, load shedding.
- Connection lifecycle ends on EOF, QUIT, idle timeout, fatal protocol error, I/O error or shutdown;
  count each.
- Bound every wait: `poll` with a tick for reads, `SO_SNDTIMEO` for writes, deadlines not durations.
- Graceful shutdown: stop accepting, signal, drain with deadline, `shutdown()` stragglers, join.
- Block signals before creating threads and `sigwait` in main.
- `shutdown()` wakes blocked readers; `close()` does not and invites fd reuse bugs.
- Backlog and `somaxconn` size the accept queue; overflow shows as 1 or 3 s connect delays.
- `ulimit -n` caps fds; `EMFILE` needs backoff; fix the leak.
- Batch replies per read into one `send`; pipelining multiplies throughput for free.
