# 06. Non-blocking I/O, epoll and event loops

**Time:** ~3.5 hours   **Prerequisites:** 03, 04, 05

## 1. In plain words

In module 05 each connection had a thread that sat waiting for it. The thread's stack remembered
"where we were" for that connection. An **event loop** flips this: one thread asks the kernel
"which of my 10,000 sockets has something for me right now?", gets a short list, handles each one
without ever waiting, and asks again. Because the thread never waits on a single socket, every
socket must be **non-blocking**: a `read` with no data returns `EAGAIN` instead of sleeping.

The analogy: a restaurant with one waiter and a board of lights. Instead of standing at each table
(thread per connection), the waiter looks at the board, sees which tables lit their lamp, serves
exactly those, and looks again. The waiter must never get stuck at one table, and must write down
on a notepad anything half-finished (the order that is half taken) because there is no "standing
there" to remember it. That notepad is the per-connection state object.

**epoll** is Linux's board of lights. **select** and **poll** are older, slower boards. Windows has
a different design, **IOCP**, where instead of lights the kitchen delivers finished plates to the
waiter. This module writes an epoll echo server from scratch, then shows how libuv, Boost.Asio,
io_uring and IOCP fit the same two patterns.

## 2. The real thing

### 2.1 Non-blocking sockets

`fcntl(fd, F_SETFL, O_NONBLOCK)` or `accept4(..., SOCK_NONBLOCK)` / `socket(..., SOCK_NONBLOCK)`.
Then:
- `recv` with nothing buffered returns -1 with `EAGAIN` (same value as `EWOULDBLOCK` on Linux).
- `send` with a full send buffer writes what fits (possibly 0 bytes, then `EAGAIN`).
- `accept` with an empty queue returns `EAGAIN`.
- `connect` returns -1 with `EINPROGRESS`; completion is signalled by writability; the result is
  read with `getsockopt(SO_ERROR)`.

`EAGAIN` means "not now, ask the readiness API when to try again". It is the normal case, not an error.

### 2.2 select, poll, epoll

**`select`**: pass three bitmaps (read, write, error) of fds and a timeout. O(max fd) to build and
scan; `FD_SETSIZE` is 1024 on glibc. Portable, and on Windows `fd_set` is an array of 64 sockets.
Do not use for servers.

**`poll`**: pass an array of `pollfd {fd, events, revents}`. No 1024 limit, but O(n) in user and
kernel space on every call: with 10,000 connections the kernel walks 10,000 entries per call even if
one is ready. Fine for a handful of fds (module 05 uses it for one fd with a timeout).

**`epoll`**: the kernel keeps the interest list. You register fds once (`epoll_ctl ADD`), then
`epoll_wait` returns only the ready ones. O(ready), not O(registered). Linux only. BSD/macOS have
`kqueue` with the same idea; Solaris has event ports.

```cpp
int ep = epoll_create1(EPOLL_CLOEXEC);
epoll_event ev{}; ev.events = EPOLLIN | EPOLLET; ev.data.ptr = conn;    // data is yours: ptr or u64
epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
epoll_event out[256];
int n = epoll_wait(ep, out, 256, timeout_ms);       // -1 = block until something is ready
for (int i = 0; i < n; ++i) handle(out[i].data.ptr, out[i].events);
```

Events: `EPOLLIN` readable (or EOF), `EPOLLOUT` writable, `EPOLLRDHUP` peer half-closed, `EPOLLERR`
and `EPOLLHUP` always reported even if not requested. Modifiers: `EPOLLET`, `EPOLLONESHOT`,
`EPOLLEXCLUSIVE`.

### 2.3 Level-triggered vs edge-triggered

**Level-triggered (LT, default)**: "report this fd as ready as long as it is ready". If data is
buffered and you read only some of it, the next `epoll_wait` reports it again. Forgiving. Like
`poll`. Cost: a readable fd you do not fully drain is reported every iteration; and a thread pool
sharing one LT epoll gets thundering-herd wakeups for the same fd.

**Edge-triggered (ET, `EPOLLET`)**: "report once when the fd *becomes* ready". If 10 KB arrive and
you read 4 KB, you will **not** be told again until *more* bytes arrive. Rule: after an ET event,
loop reading until `EAGAIN`. Same for accept: loop until `EAGAIN`. Same for write: when `EPOLLOUT`
fires, write until `EAGAIN` or your buffer is empty. Miss the rule and a connection hangs with data
in the kernel buffer, forever, with no event: the classic ET bug.

