#!/usr/bin/env python3
"""m13_cache_aside.py : cache-aside over PostgreSQL with Redis, TTL jitter, negative caching and
stampede protection (module 13).

Setup on WSL Ubuntu 24.04:
    cd code && docker compose up -d redis
    python3 -m venv ~/venv-db && . ~/venv-db/bin/activate
    pip install "redis>=5" "psycopg[binary]>=3.1"

Run:
    python3 m13_cache_aside.py demo                 # hits/misses, then one invalidation
    python3 m13_cache_aside.py stampede 200         # 200 threads miss at once, WITH the lock
    python3 m13_cache_aside.py stampede 200 --no-lock   # the same without single-flight

Environment: PG_DSN (default "dbname=loans"), REDIS_URL (default "redis://localhost:6379/0").

The pattern in one paragraph: read the cache; on a miss, ONE caller (the holder of a short Redis
lock) loads from the database and fills the cache; the others wait briefly and re-read the cache.
Writers update the database, COMMIT, and then DELETE the key (never SET it: a delete cannot
install a stale value, a set racing with another writer can). TTLs are jittered so keys filled
together do not expire together. Missing rows are cached as a sentinel for a short time so a
flood of lookups for a non-existent code does not reach the database.
"""
from __future__ import annotations

import json
import os
import random
import sys
import threading
import time
import uuid
from decimal import Decimal

import psycopg
import redis

PG_DSN = os.environ.get("PG_DSN", "dbname=loans")
REDIS_URL = os.environ.get("REDIS_URL", "redis://localhost:6379/0")

VERSION = 1            # bump when the cached JSON shape changes: old keys are simply never read again
TTL = 300              # seconds, before jitter
NEGATIVE_TTL = 30      # seconds for "this code does not exist"
LOCK_MS = 2000         # lock lifetime: must exceed the slowest expected DB load
WAIT_S = 0.02          # how long a loser sleeps before re-reading the cache
MAX_WAITS = 50         # bounded: after ~1 s of waiting, go to the database anyway
NONE = b"__NONE__"     # negative-cache sentinel

r = redis.Redis.from_url(REDIS_URL)

# Atomic compare-and-delete: release the lock only if we still own it. A plain DEL could delete
# a lock that expired and was taken by someone else in the meantime.
RELEASE_LUA = """
if redis.call("GET", KEYS[1]) == ARGV[1] then
    return redis.call("DEL", KEYS[1])
else
    return 0
end
"""
release_script = r.register_script(RELEASE_LUA)

# One PostgreSQL connection per thread (psycopg connections are not for concurrent use).
_local = threading.local()
_stats_lock = threading.Lock()
stats = {"db_loads": 0, "cache_hits": 0, "cache_misses": 0, "lock_waits": 0}


def _bump(name: str, n: int = 1) -> None:
    with _stats_lock:
        stats[name] += n


def pg() -> psycopg.Connection:
    conn = getattr(_local, "conn", None)
    if conn is None or conn.closed:
        conn = psycopg.connect(PG_DSN, autocommit=True)
        conn.execute("SET search_path = loans, public")
        _local.conn = conn
    return conn


def _json_default(o):
    if isinstance(o, Decimal):
        return str(o)          # money stays exact: never float
    raise TypeError(type(o))


def cache_key(code: str) -> str:
    return f"branch:v{VERSION}:{code}"


def load_from_db(code: str) -> dict | None:
    """The expensive part. Counted, so the stampede demo can show how often it ran."""
    _bump("db_loads")
    time.sleep(0.05)           # pretend the real query is slow (a join, a cold page): widens the race
    row = pg().execute(
        "SELECT code, name, city, max_loan_amount, time_zone FROM branches WHERE code = %s", (code,)
    ).fetchone()
    if row is None:
        return None
    return {"code": row[0], "name": row[1], "city": row[2], "max_loan_amount": row[3], "time_zone": row[4]}


