# 03. The sockets API and a blocking server

**Time:** ~3 hours   **Prerequisites:** 01, 02

## 1. In plain words

A **socket** is a file descriptor that the kernel connects to a network conversation instead of a
file on disk. You get one with `socket()`, you tell the kernel which address you want with
`bind()`, you say "I am a server, queue up callers" with `listen()`, and every `accept()` hands
you a brand-new socket for one client. On the client side you skip bind and listen and just
`connect()`. After that both sides `read()` and `write()` (or `recv()` and `send()`) the same way
they would on a pipe, and `close()` when done.

The analogy: a company switchboard. `bind` picks the phone number, `listen` turns the switchboard
on, `accept` answers a ringing line and hands it to an operator, and the operator talks on that
line until one side hangs up. The switchboard number is still free to ring again.

The hard part is not the calls themselves. The hard part is that each call can fail in many ways,
`read` and `write` may do less than you asked, and a signal can interrupt any of them. A senior
engineer's socket code is 20% happy path and 80% handling exactly these cases. This module gives
you a small reusable header that handles them once, so the rest of the course never repeats them.

## 2. The real thing

### 2.1 The call sequence

```
Server                                   Client
socket()                                 socket()
setsockopt(SO_REUSEADDR)
bind(addr, port)
listen(backlog)
accept()  --- blocks until ---           connect(addr, port)   (handshake happens here)
  returns new fd
read()/write() on new fd  <----------->  write()/read()
close(new fd)                            close()
(loop back to accept)
```

### 2.2 `getaddrinfo` instead of hand-built `sockaddr_in`

Old code fills a `sockaddr_in` with `inet_addr("10.1.2.3")`. Do not. `getaddrinfo` does name
resolution (DNS, `/etc/hosts`), handles IPv4 and IPv6 uniformly, parses service names or port
numbers, and returns a linked list of candidate addresses you try in order.

```cpp
addrinfo hints{};
hints.ai_family   = AF_UNSPEC;      // v4 or v6
hints.ai_socktype = SOCK_STREAM;    // TCP
hints.ai_flags    = AI_PASSIVE;     // for a server: nullptr host means INADDR_ANY / in6addr_any
addrinfo* res = nullptr;
int rc = getaddrinfo(nullptr, "7000", &hints, &res);   // rc != 0: use gai_strerror(rc), NOT errno
for (addrinfo* ai = res; ai; ai = ai->ai_next) { /* socket, bind, listen; break on success */ }
freeaddrinfo(res);
```

`getaddrinfo` is a blocking call that can take seconds if DNS is slow. In an event loop (module 06)
you call it from a worker thread or use `getaddrinfo_a`/c-ares.

### 2.3 `SO_REUSEADDR` and friends

When your server restarts, old connections to its port are still in TIME_WAIT for 60 s. Without
`SO_REUSEADDR`, `bind()` fails with `EADDRINUSE`. Set it on every listening socket before `bind`.
It does **not** let two processes listen on the same port (that is `SO_REUSEPORT`, used for
multi-process accept load balancing). On Windows `SO_REUSEADDR` has a different, dangerous meaning
(it allows hijacking a port already in use); use `SO_EXCLUSIVEADDRUSE` there and accept the
TIME_WAIT bind failure, or set `SO_REUSEADDR` only on Linux.

### 2.4 Read and write: the calls that lie to beginners

`ssize_t n = read(fd, buf, len)` (or `recv(fd, buf, len, flags)`) returns:

- `n > 0`: that many bytes. **Any number from 1 to `len`.** Not "the message". Just bytes.
- `n == 0`: EOF. The peer sent FIN. It will never send more. You should finish and close.
- `n < 0`: error, look at `errno`.

`ssize_t n = write(fd, buf, len)` (or `send`) returns:

- `n == len`: all written (into the kernel send buffer; not yet on the wire).
- `0 < n < len`: partial write. Happens on non-blocking sockets when the send buffer fills, and on
  blocking sockets when a signal arrives after some bytes were copied or `SO_SNDTIMEO` fires.
  You must send the rest.
- `n < 0`: error.

Therefore every program has these two loops, and the helper header has them once:

