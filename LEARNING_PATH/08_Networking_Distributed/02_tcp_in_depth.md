# 02. TCP in depth

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

TCP turns an unreliable network (packets get lost, duplicated, reordered) into something that looks
like a reliable pipe: bytes come out the other end in the order you put them in, with nothing
missing. It does this by numbering every byte, having the receiver acknowledge what arrived,
re-sending what was not acknowledged, and by slowing down when the receiver or the network is full.

Think of two people dictating a long document over a bad phone line. The reader numbers each
sentence. The listener repeats back "got up to sentence 40". If the reader hears nothing for a
while, they repeat from 41. If the listener says "slow down, I can only write 5 sentences at a
time", the reader waits. If the line crackles (packet loss), the reader assumes the line is
congested and speaks slower for a while. That is sequence numbers, ACKs, retransmission, flow
control and congestion control.

One thing TCP does **not** do: it does not know where your messages begin and end. You dictate
three sentences; the listener may hear them as one long run or as five fragments. TCP guarantees
the letters arrive in order, not that your sentence boundaries survive. This single fact causes
more networking bugs than everything else combined, and module 04 is about fixing it.

## 2. The real thing

### 2.1 The segment header

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-------------------------------+-------------------------------+
|          Source Port          |       Destination Port        |
+-------------------------------+-------------------------------+
|                        Sequence Number                        |
+---------------------------------------------------------------+
|                    Acknowledgment Number                      |
+-------+-------+-+-+-+-+-+-+-+-+-------------------------------+
| Data  | Rsvd  |C|E|U|A|P|R|S|F|            Window             |
| Offset|       |W|C|R|C|S|S|Y|I|                               |
|       |       |R|E|G|K|H|T|N|N|                               |
+-------+-------+-+-+-+-+-+-+-+-+-------------------------------+
|           Checksum            |         Urgent Pointer        |
+-------------------------------+-------------------------------+
|                    Options (MSS, SACK, timestamps, wscale)    |
+---------------------------------------------------------------+
```

Flags you need: **SYN** (start, synchronise sequence numbers), **ACK** (acknowledgment number is
valid), **FIN** (I have no more data to send), **RST** (abort; something is wrong), **PSH** (push
to application now; mostly advisory).

### 2.2 Three-way handshake

```
Client                                  Server (LISTEN)
  |  SYN, seq=x, MSS, wscale, SACK ok  -->  |
  |  <--  SYN+ACK, seq=y, ack=x+1           |   server: SYN-RECEIVED
  |  ACK, ack=y+1  -->                      |   both: ESTABLISHED
  |  (data may follow immediately)          |