ET gives fewer wakeups and works well with multiple threads on one epoll fd (each edge wakes one
waiter). The server in this module uses ET everywhere to make the drain rule a habit.

**`EPOLLONESHOT`**: after reporting an fd once, disable it until you `EPOLL_CTL_MOD` it back on.
Use case: a pool of threads calling `epoll_wait` on the same epoll fd; ONESHOT guarantees one fd is
handled by one thread at a time, and the thread re-arms when done. Cost: one `epoll_ctl` per event.

### 2.4 The reactor pattern

```
          +--------------------------------------------------+
          |  loop:                                           |
          |    events = epoll_wait()                          |
          |    for each event: dispatch to handler(conn, ev)  |
          +--------------------------------------------------+
 handlers:   on_accept       on_readable       on_writable       on_timer      on_wake(eventfd)
             accept until    recv until        send until        sweep idle    stop
             EAGAIN, add     EAGAIN, parse,    EAGAIN or empty,  conns
             to epoll        queue replies     update interest
```

A **reactor** demultiplexes readiness events and calls handlers that perform the I/O themselves.
Rules for a correct reactor:
1. Handlers never block: no blocking I/O, no `sleep`, no blocking DNS, no long computation. One
   slow handler stalls every connection. Offload CPU-heavy or blocking work to a thread pool and
   bring the result back through an `eventfd` or a pipe.
2. All per-connection state lives in an object the event's `data.ptr` points to.
3. Writes go to a per-connection output buffer first; try to flush immediately; if the kernel
   buffer is full (`EAGAIN`), register `EPOLLOUT` and finish on the writable event. Never block.
4. Register `EPOLLOUT` only while you have pending output. A socket is writable almost always, so
   a permanently registered `EPOLLOUT` with ET is harmless but with LT spins the loop at 100% CPU.
5. **Backpressure**: if a connection's output buffer grows past a high-water mark (the peer is not
   reading), stop reading from it (remove `EPOLLIN`). TCP flow control then slows the peer. Resume
   when the buffer drains below a low-water mark. Without this, one bad client eats all your memory.
6. Closing inside a handler invalidates the `Conn` object; any later event in the same batch for
   that fd must detect this (look it up again) before touching it.

### 2.5 Per-connection state

```cpp
struct Conn {
    int fd;
    std::string out;            // what we owe the peer
    tcpkv::Parser parser;       // (for tcpkv) partial request state
    bool peer_closed;           // EOF seen; close after flushing
    bool want_write;            // EPOLLOUT registered
    bool reading_paused;        // EPOLLIN removed for backpressure
    Clock::time_point last_active;
};
```

This is exactly what the thread's stack held implicitly in module 05: the parser's partial state,
the unsent bytes, the deadline. Event loops force you to write it down, which is both the cost
(more code) and the benefit (you can inspect it, count it, and hold 100,000 of them in 100 MB).

### 2.6 eventfd and timerfd

An event loop blocked in `epoll_wait` can only be woken by an fd. Linux provides fds for things
that are not sockets:
- **`eventfd`**: a 64-bit counter. `write` adds, `read` returns and zeroes (or decrements with
  `EFD_SEMAPHORE`). Signal handlers (`write` is async-signal-safe) and other threads use it to wake
  the loop: "shutdown", "a worker finished job 42, pick up the result".
- **`timerfd`**: fires at an absolute or periodic time. Register it in epoll and you have timers
  without computing `epoll_wait` timeouts by hand. For many per-connection timeouts you keep a
  timer wheel or a min-heap of deadlines and arm one `timerfd` (or the `epoll_wait` timeout) for
  the earliest.
- **`signalfd`**: receive signals as fd events; an alternative to the eventfd-from-handler trick.
- **`pidfd`**, `inotify`, and friends: everything is an fd, so everything fits one loop.

### 2.7 Multi-threading an event loop