```cpp
// Read exactly len bytes or report EOF/error.
while (got < len) {
    ssize_t n = recv(fd, p + got, len - got, 0);
    if (n < 0) { if (errno == EINTR) continue; return error; }
    if (n == 0) return eof;              // peer closed; if got > 0 this is a truncated message
    got += n;
}
// Write all len bytes.
while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) { if (errno == EINTR) continue; return error; }
    sent += n;
}
```

"Read exactly N" only works when you know N: a fixed header, or a length prefix (module 04). For
delimited text you instead read into a growing buffer and scan for the delimiter.

### 2.5 errno values you must recognise

| errno | Where | Meaning | What to do |
|-------|-------|---------|------------|
| `EINTR` | any blocking call | A signal arrived before the call completed | Retry the call. Always. |
| `EAGAIN` / `EWOULDBLOCK` | read/write/accept | Non-blocking socket has nothing to read (or no room to write); or `SO_RCVTIMEO` expired on a blocking socket | Non-blocking: wait for readiness (poll/epoll). Timeout: treat as timeout. |
| `ECONNRESET` | read/write | Peer sent RST: crashed, closed with unread data, or a middlebox dropped state | Close the connection. Client: reconnect and retry if idempotent. |
| `EPIPE` | write | We wrote to a connection the peer already closed/reset. Also raises SIGPIPE unless `MSG_NOSIGNAL` or SIGPIPE ignored | Close. Ignore SIGPIPE process-wide in servers. |
| `ECONNREFUSED` | connect | Nothing listens on that port (server got RST) | Server down or wrong port. |
| `ETIMEDOUT` | connect/read/write | Retransmissions exhausted; host unreachable or black-holed | Close. Peer probably dead or firewalled. |
| `EADDRINUSE` | bind | Port in use or TIME_WAIT without `SO_REUSEADDR` | Set `SO_REUSEADDR`; check `ss -tlnp`. |
| `EADDRNOTAVAIL` | connect | Ephemeral ports exhausted (client) | Pool connections; `tcp_tw_reuse`. |
| `EMFILE` | socket/accept | Process fd limit hit (`ulimit -n`) | Fix the leak (CLOSE_WAIT!) or raise limit. Back off on accept. |
| `ENFILE` | socket/accept | System-wide fd limit | Rare; `fs.file-max`. |
| `ECONNABORTED` | accept | Client reset while in the accept queue | Ignore and `accept` again. |
| `EHOSTUNREACH` / `ENETUNREACH` | connect/write | No route | Network or routing problem. |

**SIGPIPE** deserves its own line. Default action: terminate the process. A server that writes a
response to a client who already disconnected (very common) dies silently. Every server does
`signal(SIGPIPE, SIG_IGN)` at startup and/or uses `send(..., MSG_NOSIGNAL)`. Windows has no SIGPIPE.

### 2.6 Blocking semantics

By default sockets are **blocking**: `accept` sleeps until a connection arrives, `read` sleeps until
at least one byte arrives (or EOF or error), `write` sleeps until all bytes fit in the send buffer,
`connect` sleeps until the handshake completes or fails (up to about 2 minutes for a black hole;
set `SO_SNDTIMEO` or use non-blocking connect with `poll` to bound it).

A blocking server therefore needs one thread per thing it waits on. This module's server waits on
one client at a time; module 05 adds threads; module 06 replaces blocking with readiness
notification.

### 2.7 Shutdown vs close

`close(fd)`: drops this reference to the socket. When the last reference is gone (remember `fork`
and `dup` copy references), the kernel sends FIN after flushing the send buffer, and the fd number
becomes reusable immediately (a later `accept` may return the same number; this is why storing raw
fds in long-lived structures is dangerous).

`shutdown(fd, SHUT_WR)`: send FIN now, keep the fd, keep reading. Lets a client say "request done"
and read until the server's EOF. `shutdown(fd, SHUT_RD)` discards further input. `shutdown` affects
the connection regardless of how many fd references exist, which is how a thread can wake another
thread blocked in `read` on the same socket: `shutdown(SHUT_RDWR)` makes the blocked `read` return 0.
This is a key graceful-shutdown tool in module 05.

### 2.8 Winsock differences

