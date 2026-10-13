# 01. How the internet moves bytes

**Time:** ~2.5 hours   **Prerequisites:** none

## 1. In plain words

When your branch client sends a request to the application server, the bytes do not fly straight
there. They are wrapped in an envelope, the envelope is put into a bigger envelope, and the bigger
envelope is put into a box. Each wrapper has one job: the box gets the data to the next machine
on the same cable; the big envelope gets it across the world to the right computer; the small
envelope gets it to the right program on that computer and makes sure nothing is lost.
Your program only sees the letter inside.

That is the whole idea of network layers. Each layer solves one problem and trusts the layer below.
The postal analogy is accurate: the link layer is the delivery van (one hop), IP is the address on
the envelope (many hops, no guarantee), TCP is registered post with a return receipt (order and
delivery guaranteed), and the application layer is whatever you wrote in the letter.

A **packet** is one unit of data at a given layer. A packet has a maximum size, so a big message is
cut into many packets. Each computer has an **IP address** (which computer) and each program
listening for connections has a **port** (which program on that computer). **DNS** turns a name
like `db.bank.internal` into an IP address before any of this starts.

## 2. The real thing

### 2.1 The four layers you actually deal with

```
 +---------------------------+   Application: HTTP, your RPC protocol, tcpkv text, PostgreSQL wire
 |   Application             |   protocol. Bytes that mean something to a program.
 +---------------------------+
 |   Transport (TCP / UDP)   |   Ports, and for TCP: ordering, reliability, flow control.
 +---------------------------+
 |   Internet (IP)           |   Addresses and routing between networks. Best effort: packets can
 |                           |   be lost, duplicated, reordered.
 +---------------------------+
 |   Link (Ethernet, Wi-Fi)  |   Moves frames between two machines on the same physical network.
 +---------------------------+   MAC addresses.
```

The classic OSI model has seven layers. In practice engineers name four or five, and interviewers
expect you to know: Ethernet frame contains IP packet contains TCP segment contains your bytes.

Encapsulation on the wire for one TCP segment carrying 100 bytes of your data:

```
| Ethernet hdr 14 | IP hdr 20 | TCP hdr 20 (up to 60) | your 100 bytes | Ethernet FCS 4 |
```

Headers are overhead. Sending one byte of data costs about 54 bytes of headers plus a 20-byte
minimum frame padding. This is why batching small writes matters (module 02, Nagle).

### 2.2 IP: addresses, packets, routing

**IPv4 address**: 32 bits, written as four decimal octets, `10.20.30.40`. **IPv6**: 128 bits,
written in hex groups, `2001:db8::1`. Private ranges you see inside a bank: `10.0.0.0/8`,
`172.16.0.0/12`, `192.168.0.0/16`. `127.0.0.1` (`::1` for IPv6) is loopback: the packet never
leaves the machine but goes through the whole IP stack. `0.0.0.0` means "all interfaces" when you
bind a server.

**CIDR notation**: `10.20.0.0/16` means the first 16 bits are the network, the rest is the host.
A machine decides "is the destination on my local network?" by masking both addresses with the
prefix and comparing. If yes: deliver directly on the link layer. If no: send to the **default
gateway** (a router), which forwards it one hop closer. Every router does the same with its own
routing table. No router knows the whole path; each knows only the next hop.

**TTL** (time to live): a counter in the IP header, decremented by each router. At zero the packet
is dropped and an ICMP "time exceeded" is sent back. This prevents loops forever and is how
`traceroute` works (it sends packets with TTL 1, 2, 3... and records who complains).

**MTU** (maximum transmission unit): the largest IP packet the link can carry. Ethernet: 1500 bytes.
Minus 20 IP and 20 TCP header bytes gives the usual **MSS** (maximum segment size) of 1460 bytes of
TCP payload per packet. If a packet is bigger than a link's MTU it must be fragmented (IPv4) or
dropped with an ICMP message (IPv6, and IPv4 when the "don't fragment" bit is set). TCP avoids
fragmentation by negotiating MSS during the handshake and by path MTU discovery. A classic
production problem: a VPN or tunnel lowers the effective MTU, ICMP is blocked by a firewall, and
large responses hang while small ones work. Symptom: `SELECT 1` works, `SELECT * FROM big_table`
hangs forever.

