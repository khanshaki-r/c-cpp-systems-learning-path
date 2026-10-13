#!/usr/bin/env python3
"""tcpkv_client.py - a Python client for tcpkv: one-shot commands, a self-test, a latency benchmark.

Module 10 of 06_Linux_Toolchain_DevOps. Standard library only (no pip install needed).

    python3 scripts/tcpkv_client.py PING
    python3 scripts/tcpkv_client.py SET greeting hello world
    python3 scripts/tcpkv_client.py GET greeting
    python3 scripts/tcpkv_client.py --port 7123 --wait 5 --selftest     # used by run_tests.sh and ctest
    python3 scripts/tcpkv_client.py --bench 20000 --value-size 100       # p50/p95/p99 round-trip latency
    python3 scripts/tcpkv_client.py --repl                               # interactive, like nc but checked

Exit status: 0 success, 1 a check failed, 2 usage or connection error.
Why Python and not bash here: real assertions, per-request latency, threads, structured output.
"""

from __future__ import annotations

import argparse
import socket
import statistics
import sys
import threading
import time
from typing import List, Optional

MAX_KEY = 256          # mirrors kMaxKeyLength in include/tcpkv/protocol.hpp
MAX_VALUE = 64 * 1024  # mirrors kMaxValueLength


class TcpkvError(Exception):
    """Protocol-level surprise: unexpected reply, closed connection, timeout."""


class TcpkvClient:
    """One TCP connection to tcpkv. Line in, line out; replies end with '\\n'."""

    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        # create_connection resolves the name, tries each address, and applies the timeout
        # to connect() and to every later recv()/send(). No timeout = a hung server hangs us.
        self.sock = socket.create_connection((host, port), timeout=timeout)
        # Small request/response messages: disable Nagle so each request leaves immediately.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # A buffered reader over the socket gives readline(), which handles a reply split
        # across several TCP segments (TCP is a byte stream, not a message stream).
        self.reader = self.sock.makefile("rb")

    # -- context manager: `with TcpkvClient(...) as c:` always closes the socket
    def __enter__(self) -> "TcpkvClient":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        try:
            self.reader.close()
        finally:
            self.sock.close()

    # -- raw protocol
    def send_line(self, line: str) -> None:
        self.sock.sendall(line.encode() + b"\n")   # sendall loops over partial sends for us

    def read_reply(self) -> str:
        raw = self.reader.readline(MAX_VALUE + 64)
        if not raw:
            raise TcpkvError("server closed the connection")
        if not raw.endswith(b"\n"):
            raise TcpkvError(f"reply too long or truncated: {raw[:60]!r}")
        return raw[:-1].decode(errors="replace")

    def request(self, line: str) -> str:
        self.send_line(line)
        return self.read_reply()

    # -- typed helpers
    def ping(self) -> bool:
        return self.request("PING") == "PONG"

    def get(self, key: str) -> Optional[str]:
        reply = self.request(f"GET {key}")
        if reply == "NOT_FOUND":
            return None
        if reply.startswith("VALUE "):
            return reply[len("VALUE "):]
        raise TcpkvError(f"GET {key}: unexpected reply {reply!r}")

    def set(self, key: str, value: str) -> None:
        reply = self.request(f"SET {key} {value}")
        if reply != "OK":
            raise TcpkvError(f"SET {key}: unexpected reply {reply!r}")

    def delete(self, key: str) -> bool:
        reply = self.request(f"DEL {key}")
        if reply not in ("OK", "NOT_FOUND"):
            raise TcpkvError(f"DEL {key}: unexpected reply {reply!r}")
        return reply == "OK"


def wait_for_server(host: str, port: int, seconds: float) -> None:
    """Poll until the port accepts a connection and answers PING, or give up."""
    deadline = time.monotonic() + seconds
    last: Exception = TimeoutError("never tried")
    while time.monotonic() < deadline:
        try:
            with TcpkvClient(host, port, timeout=1.0) as c:
                if c.ping():
                    return
        except (OSError, TcpkvError) as e:   # ECONNREFUSED while the server is still starting
            last = e
        time.sleep(0.1)
    raise TcpkvError(f"server on {host}:{port} not ready after {seconds}s: {last}")


# ------------------------------------------------------------------ self-test
class Checker:
    def __init__(self) -> None:
        self.passed = 0
        self.failed: List[str] = []

    def check(self, name: str, cond: bool, detail: str = "") -> None:
        if cond:
            self.passed += 1
            print(f"  ok    {name}")
        else:
            self.failed.append(name)
            print(f"  FAIL  {name} {detail}")


