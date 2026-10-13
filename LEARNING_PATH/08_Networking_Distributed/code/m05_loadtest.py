#!/usr/bin/env python3
"""m05_loadtest.py - load-test client for the tcpkv protocol.

Usage:
    python3 m05_loadtest.py --host 127.0.0.1 --port 7000 -c 50 -n 2000 --value-size 64
    python3 m05_loadtest.py --port 7000 -c 200 -n 500 --pipeline 32
    python3 m05_loadtest.py --port 7000 --echo -c 10 -n 10000      # against the echo servers

Opens `-c` connections (one thread each). Each connection runs `-n` rounds of SET then GET and
checks that GET returns what SET stored. Reports throughput and latency percentiles.

The reply parser is incremental on purpose: one recv() may return half a reply or three replies.
If you ever write a client that assumes otherwise, this file is your counter-example.
"""
import argparse
import socket
import statistics
import sys
import threading
import time


class ReplyParser:
    """Incremental parser for tcpkv replies: +simple, -error, :int, $bulk/$-1."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf += data

    def next(self):
        """Return a parsed reply or None if more bytes are needed."""
        nl = self.buf.find(b"\r\n")
        if nl < 0:
            return None
        line = bytes(self.buf[:nl])
        t = line[:1]
        if t in (b"+", b"-", b":"):
            del self.buf[: nl + 2]
            if t == b":":
                return ("int", int(line[1:]))
            return ("ok" if t == b"+" else "err", line[1:].decode(errors="replace"))
        if t == b"$":
            n = int(line[1:])
            if n == -1:
                del self.buf[: nl + 2]
                return ("nil", None)
            total = nl + 2 + n + 2
            if len(self.buf) < total:
                return None  # payload not fully here yet
            payload = bytes(self.buf[nl + 2 : nl + 2 + n])
            if self.buf[nl + 2 + n : total] != b"\r\n":
                raise ValueError("bulk reply not CRLF-terminated")
            del self.buf[:total]
            return ("bulk", payload)
        raise ValueError("bad reply type byte: %r (line=%r)" % (t, line[:40]))


def recv_replies(sock, parser, count):
    out = []
    while len(out) < count:
        r = parser.next()
        if r is not None:
            out.append(r)
            continue
        data = sock.recv(65536)
        if not data:
            raise ConnectionError("server closed connection")
        parser.feed(data)
    return out


def worker(args, idx, results, errors):
    try:
        s = socket.create_connection((args.host, args.port), timeout=10)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as e:
        errors.append("conn %d: connect failed: %s" % (idx, e))
        return
    parser = ReplyParser()
    lat = []
    value = (b"v%06d-" % idx) + b"x" * max(0, args.value_size - 8)
    try:
        rounds = args.requests
        p = max(1, args.pipeline)
        done = 0
        while done < rounds:
            batch = min(p, rounds - done)
            t0 = time.perf_counter()
            if args.echo:
                msg = b"".join(b"line %d %d\n" % (idx, done + i) for i in range(batch))
                s.sendall(msg)
                got = b""
                while len(got) < len(msg):
                    chunk = s.recv(65536)
                    if not chunk:
                        raise ConnectionError("closed")
                    got += chunk
                if got != msg:
                    raise ValueError("echo mismatch")
            else:
                req = bytearray()
                for i in range(batch):
                    key = b"k%d-%d" % (idx, (done + i) % 1000)
                    req += b"SET " + key + b" " + str(len(value)).encode() + b"\r\n" + value + b"\r\n"
                    req += b"GET " + key + b"\r\n"
                s.sendall(req)
                replies = recv_replies(s, parser, batch * 2)
                for i in range(batch):
                    if replies[2 * i][0] != "ok":
                        raise ValueError("SET reply: %r" % (replies[2 * i],))
                    if replies[2 * i + 1] != ("bulk", value):
                        raise ValueError("GET returned %r" % (replies[2 * i + 1][:1],))
            lat.append((time.perf_counter() - t0) / batch)
            done += batch
        if not args.echo:
            s.sendall(b"QUIT\r\n")
            recv_replies(s, parser, 1)
        s.shutdown(socket.SHUT_WR)
        s.close()
    except Exception as e:  # noqa: BLE001 - report everything, the point is to see failures
        errors.append("conn %d: %s: %s" % (idx, type(e).__name__, e))
    results.append(lat)


def percentile(sorted_vals, pct):
    if not sorted_vals:
        return float("nan")
    k = int(round((len(sorted_vals) - 1) * pct / 100.0))
    return sorted_vals[k]


def main():
    ap = argparse.ArgumentParser(description="tcpkv load tester")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7000)
    ap.add_argument("-c", "--connections", type=int, default=10)
    ap.add_argument("-n", "--requests", type=int, default=1000, help="SET+GET rounds per connection")
    ap.add_argument("--value-size", type=int, default=32)
    ap.add_argument("--pipeline", type=int, default=1, help="rounds to send before reading replies")
    ap.add_argument("--echo", action="store_true", help="talk to an echo server instead of tcpkv")
    args = ap.parse_args()

    results, errors = [], []
    threads = [threading.Thread(target=worker, args=(args, i, results, errors)) for i in range(args.connections)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0

    lat = sorted(x for r in results for x in r)
    ops_per_round = 1 if args.echo else 2
    total_ops = len(lat) * ops_per_round
    print("connections : %d (%d failed)" % (args.connections, len(errors)))
    print("rounds      : %d  (%d protocol ops)" % (len(lat), total_ops))
    print("elapsed     : %.2f s" % elapsed)
    if lat:
        print("throughput  : %.0f ops/s" % (total_ops / elapsed))
        print("latency/op  : p50 %.3f ms  p90 %.3f ms  p99 %.3f ms  max %.3f ms  mean %.3f ms"
              % (percentile(lat, 50) * 1e3, percentile(lat, 90) * 1e3, percentile(lat, 99) * 1e3,
                 lat[-1] * 1e3, statistics.fmean(lat) * 1e3))
    for e in errors[:10]:
        print("ERROR:", e, file=sys.stderr)
    if len(errors) > 10:
        print("... %d more errors" % (len(errors) - 10), file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