```

Both sides pick a random **initial sequence number** (ISN). Randomness prevents an attacker from
guessing sequence numbers and injecting data, and prevents confusion with segments from an earlier
connection on the same 4-tuple. The handshake also negotiates options: MSS, window scale factor
(so the 16-bit window can describe up to 1 GB), selective acknowledgment (SACK) support, and
timestamps (used for round-trip measurement and protecting against wrapped sequence numbers).

Cost: one full round trip before the first byte of data can be sent. On a 100 ms link that is
100 ms of nothing. Hence connection pooling (module 09). TCP Fast Open can send data in the SYN but
is rarely used in practice because middleboxes break it.

### 2.3 Sequence numbers and ACKs

Every byte in each direction has a 32-bit sequence number. A segment's `seq` is the number of its
first byte. The receiver's `ack` is the number of the next byte it expects, which means
"I have everything before this". ACKs are **cumulative**: `ack=1001` means bytes up to 1000
arrived. With **SACK** the receiver can additionally say "and I also have 1500 to 2000", so the
sender retransmits only the hole 1001 to 1499.

ACKs ride on data segments going the other way when possible. If there is no data to send, the
receiver sends a pure ACK, usually **delayed** by up to 40 ms (Linux) hoping to piggyback on data
or to ACK two segments at once. The delayed-ACK timer interacts badly with Nagle (section 2.7).

### 2.4 Retransmission

The sender keeps every unacknowledged byte and a timer. Two retransmission triggers:

- **RTO** (retransmission timeout): no ACK within RTO. RTO is computed from the smoothed
  round-trip time and its variance (RFC 6298); minimum 200 ms on Linux, initial 1 s. On each
  timeout the RTO doubles (exponential backoff), up to the `tcp_retries2` limit (default 15
  retries, which adds up to roughly 13 to 30 minutes before the connection is declared dead). This
  is why a connection to a crashed host can hang for 15 minutes if you do not set your own timeouts.
- **Fast retransmit**: three duplicate ACKs for the same number mean one segment was lost but later
  ones arrived. Resend it immediately without waiting for RTO.

A **duplicate** segment (retransmitted after the original was merely delayed) is detected by sequence
number and discarded. This is why your application never sees duplicate bytes within one connection.
It says nothing about duplicate *requests* across reconnects (module 07, idempotency).

### 2.5 Flow control: the receive window

The receiver advertises a **window**: how many more bytes it can buffer. The sender may have at most
`window` unacknowledged bytes in flight. When your application stops calling `read`, the receive
buffer fills, the window shrinks to zero, and the sender stops. It then sends periodic **window
probes** until the window opens. This is backpressure built into TCP. A slow consumer slows the
producer automatically; nothing is dropped. It is also why a blocked `write` on the server can be
caused by a client that stopped reading (module 05, deadlock story).

**Bandwidth-delay product**: to keep a link full you need `bandwidth * RTT` bytes in flight. 1 Gbit/s
at 50 ms needs about 6 MB. The original 16-bit window maxes at 64 KB, which at 50 ms RTT caps
throughput at about 10 Mbit/s. Window scaling fixes this, and Linux autotunes buffer sizes up to
`tcp_rmem` max (default 6 MB). If you set `SO_RCVBUF` yourself you disable autotuning for that
socket, so do not do it without a reason.

### 2.6 Congestion control: the network's window

Flow control protects the receiver. **Congestion control** protects the network. The sender keeps a
second limit, the **congestion window (cwnd)**, and sends at most `min(rwnd, cwnd)`.

- **Slow start**: cwnd starts at 10 segments (Linux default, about 14 KB) and doubles every RTT
  (each ACK increases cwnd by one segment). It is "slow" only compared to blasting at line rate.
- **Congestion avoidance**: after reaching a threshold, grow by one segment per RTT (linear).
- **Loss as a signal**: a timeout drops cwnd to 1 and restarts slow start; three duplicate ACKs
  halve it (classic Reno). Modern Linux uses **CUBIC** by default (cubic growth function, better
  for high-bandwidth links) and offers **BBR** (models bandwidth and RTT instead of waiting for loss).

Practical consequence: a fresh connection sends only about 14 KB in the first round trip. A 100 KB
response takes four or five round trips even on a fast link. Reusing warm connections avoids
restarting slow start. (Linux also resets cwnd after an idle period, `tcp_slow_start_after_idle`,
which surprises people pooling connections to a far-away database.)

### 2.7 Nagle's algorithm and TCP_NODELAY

Nagle's rule: if there is unacknowledged data in flight, buffer small writes until an ACK arrives or
a full segment is ready. Purpose: avoid sending 41-byte packets for 1-byte payloads (telnet era).

Problem: combined with delayed ACKs on the other side, a request-response protocol that does
`write(header); write(body);` suffers a 40 ms stall: the body waits for the ACK of the header, and
the receiver delays that ACK waiting for more data. Classic symptom: "every request takes exactly
40 ms more than it should".

Fixes, in order of preference:
1. Build the whole message in one buffer and call `write` once (always do this anyway).
2. Use `writev` or `sendmsg` to send header and body in one call.
3. Set `TCP_NODELAY` to disable Nagle. Almost every RPC and database driver does this (libpq does).
4. `TCP_CORK` (Linux): hold until uncorked; useful for `sendfile` after a header.

Your RPC-over-TCP server should set `TCP_NODELAY` on every accepted socket. Check whether it does.

### 2.8 Keepalive

TCP has no heartbeat by default. An idle connection with a dead peer (power cut, cable pulled,
NAT mapping expired) stays ESTABLISHED forever on your side. `SO_KEEPALIVE` makes the kernel send
probes after `tcp_keepalive_time` idle seconds (default 7200, two hours, far too long), repeating
every `tcp_keepalive_intvl` (75 s) up to `tcp_keepalive_probes` (9) times, then kills the
connection with `ETIMEDOUT`. Per-socket overrides on Linux: `TCP_KEEPIDLE`, `TCP_KEEPINTVL`,
`TCP_KEEPCNT`. Sensible server values: idle 60 s, interval 10 s, count 5. Windows: `SIO_KEEPALIVE_VALS`
via `WSAIoctl`, or `TCP_KEEPIDLE`/`TCP_KEEPINTVL` on recent Windows 10+.

Keepalive detects dead peers; it does not detect a hung application on a live host. For that you
need application-level heartbeats or request timeouts (module 09).

### 2.9 Closing: FIN, half-close, RST

```
Active closer (A)                       Passive closer (B)
  FIN, seq=m  -->                        B: CLOSE-WAIT    (B's read() returns 0)
  <-- ACK, ack=m+1                       A: FIN-WAIT-2
  ... B may still send data to A ...     (half-close)
  <-- FIN, seq=n                         B: LAST-ACK
  ACK, ack=n+1 -->                       A: TIME-WAIT (2*MSL)   B: CLOSED
```

**Half-close** (`shutdown(fd, SHUT_WR)`): "I am done sending, but I will still read your reply".
Useful for a client that sends a request then wants the full response until EOF. `close()` does
both directions and also releases the fd. When the peer sends FIN, your `read()` returns 0: that is
**EOF**, not an error. Treat 0 as "peer finished sending"; do not loop on it.

**RST** is the abort path. You receive RST when: you send to a port nobody listens on (`ECONNREFUSED`
at connect), the peer process died and the kernel cleaned up, the peer closed with unread data in
its receive buffer, or a firewall/NAT lost the connection state. Your `read` or `write` gets
`ECONNRESET`. Writing to a socket that has already received RST gets `EPIPE` and, unless you
disabled it, **SIGPIPE** kills your process. Every server must ignore SIGPIPE or use `MSG_NOSIGNAL`
(module 03).

### 2.10 The state machine and what the states mean at 3 AM

```
                 +---------+
       passive   | CLOSED  |  active open: send SYN
       open      +---------+----------------------+
         |                                        v
   +-----v-----+                            +-----------+
   |  LISTEN   |                            | SYN-SENT  |
   +-----+-----+                            +-----+-----+
     rcv SYN, send SYN+ACK                  rcv SYN+ACK, send ACK
         v                                        v
   +-----------+   rcv ACK                  +-------------+
   | SYN-RCVD  +--------------------------->| ESTABLISHED |<--+
   +-----------+                            +------+------+   |
                                                   |          |
         app close(): send FIN                     |     rcv FIN: send ACK
                 +---------------------------------+-------------------+
                 v                                                     v
          +------------+                                       +-------------+
          | FIN-WAIT-1 |                                       | CLOSE-WAIT  |  <-- waiting for
          +-----+------+                                       +------+------+      YOUR close()
     rcv ACK    |                                             app close(): send FIN
                v                                                     v
          +------------+                                       +-------------+
          | FIN-WAIT-2 |                                       |  LAST-ACK   |
          +-----+------+                                       +------+------+
     rcv FIN, send ACK                                           rcv ACK
                v                                                     v
          +------------+   2*MSL timer (60 s on Linux)          +---------+
          | TIME-WAIT  +--------------------------------------->| CLOSED  |
          +------------+                                        +---------+
```

**TIME_WAIT** is held by whoever closed *first* (the active closer). It lasts 2 x MSL (maximum
segment lifetime), 60 seconds on Linux, 120 s default on Windows (`TcpTimedWaitDelay`, 30 s
minimum). Two reasons: (1) if the final ACK is lost, the peer retransmits FIN and we must still
answer; (2) delayed segments from this connection must die before the same 4-tuple is reused, or a
new connection could accept stale data.

Lots of TIME_WAIT is **normal** on a busy server that closes connections, and it costs little
(a small kernel structure, no fd). It becomes a problem on a **client** that opens thousands of
short connections per minute to one server: each connection burns an ephemeral port for 60 s, and
28,000 ports / 60 s means about 470 new connections per second before `connect` fails with
`EADDRNOTAVAIL`. Fixes: connection pooling (the real fix), `net.ipv4.tcp_tw_reuse=1` (safe with
timestamps, client side), let the *server* close first so TIME_WAIT lands on the server, or widen
`ip_local_port_range`. Do not use `tcp_tw_recycle` (removed in Linux 4.12; broke NAT).

**CLOSE_WAIT** is the side that received FIN but has **not called `close()` itself**. The kernel
is waiting for your application. A growing count of CLOSE_WAIT sockets has one meaning: **your
code leaked a socket**. You read 0 from `read()` and did not close the fd, or an exception path
skipped `close`, or a connection object sits in a pool forever. CLOSE_WAIT never expires on its own
(there is no timer in that state). This is the single most useful `ss` observation in production.

**FIN_WAIT_2** piling up on your side means you closed and the peer never sent its FIN: the
peer has leaked the socket (their CLOSE_WAIT). Linux times out orphaned FIN_WAIT_2 after
`tcp_fin_timeout` (60 s).

**SYN_RECV** piling up: accept queue full (your `accept` loop is stuck or too slow) or SYN flood.
Linux SYN cookies mitigate floods.

### 2.11 Head-of-line blocking

Because TCP delivers bytes in order, if segment 5 is lost, segments 6 to 20 sit in the receive
buffer and are not delivered to your application until segment 5 is retransmitted and arrives. One
lost packet stalls everything behind it for at least one RTT. This is **head-of-line (HOL)
blocking**. It is why multiplexing many independent requests over one TCP connection (HTTP/2) can be
worse than several connections on a lossy link, and why QUIC (HTTP/3) runs over UDP with per-stream
ordering: a loss on stream A does not stall stream B.

### 2.12 "TCP is a byte stream, not messages": the number one bug

You call `send(fd, "SET a 1\r\n", 9)`. The receiver calls `recv(fd, buf, 4096)`. Any of these is
correct TCP behaviour:

- `recv` returns 9 bytes: your whole message. (Common on loopback. This is why the bug hides in
  testing.)
- `recv` returns 3 bytes `SET`, next call returns 6. (Segment boundaries, small MSS, GRO on/off.)
- `recv` returns 18 bytes: your message *and* the next one you sent 1 ms later. (Nagle, or the
  receiver was slow and both arrived before it read.)
- `recv` returns 1 byte.

TCP has no record boundaries. `send` boundaries are not preserved. The receiving application must
know, from the bytes themselves, where a message ends: a delimiter, a length prefix, or a fixed
size. Module 04 is entirely about this. Every time you see code that assumes "one `recv` equals one
message", it is a bug waiting for a slow network or a bigger payload.

The same applies to `send`: it may write fewer bytes than asked (non-blocking socket, or signal
interruption on a blocking one). You must loop (module 03).

### 2.13 UDP contrast

UDP: 8-byte header (ports, length, checksum). Each `sendto` is one datagram; each `recvfrom`
returns exactly one whole datagram (or truncates it if your buffer is too small). Message boundaries
*are* preserved. No handshake, no ordering, no retransmission, no flow or congestion control, no
connection state in the kernel. Datagrams above the path MTU get fragmented and a single lost
fragment loses the whole datagram, so keep UDP payloads under about 1400 bytes.

Use UDP when you can tolerate loss and need low latency or multicast (DNS, metrics like statsd,
game state, media). Use TCP for anything where losing data is a bug (RPC, databases, HTTP/1 and /2).
QUIC rebuilds reliability over UDP to escape HOL blocking and to evolve faster than kernel TCP.

## 3. Code

`code/m02_tcp_options.cpp` creates a connected socket pair on loopback and prints and sets the
options that this module discussed. Key parts:

```cpp
// Read an int option. Each getsockopt call is a syscall; fine for a diagnostic tool.
static int get_int_opt(int fd, int level, int opt) {
    int v = 0; socklen_t len = sizeof v;
    if (getsockopt(fd, level, opt, &v, &len) < 0) die("getsockopt");
    return v;
}

// Disable Nagle: every write goes out as soon as possible. Do this on every RPC socket,
// then also build full messages before writing, otherwise you flood the wire with tiny packets.
static void set_nodelay(int fd) {
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0) die("TCP_NODELAY");
}