### 2.3 Ports and the transport layer

A port is a 16-bit number (0 to 65535). A TCP connection is identified by a **4-tuple**:
`(source IP, source port, destination IP, destination port)`. That is how one server on port 5433
can have 10,000 connections: each client has a different source IP or source port.

- Ports 0 to 1023: privileged, need root to bind on Linux (80 HTTP, 443 HTTPS, 22 SSH, 53 DNS).
- 1024 to 49151: registered (5432 PostgreSQL, 5433 YugabyteDB YSQL, 9042 YCQL, 6379 Redis).
- 49152 to 65535: ephemeral. When your client calls `connect` without `bind`, the kernel picks a free
  port here. Linux default range is `32768-60999` (`cat /proc/sys/net/ipv4/ip_local_port_range`).
  That is only 28,232 ports, which becomes a real limit for a client opening many short-lived
  connections to one server (module 02, TIME_WAIT).

TCP gives you a reliable, ordered byte stream. UDP gives you independent datagrams with no
guarantees: no ordering, no retransmission, no connection. UDP is used where latency matters more
than every packet (DNS, video, games, QUIC which builds its own reliability on top).

### 2.4 NAT

**NAT** (network address translation): a router rewrites the source IP and port of outgoing
packets to its own public IP and a port it chooses, remembers the mapping, and rewrites replies
back. Your home has one public IP and twenty devices because of NAT. Consequences for server work:

- A server behind NAT cannot be connected to from outside unless the NAT has a forwarding rule.
- The server sees the NAT's IP, not the client's. Logging "client IP" behind a corporate NAT shows
  one IP for the whole branch. Use an application-level identifier (branch code, user id) in logs.
- NAT tables have timeouts (typical 30 seconds to a few minutes for idle UDP, longer for TCP).
  An idle TCP connection through NAT can be silently dropped; the next write fails or hangs. This is
  one reason for TCP keepalive (module 02).

### 2.5 DNS resolution

DNS turns `db.bank.internal` into `10.20.30.40`. The sequence for a client:

1. Check `/etc/hosts` and the local cache (`systemd-resolved` on Ubuntu).
2. Ask the configured **recursive resolver** (from `/etc/resolv.conf`, usually your company's or ISP's).
3. The resolver, if it does not have the answer cached, asks a **root server** ("who handles
   `.internal`?" or `.com`), then the **TLD server** ("who handles `bank.com`?"), then the
   **authoritative server** for `bank.com` ("what is the A record for `db`?").
4. The answer comes back with a **TTL** (seconds it may be cached).

Record types you will meet: **A** (name to IPv4), **AAAA** (name to IPv6), **CNAME** (alias to
another name), **MX** (mail), **SRV** (service location, port included), **PTR** (reverse lookup).

In code you never do this by hand: `getaddrinfo` (module 03) does steps 1 and 2 and returns a list
of addresses. A name can map to several IPs; a good client tries them in order (this is how
"happy eyeballs" IPv6/IPv4 fallback works).

Production trap: DNS caching inside your process. Some runtimes (the JVM historically) cache
forever by default. If the database VIP moves to a new IP during failover, a process with a stale
cache keeps connecting to the dead IP. Always re-resolve on reconnect, and respect TTL.

### 2.6 What happens when a client connects

Client at `10.1.1.5` runs `connect()` to `db.bank.internal:5433`. In order:

1. **DNS**: `getaddrinfo` resolves the name to `10.20.30.40` (section 2.5).
2. **Routing decision**: `10.20.30.40` is not in `10.1.1.0/24`, so the next hop is the default
   gateway `10.1.1.1`. (`ip route get 10.20.30.40` shows this decision.)
3. **ARP** (address resolution protocol): the client needs the MAC address of `10.1.1.1`. It checks
   its ARP cache (`ip neigh`). If missing, it broadcasts "who has 10.1.1.1?" on the Ethernet
   segment and the router answers with its MAC. (IPv6 uses Neighbor Discovery instead.)
4. **Ethernet frame** to the router's MAC, containing an IP packet to `10.20.30.40`, containing a
   TCP segment with the **SYN** flag set, a random initial sequence number, and options (MSS,
   window scale, SACK permitted, timestamps).
5. Each router forwards by its routing table, decrementing TTL, rewriting the Ethernet header at
   each hop but leaving the IP and TCP headers alone (unless NAT is involved).
6. The server's kernel receives the SYN on a socket in LISTEN state on port 5433, replies with
   **SYN-ACK**, and the client replies with **ACK**. Three-way handshake complete (module 02).
7. The server's `accept()` returns a new socket for this connection. Until then the connection sits
   in the **accept queue** (backlog). Your application code has not run yet; the kernel did all
   of this.
8. Now the application protocol starts: the client sends its first bytes.

Time budget on a LAN: DNS cached, ARP cached, handshake one round trip (typically well under 1 ms
on a LAN, 10 to 100 ms across regions). Across regions, every extra round trip costs that latency
again. That is why keep-alive and connection pooling matter (module 09).

### 2.7 Tools

All of these exist on WSL Ubuntu 24.04. Run them while you read.

**`ping host`**: sends ICMP echo requests, prints round-trip time. Confirms reachability and
measures latency. Note: ICMP being blocked does not mean the TCP port is down.

**`ip addr`**, **`ip route`**, **`ip neigh`**: your interfaces and addresses, your routing table,
your ARP cache. `ip route get 8.8.8.8` shows exactly which interface and gateway a destination uses.

**`ss -tlnp`**: listening TCP sockets with the owning process. `ss -tanp` shows all TCP sockets
with their state. `ss -s` shows counts per state. This replaces the old `netstat`. On Windows the
equivalent is `netstat -ano`.

```
$ ss -tanp
State      Recv-Q Send-Q Local Address:Port   Peer Address:Port  Process
LISTEN     0      128    0.0.0.0:7000         0.0.0.0:*          users:(("tcpkv",pid=4242,fd=3))
ESTAB      0      0      127.0.0.1:7000       127.0.0.1:51234    users:(("tcpkv",pid=4242,fd=5))
TIME-WAIT  0      0      127.0.0.1:51230      127.0.0.1:7000
```

`Recv-Q` on a LISTEN socket is the number of completed connections waiting for `accept()`; `Send-Q`
there is the backlog limit. On an ESTAB socket, `Recv-Q` is bytes the application has not read yet.
A growing `Recv-Q` on your server means your server is not calling `read` fast enough.

**`dig name`**: DNS lookup with full detail (answer section, TTL, which server answered).
`dig +trace name` walks the resolution from the root. `dig -x 10.20.30.40` does a reverse lookup.

**`tcpdump`**: captures packets. `sudo tcpdump -i lo -nn port 7000` shows traffic to port 7000 on
loopback with numeric addresses. Add `-w cap.pcap` to save and open in Wireshark on Windows.
Reading a capture of a tiny exchange:

```
12:00:01.000001 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [S], seq 1000, win 65495, options [mss 65495,sackOK,TS val 1 ecr 0,nop,wscale 7], length 0
12:00:01.000020 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [S.], seq 5000, ack 1001, win 65483, options [mss 65495,sackOK,TS val 1 ecr 1,nop,wscale 7], length 0
12:00:01.000030 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [.], ack 1, win 512, length 0
12:00:01.000500 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [P.], seq 1:14, ack 1, win 512, length 13
12:00:01.000520 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [.], ack 14, win 512, length 0
12:00:01.000900 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [P.], seq 1:14, ack 14, win 512, length 13
```

How to read it: `[S]` SYN, `[S.]` SYN-ACK, `[.]` ACK only, `[P.]` PSH+ACK (data), `[F.]` FIN,
`[R]` RST. After the handshake tcpdump prints sequence numbers *relative* to the initial ones, so
`seq 1:14` means "bytes 1 through 13 of this direction". The `win` is the receive window; `wscale 7`
means multiply by 128. The 13-byte message got its own ACK, then a 13-byte reply. Four packets for
one request-response. In Wireshark, right-click a packet and choose "Follow > TCP Stream" to see
the reassembled bytes of your protocol.

## 3. Code

No new C++ this module. Use the shell on WSL.

```bash
# 1. Your addresses and route to the outside world
ip -brief addr
ip route
ip route get 1.1.1.1

# 2. Resolve a name and see the TTL
dig +noall +answer example.com
dig +noall +answer +ttlid yugabyte.com

# 3. Watch a TCP handshake on loopback. In terminal A:
sudo tcpdump -i lo -nn 'port 7000'
# In terminal B, start any TCP listener and connect to it:
python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',7000));s.listen();c,_=s.accept();print(c.recv(100))" &
sleep 0.5; printf 'hello tcpkv\r\n' | nc -q1 127.0.0.1 7000

# 4. See socket states while something is connected
ss -tanp | grep 7000
ss -s
```

Expected: terminal A shows `[S]`, `[S.]`, `[.]`, a `[P.]` with `length 13`, then `[F.]` from
both sides. `ss -tanp` run right after shows a `TIME-WAIT` entry on the side that closed first.

## 4. Under the hood (senior-level)

**The kernel owns the protocol stack.** Your process never sees Ethernet or IP headers. On
`send()`, the kernel copies your bytes into the socket's send buffer (`sk_buff` structures in
Linux), the TCP layer decides when to form a segment, the IP layer adds the header and consults
the routing table (a trie in Linux, `fib_trie`), the neighbor subsystem finds the MAC, and the
network driver puts the frame in the NIC's transmit ring via DMA. On receive, the NIC raises an
interrupt (or is polled under load, NAPI), the driver hands frames up, IP checks the destination,
TCP looks up the socket by 4-tuple in a hash table, places the bytes in the socket's receive
buffer, and wakes any thread blocked in `recv()`.