def selftest(host: str, port: int, timeout: float) -> int:
    ck = Checker()
    prefix = f"selftest_{int(time.time() * 1000)}_"   # unique keys: safe against a shared server

    with TcpkvClient(host, port, timeout) as c:
        print("basic commands")
        ck.check("PING -> PONG", c.ping())
        k = prefix + "a"
        ck.check("GET missing -> NOT_FOUND", c.get(k) is None)
        c.set(k, "1")
        ck.check("SET then GET", c.get(k) == "1")
        c.set(k, "hello world  with   spaces")
        ck.check("value keeps internal spaces", c.get(k) == "hello world  with   spaces")
        ck.check("DEL existing -> OK", c.delete(k))
        ck.check("DEL missing -> NOT_FOUND", not c.delete(k))
        ck.check("GET after DEL -> NOT_FOUND", c.get(k) is None)

        print("protocol errors")
        ck.check("unknown verb -> ERROR", c.request("FROB x").startswith("ERROR"))
        ck.check("GET without key -> ERROR", c.request("GET").startswith("ERROR"))
        ck.check("GET with two args -> ERROR", c.request("GET a b").startswith("ERROR"))
        ck.check("PING with args -> ERROR", c.request("PING x").startswith("ERROR"))
        ck.check("key too long -> ERROR", c.request("GET " + "k" * (MAX_KEY + 1)).startswith("ERROR"))
        ck.check("connection survives errors", c.ping())

        print("framing")
        c.sock.sendall(b"PING\r\n")                    # CRLF from Windows clients is tolerated
        ck.check("CRLF line ending", c.read_reply() == "PONG")
        big = "v" * MAX_VALUE
        c.set(prefix + "big", big)
        ck.check("64 KiB value round-trips", c.get(prefix + "big") == big)
        c.delete(prefix + "big")
        # Pipelining: three requests in one send(); replies must come back in order.
        c.sock.sendall(f"SET {prefix}p 7\nGET {prefix}p\nDEL {prefix}p\n".encode())
        replies = [c.read_reply() for _ in range(3)]
        ck.check("pipelined requests answered in order", replies == ["OK", "VALUE 7", "OK"], str(replies))
        # A request split across two sends must still parse (server buffers partial lines).
        c.sock.sendall(b"PI")
        time.sleep(0.05)
        c.sock.sendall(b"NG\n")
        ck.check("request split across segments", c.read_reply() == "PONG")

    print("concurrency")
    errors: List[str] = []

    def worker(n: int) -> None:
        try:
            with TcpkvClient(host, port, timeout) as wc:
                for i in range(200):
                    key = f"{prefix}t{n}_{i}"
                    wc.set(key, str(i))
                    if wc.get(key) != str(i):
                        errors.append(f"thread {n} key {key}")
                    wc.delete(key)
        except (OSError, TcpkvError) as e:
            errors.append(f"thread {n}: {e}")

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ck.check("8 clients x 200 SET/GET/DEL", not errors, "; ".join(errors[:3]))

    print("QUIT")
    with TcpkvClient(host, port, timeout) as c:
        c.send_line("QUIT")
        ck.check("QUIT closes the connection", c.reader.readline() == b"")

    print(f"\n{ck.passed} passed, {len(ck.failed)} failed")
    return 0 if not ck.failed else 1


# ------------------------------------------------------------------ benchmark
def bench(host: str, port: int, timeout: float, n: int, value_size: int) -> int:
    """Sequential round trips on one connection: measures latency, not peak throughput."""
    value = "x" * value_size
    lat_us: List[float] = []
    with TcpkvClient(host, port, timeout) as c:
        for i in range(n):
            key = f"bench_{i % 1000}"
            t0 = time.perf_counter_ns()
            if i % 3 == 0:
                c.set(key, value)
            else:
                c.get(key)
            lat_us.append((time.perf_counter_ns() - t0) / 1000.0)
    lat_us.sort()

    def pct(p: float) -> float:
        return lat_us[min(len(lat_us) - 1, int(p / 100.0 * len(lat_us)))]

    total_s = sum(lat_us) / 1e6
    print(f"requests={n} value={value_size}B total={total_s:.3f}s rate={n / total_s:.0f} req/s")
    print(f"latency us: min={lat_us[0]:.0f} p50={pct(50):.0f} p95={pct(95):.0f} "
          f"p99={pct(99):.0f} max={lat_us[-1]:.0f} mean={statistics.fmean(lat_us):.0f}")
    return 0


# ------------------------------------------------------------------ REPL
def repl(host: str, port: int, timeout: float) -> int:
    with TcpkvClient(host, port, timeout) as c:
        print(f"connected to {host}:{port}; Ctrl-D to exit")
        for line in sys.stdin:
            line = line.rstrip("\n")
            if not line:
                continue
            c.send_line(line)
            if line == "QUIT":
                return 0
            print(c.read_reply())
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="tcpkv client")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7000)
    ap.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    ap.add_argument("--wait", type=float, default=0.0, metavar="SECONDS",
                    help="wait up to SECONDS for the server to answer PING first")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--selftest", action="store_true", help="run the protocol self-test")
    mode.add_argument("--bench", type=int, metavar="N", help="N sequential requests, report latency")
    mode.add_argument("--repl", action="store_true", help="interactive session")
    ap.add_argument("--value-size", type=int, default=16, help="bench value size in bytes")
    ap.add_argument("command", nargs="*", help="one command, e.g. SET k v")
    args = ap.parse_args(argv)

    try:
        if args.wait > 0:
            wait_for_server(args.host, args.port, args.wait)
        if args.selftest:
            return selftest(args.host, args.port, args.timeout)
        if args.bench:
            return bench(args.host, args.port, args.timeout, args.bench, args.value_size)
        if args.repl:
            return repl(args.host, args.port, args.timeout)
        if not args.command:
            ap.print_usage(sys.stderr)
            return 2
        with TcpkvClient(args.host, args.port, args.timeout) as c:
            print(c.request(" ".join(args.command)))
        return 0
    except (OSError, TcpkvError) as e:   # ConnectionRefusedError and socket.timeout are OSErrors
        print(f"tcpkv_client: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