// Keepalive tuned for a server: detect a dead peer within about 60+5*10 = 110 seconds.
static void set_keepalive(int fd, int idle_s, int intvl_s, int cnt) {
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) < 0) die("SO_KEEPALIVE");
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s) < 0) die("TCP_KEEPIDLE");
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof intvl_s) < 0) die("TCP_KEEPINTVL");
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt) < 0) die("TCP_KEEPCNT");
}
```

Run it and compare the printed `SO_RCVBUF` with `/proc/sys/net/ipv4/tcp_rmem`. Note that Linux
reports double the value you set for `SO_SNDBUF`/`SO_RCVBUF` (it reserves half for bookkeeping).

Shell experiments to pair with the module:

```bash
# Watch TIME_WAIT appear and expire. Client closes first => client holds TIME_WAIT.
python3 -m http.server 8000 &
curl -s localhost:8000 >/dev/null; ss -tan | grep 8000      # shows TIME-WAIT on the ephemeral port
sleep 61; ss -tan | grep 8000                                # gone

# Create a CLOSE_WAIT deliberately: server accepts, client closes, server never closes.
python3 -c "
import socket,time; s=socket.socket(); s.bind(('127.0.0.1',7001)); s.listen()
c,_=s.accept(); print(c.recv(10)); time.sleep(300)" &
sleep 0.3; printf 'x' | nc -q0 127.0.0.1 7001
ss -tan | grep 7001       # one CLOSE-WAIT (server side) and one FIN-WAIT-2 (client side)