| POSIX (Linux) | Winsock (Windows) | Note |
|---------------|-------------------|------|
| nothing | `WSAStartup(MAKEWORD(2,2), &wsa)` once per process, `WSACleanup()` at exit | Forgetting it makes every call fail with `WSANOTINITIALISED` |
| `int fd` | `SOCKET s` (unsigned, `UINT_PTR`) | Invalid is `INVALID_SOCKET`, not -1. Never compare to `< 0`. |
| `close(fd)` | `closesocket(s)` | `close` on a SOCKET is undefined |
| `errno` | `WSAGetLastError()` | Values are `WSAE*`: `WSAEWOULDBLOCK`, `WSAECONNRESET`, `WSAEINTR` |
| `read`/`write` | not for sockets; use `recv`/`send` | `ReadFile` works on sockets but mixing is unwise |
| `ssize_t` return | `int` return, `SOCKET_ERROR` (-1) on error | |
| `fcntl(O_NONBLOCK)` | `ioctlsocket(s, FIONBIO, &one)` | |
| `poll` | `WSAPoll` (Vista+; had bugs with connect errors before Windows 10) | `select` exists but `fd_set` is an array, not a bitmap, limited by `FD_SETSIZE` 64 |
| `epoll` | none; use IOCP (`CreateIoCompletionPort`, `WSARecv` overlapped) or `WSAEventSelect` | Module 06 |
| `SIGPIPE` | none | `send` returns `WSAECONNRESET`/`WSAECONNABORTED` |
| `MSG_NOSIGNAL` | not needed, does not exist | |
| `SO_REUSEADDR` prevents TIME_WAIT bind failure | `SO_REUSEADDR` allows port hijacking; use `SO_EXCLUSIVEADDRUSE` | Windows does not block bind on TIME_WAIT by default |
| `TCP_KEEPIDLE`/`TCP_KEEPINTVL` | `WSAIoctl(SIO_KEEPALIVE_VALS)` (ms units) or the same option names on Windows 10 1709+ | |
| `getaddrinfo`/`freeaddrinfo` | same names, `<ws2tcpip.h>` | |
| `inet_pton`/`inet_ntop` | same (Vista+) | |
| link | nothing | `ws2_32.lib` |
| `EINTR` from signals | essentially never happens | Winsock blocking calls are not interrupted by signals |
| `fork` + inherit socket | `WSADuplicateSocket` to pass to another process | |

A portability layer is a dozen macros and an init function. Your employer's Windows server has
one; find it and read it with this table beside you.

## 3. Code

### 3.1 The RAII wrapper (`code/m03_socket.hpp`)

The core type:

```cpp
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }                                 // closes on every exit path: no CLOSE_WAIT leaks
    Fd(const Fd&) = delete;                            // two owners would double-close
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }   // move = transfer ownership
    Fd& operator=(Fd&& o) noexcept { if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = -1; } return *this; }
    int  get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int  release() noexcept { int f = fd_; fd_ = -1; return f; }
    void reset(int fd = -1) noexcept { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
private:
    int fd_ = -1;
};
```

Why this matters: the CLOSE_WAIT leak in module 02 cannot happen. Whether the handler returns,
throws, or breaks out of a loop, the destructor runs. This is the single biggest robustness gain
you get from C++ over C in network code. (In your C server, the equivalent discipline is a single
`cleanup:` label that every error path `goto`s.)

The I/O loops:

```cpp
inline size_t read_some(int fd, void* buf, size_t len) {
    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) return static_cast<size_t>(n);       // 0 = EOF, caller decides
        if (errno == EINTR) continue;                     // signal: not an error, retry
        throw_errno("recv");                              // ECONNRESET, EAGAIN(timeout), ...
    }
}

inline bool read_exact(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        size_t n = read_some(fd, p + got, len - got);
        if (n == 0) {
            if (got == 0) return false;                  // clean EOF between messages
            throw std::runtime_error("connection closed mid-message");  // truncated: protocol error
        }
        got += n;
    }
    return true;
}

inline void write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);   // EPIPE instead of SIGPIPE
        if (n < 0) { if (errno == EINTR) continue; throw_errno("send"); }
        sent += static_cast<size_t>(n);
    }
}
```

Note the distinction in `read_exact`: EOF before the first byte is normal (the client finished and
closed between requests); EOF in the middle of a message is a protocol violation and is reported
differently. Protocol code needs that distinction.

