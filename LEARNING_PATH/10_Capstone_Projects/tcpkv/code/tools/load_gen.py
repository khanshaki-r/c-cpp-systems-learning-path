#!/usr/bin/env python3
"""Concurrent load generator and correctness checker for tcpkv (Python edition).

Same contract as tools/load_gen.cpp: every client thread owns a disjoint key range and
verifies each GET against the last value it wrote. Exit code 1 on any error or mismatch.

    python3 tools/load_gen.py --port 7379 --clients 8 --duration 10

Python threads do not run in parallel (the GIL), so this tool measures the server less
precisely than the C++ one; it is handy because it needs no build and shows the protocol
is trivial to speak from another language.
"""

import argparse
import random
import socket
import sys
import threading
import time
from typing import List, Optional


class ProtocolError(Exception):
    pass


class Client:
    """Minimal blocking client: inline commands, length-prefixed SET, RESP-like replies."""

    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""

    def close(self) -> None:
        try:
            self.sock.sendall(b"QUIT\n")
            self._read_reply()
        except (OSError, ProtocolError):
            pass
        self.sock.close()

    def get(self, key: str) -> Optional[bytes]:
        self.sock.sendall(f"GET {key}\n".encode())
        kind, payload = self._read_reply()
        if kind == b"$":
            return payload
        if kind == b"nil":
            return None
        raise ProtocolError(f"unexpected reply to GET: {kind!r} {payload!r}")

    def set(self, key: str, value: bytes) -> None:
        self.sock.sendall(f"SET {key} {len(value)}\n".encode() + value + b"\n")
        kind, payload = self._read_reply()
        if kind != b"+" or payload != b"OK":
            raise ProtocolError(f"unexpected reply to SET: {kind!r} {payload!r}")

    # -- wire level ---------------------------------------------------------------------------

    def _fill(self, needed: int) -> None:
        while len(self.buffer) < needed:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ProtocolError("connection closed by server")
            self.buffer += chunk

    def _read_line(self) -> bytes:
        while True:
            index = self.buffer.find(b"\n")
            if index >= 0:
                line = self.buffer[:index]
                self.buffer = self.buffer[index + 1:]
                return line.rstrip(b"\r")
            self._fill(len(self.buffer) + 1)

    def _read_bulk(self, length: int) -> bytes:
        self._fill(length + 1)
        if self.buffer[length:length + 1] != b"\n":
            raise ProtocolError("bulk reply not terminated")
        value = self.buffer[:length]
        self.buffer = self.buffer[length + 1:]
        return value

    def _read_reply(self):
        line = self._read_line()
        if not line:
            raise ProtocolError("empty reply")
        kind, payload = line[:1], line[1:]
        if kind in (b"+", b"-", b":"):
            return kind, payload
        if kind == b"$":
            length = int(payload)
            if length < 0:
                return b"nil", b""
            return b"$", self._read_bulk(length)
        if kind == b"*":
            count = int(payload)
            items = []
            for _ in range(count):
                header = self._read_line()
                if not header.startswith(b"$"):
                    raise ProtocolError("array element is not bulk")
                items.append(self._read_bulk(int(header[1:])))
            return b"*", items
        raise ProtocolError(f"malformed reply: {line!r}")


class WorkerResult:
    def __init__(self) -> None:
        self.gets = 0
        self.sets = 0
        self.errors = 0
        self.mismatches = 0
        self.fatal = ""
        self.get_latencies: List[float] = []  # seconds
        self.set_latencies: List[float] = []


def make_value(worker: int, key: int, version: int, size: int) -> bytes:
    value = f"{worker}:{key}:{version}:".encode()
    if len(value) < size:
        value += b"x" * (size - len(value))
    return value


def run_worker(worker_id: int, run_id: int, args, deadline: float, result: WorkerResult) -> None:
    rng = random.Random(run_id ^ (worker_id * 0x9E3779B97F4A7C15))
    versions = [0] * args.keyspace
    prefix = f"lg:{run_id}:{worker_id}:"
    try:
        client = Client(args.host, args.port)
        while time.monotonic() < deadline:
            k = rng.randrange(args.keyspace)
            key = prefix + str(k)
            if rng.random() < args.read_ratio:
                t0 = time.perf_counter()
                value = client.get(key)
                result.get_latencies.append(time.perf_counter() - t0)
                result.gets += 1
                if versions[k] == 0:
                    if value is not None:
                        result.mismatches += 1
                elif value != make_value(worker_id, k, versions[k], args.value_size):
                    result.mismatches += 1
            else:
                version = versions[k] + 1
                value = make_value(worker_id, k, version, args.value_size)
                t0 = time.perf_counter()
                client.set(key, value)
                result.set_latencies.append(time.perf_counter() - t0)
                versions[k] = version
                result.sets += 1
        client.close()
    except Exception as exc:  # noqa: BLE001 - report anything that ends the worker
        result.errors += 1
        result.fatal = repr(exc)


def percentile_us(samples: List[float], p: float) -> float:
    if not samples:
        return 0.0
    ordered = sorted(samples)
    index = min(len(ordered) - 1, max(0, int(round(p * len(ordered))) - 1))
    return ordered[index] * 1e6


def print_latency(name: str, samples: List[float]) -> None:
    mean_us = (sum(samples) / len(samples) * 1e6) if samples else 0.0
    print(f"{name:<6} n={len(samples):<10} mean={mean_us:8.1f} us  "
          f"p50={percentile_us(samples, 0.50):7.0f} us  p99={percentile_us(samples, 0.99):7.0f} us  "
          f"p99.9={percentile_us(samples, 0.999):7.0f} us")


def main() -> int:
    parser = argparse.ArgumentParser(description="tcpkv load generator")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7379)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--keyspace", type=int, default=10000)
    parser.add_argument("--value-size", type=int, default=64)
    parser.add_argument("--read-ratio", type=float, default=0.8)
    args = parser.parse_args()

    run_id = int(time.time_ns()) ^ random.getrandbits(64)
    print(f"tcpkv load_gen.py: {args.clients} clients, {args.duration:.0f} s, keyspace {args.keyspace} "
          f"per client, value {args.value_size} B, read ratio {args.read_ratio:.2f}, "
          f"target {args.host}:{args.port}")

    results = [WorkerResult() for _ in range(args.clients)]
    start = time.monotonic()
    deadline = start + args.duration
    threads = [
        threading.Thread(target=run_worker, args=(i, run_id, args, deadline, results[i]), daemon=True)
        for i in range(args.clients)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - start

    gets = sum(r.gets for r in results)
    sets = sum(r.sets for r in results)
    errors = sum(r.errors for r in results)
    mismatches = sum(r.mismatches for r in results)
    get_latencies = [x for r in results for x in r.get_latencies]
    set_latencies = [x for r in results for x in r.set_latencies]
    for r in results:
        if r.fatal:
            print(f"worker error: {r.fatal}", file=sys.stderr)

    ops = gets + sets
    print(f"total  {ops} ops in {elapsed:.2f} s  ->  {ops / elapsed if elapsed > 0 else 0:.0f} ops/s")
    print_latency("GET", get_latencies)
    print_latency("SET", set_latencies)
    print(f"errors {errors}, mismatches {mismatches}")
    return 0 if errors == 0 and mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