Options, from simple to complex:
1. **One loop, one thread** (this module's server, Redis before 6.0, Node.js). Scales to a core's
   worth of work. CPU work goes to a pool.
2. **One loop per thread, `SO_REUSEPORT`**: N independent loops each with its own listener on the
   same port; the kernel spreads connections. No sharing between loops (nginx workers, Envoy).
3. **One acceptor loop, N worker loops**: acceptor hands fds to workers round-robin via a queue plus
   `eventfd` (Netty, Boost.Asio with multiple `io_context`s, libuv does not do this natively).
4. **One epoll fd, N threads calling `epoll_wait`** with `EPOLLONESHOT` or `EPOLLEXCLUSIVE`: shared
   state must be locked; rarely worth it.

### 2.8 When event loops beat thread pools, and when they do not

Event loops win when connections are **many and mostly idle** (chat, push notifications, long-poll,
100,000 branch terminals with a request a minute): memory per connection is a few KB instead of a
thread, and no scheduler work for idle connections. They also win for **I/O-bound proxies** that
shuffle bytes between sockets.

Thread pools win when each request does **blocking or CPU-heavy work** (a database call through a
blocking driver, a loan calculation, decrypting a file) and connections are few hundred. The
blocking driver forces a thread anyway, and the code is simpler to read and debug (a stack trace
tells the whole story).

The combination is standard: an event loop for network I/O and framing, a thread pool for request
execution, `eventfd` to return results. That is how a modern version of your RPC server would look.

### 2.9 io_uring

epoll tells you *readiness*; you still do the `recv`/`send` syscalls. **io_uring** (Linux 5.1+)
submits operations (`recv into this buffer`, `accept`, `send`) through a shared ring in memory and
collects *completions* from another ring, often with zero syscalls per operation under load. That
is a **proactor**: you ask for the operation and are told when it is done. Benefits: fewer
syscalls, batching, true async file I/O (epoll never worked for regular files), zero-copy options.
Costs: newer, more complex API (use liburing), security hardening still evolving, kernel-version
sensitive. Know what it is and that it is the direction Linux is heading.

### 2.10 Windows IOCP: proactor from the start

Windows never had a good readiness API (`select` is limited, `WSAPoll` arrived late). Its native
model is **overlapped I/O with I/O completion ports**:

```
CreateIoCompletionPort()                         // the port
CreateIoCompletionPort(socket, port, key, 0)      // associate a socket
WSARecv(socket, &buf, ..., &overlapped, NULL)     // start a receive INTO YOUR BUFFER; returns at once
...
GetQueuedCompletionStatus(port, &bytes, &key, &overlapped, timeout)   // worker threads block here
// => "the receive you started into buffer X completed with N bytes"
```

Differences from epoll: you hand the kernel the buffer up front (so memory is pinned per pending
operation, which is why you keep only one or two receives outstanding per connection); the kernel
wakes one of N threads blocked on the port (built-in thread pool integration, with a concurrency
limit); you get completions, not readiness. `AcceptEx`, `ConnectEx`, `TransmitFile` are the
overlapped versions of accept/connect/sendfile. Your Windows RPC server could be IOCP-based or
thread-per-connection with blocking `recv`; find out which, because IOCP vs blocking changes every
answer about its scalability.

Reactor (epoll, kqueue, poll): "tell me when I can do it, I do it".
Proactor (IOCP, io_uring): "do it and tell me when it is done".

### 2.11 libuv and Boost.Asio

**libuv** (Node.js's loop, written in C): a reactor on Linux (epoll), kqueue on macOS, and IOCP on
Windows, exposed with one callback API (`uv_read_start(stream, alloc_cb, read_cb)`). Internally on
Windows it emulates readiness semantics on top of completions. One loop per thread; a thread pool
for blocking work (`uv_queue_work`); `uv_async_send` is its eventfd.

**Boost.Asio** (also standalone Asio, and the model behind C++ Networking TS): a *proactor*
interface everywhere. `socket.async_read_some(buffer, handler)` means "read into this buffer and
call the handler with bytes_transferred". On Linux it is implemented over epoll (reactor) by doing
the `recv` for you when the fd is readable; on Windows it maps directly onto IOCP; recent versions
can use io_uring. `io_context::run()` is the loop; `strand` serialises handlers for one connection
when several threads run the context; `steady_timer` replaces timerfd; `post`/`dispatch` replace
eventfd. If you write new C++ network code at work, Asio is the default choice; after this module
you know what it does underneath.

## 3. Code

### 3.1 The epoll echo server (`code/m06_epoll_echo_server.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m06_epoll_echo_server.cpp -o epoll_echo
./epoll_echo 7000 &
python3 m05_loadtest.py --echo --port 7000 -c 500 -n 200      # 500 connections, one thread
seq 1 100000 | ./echo_client 127.0.0.1 7000
ss -tanp | grep -c 7000                                        # count connections during the test
kill -INT %1                                                   # eventfd shutdown, prints counters
```

Accept until `EAGAIN` (edge-triggered rule):

```cpp
void on_accept() {
    for (;;) {
        int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;     // queue drained
            if (errno == EINTR || errno == ECONNABORTED) continue;
            /* EMFILE etc: log, return */
        }
        auto c = std::make_unique<Conn>(); c->fd = cfd; c->last_active = Clock::now();
        epoll_event ev{}; ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP; ev.data.ptr = c.get();
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
        conns.emplace(cfd, std::move(c));
    }
}
```

Read until `EAGAIN`, with backpressure:

```cpp
void on_readable(Conn& c) {
    char buf[64 * 1024];
    for (;;) {
        ssize_t n = recv(c.fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EINTR) continue;
                     if (errno == EAGAIN) break;              // drained: wait for the next edge
                     close_conn(c); return; }
        if (n == 0) { c.peer_closed = true; break; }          // EOF: flush, then close
        c.out.append(buf, n);
        if (c.out.size() >= kHighWater) { c.reading_paused = true; epoll_update(c); break; }
    }
    if (!flush(c)) { close_conn(c); return; }
    if (c.peer_closed && c.out.empty()) close_conn(c);
}
```

Flush, registering `EPOLLOUT` only while needed:

```cpp
bool flush(Conn& c) {
    while (!c.out.empty()) {
        ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue;
                     if (errno == EAGAIN) break;              // kernel buffer full: wait for EPOLLOUT
                     return false; }                          // EPIPE / ECONNRESET
        c.out.erase(0, n);
    }
    bool need_out = !c.out.empty();
    bool resume_read = c.reading_paused && c.out.size() < kHighWater / 2;
    if (need_out != c.want_write || resume_read) { c.want_write = need_out; if (resume_read) c.reading_paused = false; epoll_update(c); }
    return true;
}
```

The shutdown path: `SIGINT` handler writes 1 to an `eventfd`; the loop sees it readable and sets
`running = false`. The `timerfd` fires every second and closes connections idle for 60 s. Both are
just more fds in the same epoll.

The dangling-pointer guard in the loop: a `Conn` closed by the timer or by an earlier event in the
same `epoll_wait` batch may still appear later in that batch. Before using `data.ptr`, the loop
looks up `conns.find(fd)` and checks the pointer matches.

### 3.2 From echo to tcpkv on epoll

Replace `c.out.append(buf, n)` with: `c.parser.feed(buf, n); drain parser; c.out += replies`. The
`Store` from module 05 works as is (single thread: no lock needed). That is the whole change;
the parser's `feed`/`next` design was made for this.

## 4. Under the hood (senior-level)

**What epoll keeps.** `epoll_create` allocates an `eventpoll` with a red-black tree of registered
fds (`epitem`s) and a ready list. `EPOLL_CTL_ADD` inserts into the tree and hooks a callback into
the target socket's wait queue. When data arrives, the socket's wakeup callback (`ep_poll_callback`)
runs in softirq context and links the `epitem` onto the ready list, waking `epoll_wait` if it is
sleeping. `epoll_wait` copies ready items to user space; in LT mode it re-checks each item's
readiness and re-queues it if still ready; in ET mode it does not. That is the entire LT/ET
difference, and why ET is cheaper: no re-poll.

**Why ET needs the drain loop.** The callback fires when the socket state *changes* (new skb
arrives). If you leave data in the buffer and no new segment arrives, nothing fires. With LT,
`epoll_wait` itself re-polls the socket and re-reports.

**Readiness is a hint, not a promise.** A socket reported readable may return `EAGAIN` if another
thread read it first, or in rare kernel paths (checksum failure after readiness was signalled).
Always handle `EAGAIN` even after an event.

**Why `EPOLLOUT` is almost always true.** The send buffer is usually not full, so a registered
`EPOLLOUT` with LT fires on every `epoll_wait`: the loop spins. Register it only when a `send`
returned `EAGAIN`, and deregister when the buffer drains. The server does exactly this.

**Cost model.** `epoll_wait` returning N events costs one syscall plus O(N). With 100,000 idle
connections and 1,000 active, each loop iteration touches 1,000 items; `poll` would touch 100,000.
`epoll_ctl` is a syscall per modification; ET reduces modifications because interest rarely
changes. Measured on commodity hardware, a single epoll thread handles hundreds of thousands of
small messages per second; the ceiling is syscall cost (which io_uring attacks) and per-message
work, not epoll itself.

**Memory per connection.** A `Conn` with an empty `std::string` and a parser is a few hundred
bytes; the kernel's `tcp_sock` plus socket buffers (autotuned, starting at 16 to 128 KB when data
flows, nearly nothing when idle) dominate. 100,000 idle connections fit in a few GB mostly because
of kernel structures, which is why `tcp_rmem`/`tcp_wmem` minimums matter at that scale.

**Thundering herd and `EPOLLEXCLUSIVE`.** Several threads waiting on the same LT epoll fd for a
listener all wake on one connection; one wins `accept`, the rest get `EAGAIN`. `EPOLLEXCLUSIVE`
(Linux 4.5) wakes only one. `SO_REUSEPORT` sidesteps it with separate listeners.

**IOCP internals.** The completion port is a kernel queue; `GetQueuedCompletionStatus` threads
block on it in LIFO order (the most recently active thread is reused: warm cache) and the port
enforces a concurrency limit (threads actively running, default = CPUs); a thread that blocks lets
another run. The buffer you pass to `WSARecv` is locked in physical memory until completion, so
10,000 outstanding 64 KB receives pin 640 MB; the standard trick is a zero-byte receive to be told
"data is here" and then a normal `recv`, which turns IOCP back into a readiness model.

## 5. In production

- **The ET hang.** A server using `EPOLLET` read exactly one `recv` per event. Under load two
  requests arrived in one segment; the second sat in the kernel buffer with no new event; the client
  timed out. Reproduced only with pipelining or on a WAN. Rule: drain until `EAGAIN`.
- **100% CPU doing nothing.** `EPOLLOUT` registered permanently in LT mode. Every `epoll_wait`
  returned immediately with every connection writable. Register `EPOLLOUT` only with pending output.
- **One slow handler.** A handler did a blocking DNS lookup (`getaddrinfo`) for logging. Each
  lookup took 5 s when the DNS server was flaky; every connection on the loop froze for 5 s.
  Never block in a handler; offload to a thread or use an async resolver.
- **Memory explosion from one client.** A client sent 1 GB of requests and never read replies. The
  server buffered 1 GB of replies and was OOM-killed. High-water mark and pause reading: TCP flow
  control then stops the client at the kernel level.
- **Use-after-free in the event batch.** Timer closed an idle connection; a later event in the same
  batch pointed at the freed `Conn`. Crash once a week. Fix: look the fd up again before use, or
  defer destruction to the end of the loop iteration (a "to delete" list).
- **fd reuse within a batch.** A `Conn` is closed and `accept` returns the same fd number in the
  same iteration; a stale event for the old connection now matches the new one by fd. The pointer
  check (`it->second.get() != &c`) catches it; generation counters are the general solution.
- **Where this meets your work.** If the Windows server is blocking thread-per-connection, the
  IOCP version is the "how would you make it handle 50,000 branch terminals" answer. If it already
  uses IOCP, map its completion handler and per-connection context to this module's `Conn` and
  handlers; you will find the same high-water-mark and dangling-context problems.

## 6. Try it

1. **(easy)** Run the epoll echo server with the load tester at `-c 1000`. Compare RSS
   (`ps -o rss`) and thread count with the thread-pool server at the same connection count.
   Done when: you can state the per-connection memory of each design from your measurements.

2. **(easy)** Break the ET rule: change `on_readable` to do exactly one `recv`. Run the load test
   with `--pipeline 16`. Done when: you observe hangs and can explain them from the kernel's point
   of view. Restore the loop.

3. **(medium)** Convert the echo server to tcpkv: add a `Parser` per `Conn`, the `Store`, and reply
   formatting. Run `m05_loadtest.py` against it. Done when: the load test passes at `-c 500`.

4. **(medium)** Add a non-blocking `connect` to the tcpkv client side: start the connect, register
   `EPOLLOUT`, read `SO_ERROR` on the event, then proceed. Test against a black-hole address with a
   3 s timer. Done when: it fails cleanly at 3 s instead of hanging.

5. **(hard)** Add a thread pool for "slow" commands: a fake `SLOW <ms>` command that sleeps in a
   worker and returns `+OK`. The worker must post the reply back to the loop via an `eventfd` and a
   mutex-protected queue, and the reply must be written on the loop thread. Done when: 100 `SLOW
   1000` requests on 100 connections complete in about 1 s total with 16 workers, while `PING` on
   another connection stays instant.

6. **(hard)** Rewrite the echo server with Boost.Asio (`sudo apt install libboost-dev`), using
   `async_read_some` / `async_write` and a single `io_context`. Compare lines of code and
   performance. Done when: both pass the load test and you can explain what Asio did for you.

## 7. Interview questions

**Q: What is the difference between blocking and non-blocking I/O?**
A: A blocking `recv` sleeps until data arrives. A non-blocking one returns `EAGAIN` immediately if
nothing is buffered, so a single thread can service many sockets by asking a readiness API
(epoll) which ones have data, and never sleeping on one socket.

**Q: Why is epoll faster than poll?**
A: `poll` passes the whole fd list on every call and the kernel scans all of it: O(n) per call.
epoll keeps the interest list in the kernel and callbacks put ready fds on a list, so `epoll_wait`
is O(ready). With 10,000 connections and 10 active, that is a 1000x difference per iteration.

**Q: Level-triggered versus edge-triggered?**
A: LT reports an fd every time it is ready; ET reports only when it becomes ready. ET requires
draining until `EAGAIN` after each event or you miss data sitting in the buffer. ET has fewer
wakeups and suits multi-threaded consumers; LT is forgiving.

**Q: What state do you need per connection in an event loop?**
A: Everything a thread would have kept on its stack: the partial parse state, the output bytes not
yet accepted by the kernel, whether the peer half-closed, which epoll interests are registered,
and the last-activity time for timeouts.

**Q: How do you handle a client that sends but never reads?**
A: The output buffer grows. At a high-water mark, stop reading (remove `EPOLLIN`); the receive
buffer fills, TCP advertises a zero window and the client's `send` blocks. Resume at a low-water
mark. Also apply a hard limit and close.

**Q: Reactor versus proactor?**
A: Reactor (epoll, kqueue): the loop is told a socket is ready and performs the read or write
itself. Proactor (IOCP, io_uring, Asio's interface): the application submits an operation with a
buffer and is told when it completed. Asio presents a proactor interface over epoll by doing the
read for you on readiness.

**Q: How do you wake an event loop from another thread or a signal handler?**
A: Register an `eventfd` (or a pipe) in epoll; `write` to it from the other context (`write` is
async-signal-safe). The loop sees it readable, reads the counter, and processes whatever queue the
writer filled. `timerfd` does the same for time.

**Q: When would you not use an event loop?**
A: When each request does blocking or CPU-heavy work through libraries you cannot make
asynchronous (blocking DB drivers, heavy computation) and connections number in the hundreds. A
thread pool is simpler, stack traces are readable, and the blocking library forces threads anyway.
The hybrid (loop for I/O, pool for work) is the common production answer.

## 8. Recap

- Non-blocking sockets return `EAGAIN` instead of sleeping; a readiness API says when to retry.
- `select` is capped and O(n); `poll` is O(n); `epoll` is O(ready) with a kernel-side interest list.
- LT: reported while ready. ET: reported on change; drain to `EAGAIN` or hang. `EPOLLONESHOT` for
  multi-thread hand-off.
- Reactor: never block in a handler; per-connection state object; output buffer plus `EPOLLOUT`
  only while pending; backpressure via pausing reads; beware dangling `Conn` within a batch.
- `eventfd` wakes the loop from threads and signal handlers; `timerfd` gives timers as fds.
- Event loops win for many idle connections and byte shuffling; thread pools win for blocking or
  CPU-heavy per-request work; hybrids are the norm.
- io_uring and IOCP are proactors (submit operation, get completion); epoll is a reactor.
- libuv wraps epoll/kqueue/IOCP as a reactor API; Asio exposes a proactor API over all of them.