`listen_tcp` and `connect_tcp` wrap `getaddrinfo`, try each address, set `SO_REUSEADDR` on
listeners, and retry `connect` on `EINTR`. `accept_tcp` retries `EINTR` and `ECONNABORTED`, and
returns an empty `Fd` when the listener has been closed (that is how module 05 stops the accept loop).

### 3.2 Echo server (`code/m03_echo_server.cpp`)

```cpp
static void serve_one(net::Fd client) {
    net::set_nodelay(client.get());
    char buf[4096];
    try {
        for (;;) {
            size_t n = net::read_some(client.get(), buf, sizeof buf);
            if (n == 0) break;                        // peer FIN: normal end
            net::write_all(client.get(), buf, n);     // echo whatever arrived, however much
        }
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "[!] %s\n", e.what()); // ECONNRESET/EPIPE: client is gone, carry on
    }
}   // client destroyed => close()

int main(int argc, char** argv) {
    net::ignore_sigpipe();
    net::Fd listener = net::listen_tcp(nullptr, argc > 1 ? argv[1] : "7000", 128);
    for (;;) {
        net::Fd client = net::accept_tcp(listener.get());
        if (!client) break;
        serve_one(std::move(client));
    }
}
```

An echo server does not need framing because it does not interpret the bytes. The moment you
interpret bytes (module 04), you need framing.

### 3.3 Echo client (`code/m03_echo_client.cpp`)