**Loopback is not free.** `127.0.0.1` skips the NIC but still goes through TCP, IP and the
socket layer. It costs two system calls and two copies per message, typically a few microseconds
per round trip on modern hardware. It is a realistic test of your protocol logic, not of network
latency.

**Offloads.** Modern NICs do checksumming and segmentation (TSO/GSO) so the kernel can hand a
64 KB "segment" to the NIC and let hardware split it into MSS-size packets. This is why `tcpdump` on
the sending side sometimes shows segments larger than the MTU: the capture point is above the
offload. Capture on the receiver or disable offloads (`ethtool -K eth0 tso off`) when that confuses you.

**Socket buffers are the shock absorbers.** `SO_SNDBUF` and `SO_RCVBUF` set per-socket sizes;
Linux autotunes them between the bounds in `/proc/sys/net/ipv4/tcp_rmem` and `tcp_wmem`. The
receive buffer size directly limits the advertised TCP window and therefore throughput on a
high-latency link (module 02, bandwidth-delay product).

**Windows difference.** The same layering exists. The Windows stack is `tcpip.sys`; the user-mode
API is Winsock (`ws2_32.dll`), which maps to the AFD driver. `netstat -ano` replaces `ss`,
`Get-NetTCPConnection` in PowerShell shows states, `nslookup` or `Resolve-DnsName` replaces `dig`,
and Wireshark or `pktmon` replaces `tcpdump`.

## 5. In production

- **"The server is down" is rarely one thing.** Separate the layers when debugging: can you ping
  the IP (IP layer)? Can you `nc -zv host port` (TCP port open)? Does `ss -tlnp` on the server show
  the process listening on `0.0.0.0` or only on `127.0.0.1` (bind mistake)? Is the firewall
  dropping (`iptables -L -n`, or on Windows the Defender firewall)? Does DNS return the IP you
  expect (`dig`)? Each question maps to one layer.
- **Bound to the wrong interface.** A service that binds `127.0.0.1` works in local testing and is
  unreachable from the branch. Bind `0.0.0.0` (or the specific interface) for a server.
- **Stale DNS after failover.** After moving YugabyteDB to new nodes, clients that cached the old IP
  kept failing. The fix was to re-resolve on every reconnect and to lower the TTL before the cutover.
  Lowering TTL a day in advance is a standard pre-cutover step (module 13).
- **NAT idle timeout drops.** Branch clients through a firewall saw "connection reset" after idle
  periods. The firewall had a 15-minute idle timeout. TCP keepalive at 5 minutes fixed it.
- **MTU black holes.** Through a site-to-site VPN, small queries worked and large result sets hung.
  Path MTU discovery was broken because ICMP was filtered. Fix: clamp MSS on the VPN router or lower
  the MTU on the interface. Diagnosis: `ping -M do -s 1472 host` to find the largest unfragmented size.
- **Reading `ss` under load.** `Recv-Q` growing on your server's connections means your application
  is slower than the network: look at your threads. `SYN-RECV` piling up means the accept queue is
  full or you are under a SYN flood. Many `CLOSE-WAIT` means your code never called `close()`
  (module 02).