# Kernel tunables mentioned in this module
sysctl net.ipv4.tcp_fin_timeout net.ipv4.tcp_tw_reuse net.ipv4.ip_local_port_range \
       net.ipv4.tcp_keepalive_time net.ipv4.tcp_congestion_control net.ipv4.tcp_retries2
```

## 4. Under the hood (senior-level)

**Where the bytes live.** `send()` copies from your buffer into kernel `sk_buff`s on the socket's
write queue and returns; the data has not left the machine. TCP later forms segments from the queue
up to `min(cwnd, rwnd)`, hands them to IP, and keeps them on the retransmit queue until ACKed. On
the receiver, in-order segments are appended to the receive queue; out-of-order ones go to an
out-of-order queue until the gap fills. `recv()` copies from the receive queue to your buffer. So a
TCP byte is copied at least twice (user to kernel, kernel to user) plus DMA. Zero-copy paths
(`sendfile`, `MSG_ZEROCOPY`, io_uring registered buffers) exist for large transfers.

**Where the connection lives before `accept`.** Linux keeps two queues per listening socket: the
SYN queue (half-open, SYN-RECV) and the accept queue (ESTABLISHED, waiting for `accept`). The
`backlog` argument to `listen` caps the accept queue (also capped by `net.core.somaxconn`, default
4096 on recent kernels). When the accept queue is full, new SYNs are dropped (the client retries
after 1 s, then 3 s...) which shows up as "connections randomly take 1 or 3 seconds". Monitor
`nstat -az TcpExtListenOverflows`.

**RTT estimation.** TCP timestamps let the sender measure RTT per ACK. SRTT and RTTVAR are
exponentially weighted averages; RTO = SRTT + 4 * RTTVAR, floored at 200 ms. This is why a LAN
retransmission still waits 200 ms: the floor dominates. Tail latency spikes of exactly ~200 ms in a
LAN service are a strong hint of packet loss and RTO.

**Why TIME_WAIT is 2 x MSL.** MSL is the longest a segment can survive in the network (TTL-bound;
RFC 793 said 2 minutes, Linux uses 30 s so 2 x MSL = 60 s). After the final ACK, the worst case is:
our ACK is lost (up to MSL in flight), the peer retransmits FIN (another MSL). After 2 x MSL no
segment from this incarnation can still exist, so the 4-tuple is safe to reuse.

**Sequence number wrap.** 32 bits wrap after 4 GB of data in one direction. At 10 Gbit/s that is
3.4 seconds, less than MSL. TCP timestamps (PAWS: protection against wrapped sequences) solve the
ambiguity. This is one reason timestamps are on by default.

**`SO_LINGER`.** By default `close()` returns immediately and the kernel delivers queued data in
the background then sends FIN. `SO_LINGER` with timeout 0 sends RST immediately and discards
unsent data, skipping TIME_WAIT. Some load-testing tools do this to avoid port exhaustion. Never
do it in a real server: the peer loses data and sees `ECONNRESET`.

**Windows differences.** Same protocol, same states (`netstat -ano`, `Get-NetTCPConnection`).
TIME_WAIT default 120 s via `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\TcpTimedWaitDelay`.
Default ephemeral range is 49152 to 65535 (16,384 ports), so port exhaustion arrives sooner than
on Linux. Windows has no SIGPIPE; `send` to a reset connection returns `SOCKET_ERROR` with
`WSAECONNRESET` or `WSAECONNABORTED`. Congestion control is CUBIC on modern Windows too.
Keepalive defaults are 2 hours idle, 1 s interval, 10 probes.

## 5. In production

- **The 40 ms mystery.** An RPC client wrote the header and the body in two `send` calls. Every
  call took 40 ms longer than it should. Nagle plus delayed ACK. Fix: one `send` per message and
  `TCP_NODELAY`. If you measure your employer's RPC transport and see a floor near 40 ms on a LAN,
  this is the first suspect.
- **"Connection reset by peer" after idle.** A firewall between branch and data centre dropped
  idle TCP state after 15 minutes. The next request hit a dead mapping, firewall sent RST.
  Fix: keepalive at 5 minutes, and the client reconnects on `ECONNRESET` and retries if the request
  is idempotent (module 07).
- **CLOSE_WAIT leak.** After a release, `ss -s` on the application server showed CLOSE_WAIT
  climbing by hundreds per hour. A new error path returned early without closing the client socket.
  Within a day the process hit the fd limit and `accept` failed with `EMFILE`. Lesson: RAII on the
  socket (module 03) makes this class of bug impossible; and alert on CLOSE_WAIT count.
- **Port exhaustion on a batch client.** A migration tool opened a new connection per row.
  After a minute `connect` failed with `EADDRNOTAVAIL`. TIME_WAIT had eaten the ephemeral range.
  Fix: one connection, or a pool. The Berkeley DB to YugabyteDB migration you worked on used a pool of
  long-lived connections; this is why that matters.
- **15-minute hang on a dead host.** A database node lost power. Connections to it had data in
  flight; the kernel retransmitted with exponential backoff for `tcp_retries2` rounds, about 15
  minutes, while the application thread sat in `send`. Nothing in the application had a timeout.
  Fix: `SO_SNDTIMEO`/`SO_RCVTIMEO` or `poll` with timeout (module 05), application-level deadlines,
  and lower `tcp_retries2` on client hosts (around 8 gives about 100 s).
- **Read the capture, not the logs.** When two teams blame each other's service, a `tcpdump` on
  both sides settles it in minutes: who sent FIN first, who sent RST, were there retransmissions,
  was the window zero (receiver too slow). Learn to read `[F.]`, `[R]`, `win 0` and
  "TCP Retransmission" in Wireshark.

## 6. Try it

1. **(easy)** Using the capture from module 01, identify which side sent FIN first, and with `ss`
   confirm that side holds TIME_WAIT. Done when: you can state the rule "active closer holds
   TIME_WAIT" from evidence.

2. **(easy)** Write down the TCP state machine from memory (states and transitions for a normal
   open and close). Check it against section 2.10. Done when: no state is missing.

3. **(medium)** Reproduce the Nagle + delayed ACK stall: write a client that sends a 1-byte header
   and a 10-byte body in two `send` calls and waits for a 1-byte reply from a tiny server; time 100
   iterations with and without `TCP_NODELAY`. Done when: you observe a large difference (on loopback
   Linux may not show it; try with the server in another WSL distro or a VM if needed, and explain why).

4. **(medium)** Reproduce CLOSE_WAIT with the Python snippet in section 3, then write the C++ fix:
   a server that treats `read() == 0` by closing. Verify with `ss` that the state goes to CLOSED.
   Done when: `ss -tan | grep 7001` is empty after the client disconnects.

5. **(hard)** Estimate the maximum connection rate a client can sustain to a single server
   given the default `ip_local_port_range` and a 60 s TIME_WAIT, then verify by writing a Python
   loop that opens and closes connections as fast as possible and reports when `connect` fails.
   Then enable `tcp_tw_reuse` and repeat. Done when: your measured limit matches the estimate within
   a factor of two and you can explain the mechanism.

6. **(hard)** Using `tc qdisc add dev lo root netem loss 5% delay 20ms`, transfer 10 MB over
   loopback with `nc` and watch retransmissions in `tcpdump`/Wireshark. Measure throughput.
   Remove the qdisc (`tc qdisc del dev lo root`). Done when: you can point at a fast retransmit
   and an RTO-triggered retransmit in the capture and explain the throughput drop.

## 7. Interview questions

**Q: Why three packets for the handshake and not two?**
A: Each side must get its own ISN acknowledged so both know the other can receive. Two packets
would leave the server unsure that the client got its SYN-ACK. The third packet (ACK) can already
carry data.

**Q: You see 30,000 sockets in TIME_WAIT on a server. Is that a problem?**
A: Usually not on a server: TIME_WAIT costs a small kernel structure and no fd, and is normal when
the server closes connections first. It is a problem on a client exhausting ephemeral ports.
The fix is pooling; `tcp_tw_reuse` helps on the client; letting the server close first moves the
TIME_WAIT. Never use `SO_LINGER 0` to avoid it in production.

**Q: You see CLOSE_WAIT growing. What does it mean?**
A: The peer closed (sent FIN), the kernel ACKed it, and our application has not called `close()`.
It is always an application bug: a leaked fd, an error path skipping close, or ignoring `read() == 0`.
It never times out by itself. RAII wrappers prevent it.

**Q: What is the difference between flow control and congestion control?**
A: Flow control protects the receiver: the advertised window says how much buffer space it has.
Congestion control protects the network: the sender's cwnd grows (slow start, then linear) and
shrinks on loss. The sender uses the minimum of the two.

**Q: What is Nagle's algorithm and when do you disable it?**
A: It delays small writes while data is unacknowledged, to avoid tiny packets. With delayed ACKs
on the peer it causes about 40 ms stalls in write-write-read patterns. Disable with `TCP_NODELAY`
on latency-sensitive request-response protocols, and always coalesce a message into one `send`.

**Q: Why is "one recv equals one message" wrong?**
A: TCP is a byte stream; segment boundaries and `send` boundaries are not preserved. `recv` can
return part of a message, several messages, or one byte. The application needs framing (length
prefix or delimiter) and an incremental parser.

**Q: How does a connection to a crashed machine behave?**
A: If idle: nothing, it stays ESTABLISHED forever unless keepalive is on. If sending: the kernel
retransmits with exponential backoff for `tcp_retries2` rounds, about 15 minutes by default, and
then returns `ETIMEDOUT`. If the machine reboots and comes back, it answers with RST and you get
`ECONNRESET`. Application timeouts are mandatory.

**Q: What is head-of-line blocking?**
A: In-order delivery means one lost segment holds back all later bytes until it is retransmitted.
Multiplexed streams over one TCP connection all stall together. QUIC avoids it with independent
streams over UDP.

## 8. Recap

- Handshake: SYN, SYN-ACK, ACK; negotiates MSS, window scale, SACK, timestamps; costs one RTT.
- Every byte is numbered; ACKs are cumulative; SACK fills holes; lost data is retransmitted by RTO
  (min 200 ms, doubles) or fast retransmit (3 dup ACKs).
- Flow control = receiver window; congestion control = cwnd with slow start and CUBIC/BBR; sender
  uses the minimum.
- Nagle + delayed ACK = 40 ms stalls; coalesce writes and set `TCP_NODELAY`.
- Keepalive detects dead peers; defaults are 2 hours; tune per socket.
- `read() == 0` is EOF (peer FIN). `ECONNRESET`/`EPIPE` are RST. Ignore SIGPIPE in servers.
- TIME_WAIT: active closer, 60 s, normal on servers, a port problem on clients.
- CLOSE_WAIT: you forgot to close. Never expires. Always an application bug.
- TCP is a byte stream; frame your messages and loop on partial reads and writes.
- UDP preserves datagram boundaries but guarantees nothing else.