The client sends each stdin line and uses `read_exact(line.size())` to get the echo back. It sets
`SO_RCVTIMEO` to 5 s so a hung server cannot hang it, and does `shutdown(SHUT_WR)` at the end so
the server sees EOF and the client holds TIME_WAIT.

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_echo_server.cpp -o echo_server
g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_echo_client.cpp -o echo_client
./echo_server 7000 &
seq 1 200000 | ./echo_client 127.0.0.1 7000      # ok: 200000 lines, 1488895 bytes echoed correctly
```

Run `strace -e trace=network,read,write -p $(pidof echo_server)` while the bulk test runs. You
will see `recvfrom` return many different sizes. That is section 2.4 in real life.

## 4. Under the hood (senior-level)

**What `socket()` allocates.** A `struct socket` plus a protocol-specific `struct sock` (for TCP,
`struct tcp_sock`, over 2 KB), an fd table entry, and a `struct file`. The fd is an index into the
process's fd table; the kernel object is reference counted. `dup`/`fork` add references; `close`
drops one. The TCP connection ends when the last reference closes, which is a classic bug after
`fork` in servers that forget to close the client socket in the parent.

**What `bind()` checks.** Port availability per protocol and address family, privilege for ports
below 1024 (or `CAP_NET_BIND_SERVICE`), and the `SO_REUSEADDR` rules: can bind if all existing
sockets on the port also have `SO_REUSEADDR` and none is in LISTEN (Linux), or if the existing
ones are only in TIME_WAIT.

**What `listen()` does.** Transitions the socket to LISTEN state and allocates the SYN queue and
accept queue. `backlog` (clamped by `somaxconn`) sizes the accept queue. Half-open connections live
in the SYN queue (or as SYN cookies under pressure).

**What `accept()` does.** Dequeues a fully established connection from the accept queue, allocates
a new fd pointing at the already-created `struct sock`. The handshake happened before, in softirq
context, when the ACK arrived. If the queue is empty and the socket is blocking, the thread sleeps
on the socket's wait queue and is woken by the softirq that enqueues the next connection.

**What `recv()` does.** Takes the socket lock, walks the receive queue of `sk_buff`s, copies up to
`len` bytes to user space (`skb_copy_datagram_iter`), frees consumed skbs, updates the receive
window (may send a window update ACK), releases the lock. If the queue is empty and the socket is
blocking it sleeps with a timeout equal to `SO_RCVTIMEO` (infinite by default). That is why
`recv` returns whatever is queued: it does not wait for `len` bytes, only for at least one.

**What `send()` does.** Copies user bytes into skbs appended to the write queue up to the
send-buffer limit, then calls `tcp_push` which may transmit immediately (if cwnd/rwnd allow and
Nagle allows) or leave the data for the ACK clock to send later. If the send buffer is full the
caller sleeps (`SO_SNDTIMEO` bounds it). A blocking `send` returning a short count happens when a
signal arrives during that sleep after some bytes were already copied.

**`close()` and lingering.** Default: `close` returns at once, the kernel keeps the `sock` alive
("orphan") until the send queue drains and FIN is ACKed, then TIME_WAIT. Orphans count against
`tcp_max_orphans`. If the receive queue still has unread data when you `close`, Linux sends RST
instead of FIN: the peer sees `ECONNRESET`. This surprises people who close a socket after a
protocol error without draining it.

**Thread safety.** The kernel socket is thread-safe per call: two threads can `send` concurrently
and bytes will not interleave within one call, but two `send` calls from two threads can interleave
with each other at the message level, corrupting a framed protocol. One writer per socket, or a
mutex around whole-message writes (your RPC server likely has exactly this mutex per connection).

**Windows: the same shape underneath.** Winsock calls go through `ws2_32.dll`, `mswsock.dll`, into
the AFD driver (`afd.sys`) and `tcpip.sys`. `recv` is a blocking IRP; overlapped I/O (module 06)
lets the kernel complete into your buffer asynchronously. `closesocket` with pending overlapped
operations cancels them with `WSA_OPERATION_ABORTED`.

## 5. In production

- **Partial read bug, classic form.** Code that does `recv(fd, &header, sizeof header, 0)` once
  and then uses `header.length` works for months on a LAN. Then a WAN link or a large payload
  delivers 3 bytes of the 8-byte header, the code reads garbage length (say 2 GB), tries to
  allocate, and crashes. Every `recv` that expects a fixed size must be a loop. If your Windows
  server has a single `recv` for the header, that is a latent crash; check it this week.
- **Partial write bug, classic form.** Non-blocking socket, `send` returns 1200 of 4096 bytes,
  code ignores the return value. The peer receives a truncated message and the next message's bytes
  look like garbage. Symptom: "random protocol errors under load". The fix is `write_all` or a
  per-connection output buffer (module 06).
- **SIGPIPE at 3 AM.** Server process disappears with no log line and exit status 141 (128 + 13).
  A client disconnected while a response was being written. Ignore SIGPIPE on day one.
- **EINTR after adding a profiler or a signal-based timer.** Code that worked for years starts
  failing with "Interrupted system call" after someone added `SIGALRM`-based timeouts or `SIGPROF`
  profiling. Every blocking call needs the EINTR retry. (`SA_RESTART` helps for some calls but not
  for `accept`/`recv` with timeouts, and not for `poll`.)
- **EMFILE and the accept storm.** When the fd limit is hit, `accept` fails with `EMFILE`, the
  listener is still readable, and a naive loop spins at 100% CPU logging errors. Mitigation: on
  `EMFILE`, sleep briefly, or keep a spare fd to close-and-accept-and-close to drain the queue
  politely. The real fix is the fd leak or `ulimit -n`.
- **Closing with unread data sends RST.** A server that detects a protocol error, logs, and
  closes while the client is still sending sees the client log `ECONNRESET`, and the client team
  blames the network. Drain or `shutdown(SHUT_WR)` then read to EOF before `close` when you want a
  clean FIN.
- **Your banking server.** The pattern there is the same: a listener thread, `accept`, hand the
  socket to a worker. Audit the worker's header read, the write path, the SIGPIPE equivalent (not
  applicable on Windows), and whether every error path reaches `closesocket`. Those four checks are
  the module in one sentence.

## 6. Try it

1. **(easy)** Build and run the echo server and client. Run `ss -tanp | grep 7000` during a bulk
   test and after the client exits. Done when: you can name every state you see and which side
   owns it.

2. **(easy)** Remove `ignore_sigpipe()` and `MSG_NOSIGNAL` from a copy of the server. Connect with
   `nc`, kill `nc` with `kill -9` while the server is mid-echo of a large input
   (`yes | head -c 50000000 | nc ...`). Done when: you see the server die with status 141, and you
   can explain it.

3. **(medium)** Make the server print how many bytes each `recv` returned for one client session.
   Send `seq 1 100000` through the client. Done when: you have seen at least three distinct sizes
   and can explain why they vary.

4. **(medium)** Write `connect_with_timeout(host, port, ms)` using a non-blocking `connect`,
   `poll` for `POLLOUT`, then `getsockopt(SO_ERROR)` to read the result, then switch back to
   blocking. Test against a black-hole address (`10.255.255.1`). Done when: it fails within your
   timeout instead of 2 minutes.

5. **(hard)** Write the Winsock version of `m03_socket.hpp` as `m03_socket_win.hpp` (you can
   compile it with MSVC at work): `WSAStartup` in a static initialiser, `SOCKET` type, `closesocket`,
   `WSAGetLastError` mapped to a small error enum, `ioctlsocket` for non-blocking. Done when: the
   echo client compiles against both headers with `#ifdef _WIN32` only in the header.