## 6. Try it

1. **(easy)** Run `ip route get` for three destinations: `127.0.0.1`, your WSL gateway, and
   `1.1.1.1`. Write one sentence each about which interface and gateway are used and why.
   Done when: you can explain the three decisions.

2. **(easy)** Use `dig` to find the A records and TTL of two public hosts and one CNAME chain
   (`www.github.com` is a CNAME). Done when: you can list the chain and the final IPs.

3. **(medium)** Capture a handshake plus one request to any local TCP service with `tcpdump -w`.
   Open it in Wireshark on Windows (the file is under `\\wsl$\Ubuntu\home\...`). Use Follow TCP
   Stream. Done when: you can point at the SYN, SYN-ACK, ACK, the data segment, and the FINs, and
   state the MSS each side offered.

4. **(medium)** Compute: a link with 100 Mbit/s and 50 ms round-trip time. How many bytes must be
   "in flight" to keep the link full? (Bandwidth-delay product: bytes/s times seconds.) Compare to
   the default 64 KB window without window scaling. Done when: you can say why window scaling was
   needed and what throughput the 64 KB window limits you to.

5. **(hard)** Write a shell script that, given a hostname and port, reports which layer fails:
   DNS (`getent hosts`), IP reachability (`ping -c1 -W1`), TCP port (`nc -zv -w2`), and prints a
   one-line diagnosis. Done when: it gives sensible output for a good host, a bad name, and a good
   host with a closed port.

## 7. Interview questions

**Q: Walk me through what happens when a client connects to a server by hostname.**
A: DNS resolution via `getaddrinfo` (cache, then recursive resolver, then root/TLD/authoritative if
not cached). Routing decision: on-link or via default gateway. ARP to find the next hop's MAC.
TCP SYN inside IP inside Ethernet, forwarded hop by hop with TTL decremented. Server kernel
replies SYN-ACK, client ACKs, connection enters the accept queue, `accept()` returns a socket.
Only then does application data flow.

**Q: What identifies a TCP connection?**
A: The 4-tuple of source IP, source port, destination IP, destination port. A server with one
listening port can serve many clients because the client side of the tuple differs. Ephemeral
port exhaustion is a client-side problem for exactly this reason.

**Q: What is MTU and why do you care?**
A: The largest packet a link carries, 1500 bytes on Ethernet. TCP derives MSS (1460) from it.
If a path has a smaller MTU and ICMP is blocked, large packets are silently dropped: small
requests work, large ones hang. The fix is MSS clamping or lowering the interface MTU.

**Q: Why can a server see one client IP for a whole office?**
A: NAT. The office router rewrites all outgoing source addresses to its own public IP and
different ports. The server cannot distinguish clients by IP; use an application-level identity.

**Q: What does a large Recv-Q on a server socket mean?**
A: Bytes have arrived and the kernel has buffered them, but the application has not called `read`.
The application is the bottleneck, not the network. Once the receive buffer fills, TCP flow
control shrinks the window and the sender slows down.

**Q: Difference between TCP and UDP in one breath?**
A: TCP: connection-oriented, reliable, ordered byte stream, with flow and congestion control, at
the cost of handshake latency and head-of-line blocking. UDP: connectionless datagrams, no
guarantees, no connection state, lowest latency; the application must handle loss and order.

## 8. Recap

- Four layers: link (one hop, MAC), IP (addresses, routing, best effort), transport (ports; TCP
  reliable stream, UDP datagrams), application (your bytes).
- A TCP connection is a 4-tuple. Ephemeral ports on the client side are a finite resource.
- MTU 1500, MSS 1460. Broken path MTU discovery shows as "small works, large hangs".
- NAT hides clients behind one IP and drops idle mappings; keepalive helps.
- DNS: cache, recursive resolver, root, TLD, authoritative. Respect TTL; re-resolve on reconnect.
- Connecting: DNS, route lookup, ARP, SYN/SYN-ACK/ACK, accept queue, then application data.
- Tools: `ip`, `ss -tanp`, `dig`, `tcpdump`, Wireshark Follow TCP Stream. Debug one layer at a time.
- The kernel owns the stack; your process sees only the byte stream through socket buffers.