def get_branch(code: str, use_lock: bool = True) -> dict | None:
    key = cache_key(code)
    token = uuid.uuid4().hex
    for _ in range(MAX_WAITS):
        raw = r.get(key)
        if raw is not None:
            _bump("cache_hits")
            return None if raw == NONE else json.loads(raw)
        _bump("cache_misses")
        if not use_lock:
            break
        # Single flight across processes: SET NX PX. Only one caller gets True.
        if r.set(f"lock:{key}", token, nx=True, px=LOCK_MS):
            try:
                raw = r.get(key)               # double-check: someone may have filled it meanwhile
                if raw is not None:
                    return None if raw == NONE else json.loads(raw)
                row = load_from_db(code)
                if row is None:
                    r.set(key, NONE, ex=NEGATIVE_TTL)
                else:
                    ttl = int(TTL * random.uniform(0.9, 1.1))      # jitter against synchronised expiry
                    r.set(key, json.dumps(row, default=_json_default), ex=ttl)
                return row
            finally:
                release_script(keys=[f"lock:{key}"], args=[token])
        _bump("lock_waits")
        time.sleep(WAIT_S)                     # a loser: wait briefly, then re-read the cache
    # No lock wanted, or waited too long (the holder died or is very slow): degrade to the DB.
    row = load_from_db(code)
    if row is not None:
        r.set(key, json.dumps(row, default=_json_default), ex=int(TTL * random.uniform(0.9, 1.1)))
    return row


ALLOWED_FIELDS = {"name", "city", "max_loan_amount"}


def update_branch(code: str, **fields) -> None:
    bad = set(fields) - ALLOWED_FIELDS
    if bad:
        raise ValueError(f"not updatable: {bad}")
    sets = ", ".join(f"{f} = %s" for f in fields)          # names come from the whitelist only
    with pg().transaction():
        pg().execute(f"UPDATE branches SET {sets} WHERE code = %s", (*fields.values(), code))
    # AFTER the commit: delete, never set. If we deleted before committing, a reader could refill
    # the cache from the old row between our delete and our commit.
    r.delete(cache_key(code))


def demo() -> None:
    code = "BR0007"
    r.delete(cache_key(code))
    info0 = r.info("stats")
    t0 = time.perf_counter()
    for _ in range(1000):
        get_branch(code)
    dt = time.perf_counter() - t0
    info1 = r.info("stats")
    print(f"1000 reads in {dt * 1000:.0f} ms; app stats {stats}")
    print(f"redis keyspace_hits +{info1['keyspace_hits'] - info0['keyspace_hits']}, "
          f"misses +{info1['keyspace_misses'] - info0['keyspace_misses']}")
    print("ttl of the key:", r.ttl(cache_key(code)), "s (jittered around", TTL, ")")

    old = get_branch(code)
    update_branch(code, name=old["name"] + " (renamed)")
    loads_before = stats["db_loads"]
    print("after update:", get_branch(code)["name"], "| db loads for that read:", stats["db_loads"] - loads_before)
    update_branch(code, name=old["name"])             # put it back
    print("negative cache:", get_branch("BR9999"), "->", r.get(cache_key("BR9999")), "ttl", r.ttl(cache_key("BR9999")))


def stampede(n: int, use_lock: bool) -> None:
    code = "BR0042"
    r.delete(cache_key(code))
    pg()                                               # warm this thread's connection
    for k in stats:
        stats[k] = 0
    barrier = threading.Barrier(n)

    def worker() -> None:
        pg()                                           # connect before the barrier: measure the cache, not connects
        barrier.wait()
        get_branch(code, use_lock=use_lock)

    threads = [threading.Thread(target=worker) for _ in range(n)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print(f"{n} concurrent misses, lock={'on' if use_lock else 'off'}: "
          f"{stats['db_loads']} database loads, {stats['lock_waits']} lock waits, "
          f"{(time.perf_counter() - t0) * 1000:.0f} ms")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "demo":
        demo()
    elif len(sys.argv) >= 3 and sys.argv[1] == "stampede":
        stampede(int(sys.argv[2]), use_lock="--no-lock" not in sys.argv)
    else:
        print(__doc__)
        sys.exit(2)