6. **(hard)** Implement a tiny `strace`-free "recv size histogram": an `LD_PRELOAD` shared library
   that wraps `recv` and `read`, counts return sizes in buckets, and prints the histogram at exit.
   Use it on the echo server. Done when: it compiles and shows a distribution.

## 7. Interview questions

**Q: What does `recv` returning 0 mean, and what should you do?**
A: The peer closed its sending side (sent FIN). It is EOF, not an error. Finish any response you
owe, then close your socket; otherwise you sit in CLOSE_WAIT indefinitely. Never retry `recv` after 0.

**Q: Why must you loop on `recv` and `send`?**
A: TCP is a byte stream. `recv` returns whatever is in the receive buffer, 1 byte to `len`. `send`
may copy only part of your buffer when the send buffer is full (non-blocking) or a signal or timeout
interrupts (blocking). The loop is the only correct way to read N bytes or write N bytes.

**Q: What is `EINTR` and how do you handle it?**
A: A signal handler ran while the thread was blocked in a system call; the call returns -1 with
`EINTR` instead of completing. It is not a failure. Retry the call. `SA_RESTART` reduces but does
not eliminate it.

**Q: What is SIGPIPE?**
A: The signal raised when you write to a socket (or pipe) whose peer has closed. Default action
kills the process. Servers must `signal(SIGPIPE, SIG_IGN)` or use `MSG_NOSIGNAL` and handle `EPIPE`.

**Q: What does `SO_REUSEADDR` do, and is it the same on Windows?**
A: On Linux it lets `bind` succeed while old connections on the port are in TIME_WAIT, so a server
can restart immediately. It does not allow two listeners. On Windows it allows binding to a port
that another socket is actively using, which is a security problem; use `SO_EXCLUSIVEADDRUSE` there.

**Q: `close` versus `shutdown`?**
A: `close` releases the fd and, when the last reference is gone, sends FIN after flushing. `shutdown`
acts on the connection regardless of references: `SHUT_WR` sends FIN but keeps the fd open for
reading (half-close); `SHUT_RDWR` also wakes other threads blocked in `recv` on that socket, which
is useful for graceful shutdown.

**Q: Why use `getaddrinfo` instead of filling `sockaddr_in`?**
A: It does name resolution, supports IPv4 and IPv6 without separate code paths, returns multiple
addresses to try in order, and parses ports and service names. Hand-built `sockaddr_in` ties you to
IPv4 and literal IPs.

**Q: How does an RAII fd wrapper prevent a production incident?**
A: A leaked socket leaves the connection in CLOSE_WAIT forever and eventually exhausts the fd limit
so `accept` fails with `EMFILE`. With a move-only wrapper whose destructor closes, every return,
break and exception path closes the socket, so the leak class disappears.

## 8. Recap

- Server: socket, `SO_REUSEADDR`, bind, listen, accept loop. Client: socket, connect. Both:
  recv/send loops, close.
- Use `getaddrinfo`; try every returned address; `gai_strerror` for its errors.
- `recv` returns 1..len bytes, 0 on EOF, -1 with errno. `send` may be short. Loop, always.
- EINTR: retry. EAGAIN: wait or timeout. ECONNRESET/EPIPE: peer gone, close. EMFILE: you leaked.
- Ignore SIGPIPE in every server. Use `MSG_NOSIGNAL` too.
- `shutdown(SHUT_WR)` for half-close; `shutdown(SHUT_RDWR)` to wake a blocked reader.
- Closing with unread data sends RST, not FIN.
- One writer per socket at a time, or messages interleave.
- RAII `Fd` wrapper: move-only, closes in destructor, kills the CLOSE_WAIT leak class.
- Winsock: `WSAStartup`, `SOCKET`/`INVALID_SOCKET`, `closesocket`, `WSAGetLastError`, no SIGPIPE,
  `SO_EXCLUSIVEADDRUSE`, IOCP instead of epoll.
