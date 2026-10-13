# 13. Caching and Redis

**Time:** ~2 hours   **Prerequisites:** 05, 07

## 1. In plain words

A cache is a copy of data kept somewhere faster than its source, on the bet that it will be
asked for again before it changes. The database already has caches (shared buffers, the OS page
cache), but they still cost a network round trip, a parse, a plan, and a join. An application
cache answers from memory in microseconds and costs no database work at all. The price is the
oldest problem in computing: the copy can be wrong. "There are only two hard things in computer
science: cache invalidation and naming things."

The analogy: a clerk keeps a photocopy of the branch list and the product rate sheet on his
desk instead of walking to the filing room for every form. Fast, until head office changes a rate
and his copy is stale. Every caching design is a decision about how the clerk learns the copy is
stale, how long he is allowed to use it anyway, and what happens when fifty clerks all discover
at 9:00 that their copies expired and walk to the filing room at once.

Redis is the standard shared cache: an in-memory data-structure server on a socket, used by every
process in a cluster as one cache. This module covers the patterns, the failure modes, Redis's
model and persistence, and what your "in-memory caching removing master-table joins" design looks
like when done carefully.

## 2. The real thing

### 2.1 Cache patterns

| Pattern | Read | Write | Consistency | Use |
|---|---|---|---|---|
| **Cache-aside (lazy)** | app checks cache; on miss reads DB and writes cache | app writes DB, then **deletes** (not updates) the cache key | stale until TTL or delete; race windows exist | default for read-heavy lookups |
| **Read-through** | cache library loads from DB on miss itself | same as aside | same | when a cache layer wraps the data access |
| **Write-through** | from cache | app writes cache and DB synchronously (cache library forwards) | cache always has latest written value; DB write still the slow path | when reads must see writes immediately and the cache is the system of access |
| **Write-behind (write-back)** | from cache | app writes cache; cache flushes to DB asynchronously, batched | DB lags; a cache crash loses writes | counters, metrics, anything tolerable to loss; never money |
| **Refresh-ahead** | from cache | background refresh before TTL expires for hot keys | bounded staleness | hot reference data |

Why **delete, not update**, on write in cache-aside: two concurrent writers can update the cache
in the opposite order to the DB (A writes DB v1, B writes DB v2, B updates cache v2, A updates
cache v1: stale forever). Deleting makes the next reader fetch the truth. The remaining race:
reader misses, reads DB (old), writer writes DB and deletes cache, reader writes old value to
cache. It is narrow (the reader's DB read must straddle the write) and bounded by TTL; if that is
not acceptable, use a short TTL plus versioning (below), or lease-based sets (`SET key value NX`
with a lease token).

### 2.2 TTL and expiry

Every cached entry gets a time to live. Short TTL (seconds) bounds staleness for data that
changes; long TTL (hours) for reference data with explicit invalidation. Add **jitter** (TTL ±
10%) so entries loaded together do not expire together. Redis expires keys lazily on access and
actively by sampling (default 10 times per second, 20 random keys with TTL each, repeat while
more than 25% expired), so memory from expired keys is reclaimed within seconds, not instantly.
With `maxmemory` set, an eviction policy applies when memory is full: `allkeys-lru` (approximate
LRU via sampling), `volatile-lru` (only keys with TTL), `allkeys-lfu` (frequency), `volatile-ttl`,
`noeviction` (errors on write; the default, and wrong for a cache).

### 2.3 Invalidation strategies

1. **TTL only**: simplest; staleness up to TTL. Fine for data where a few seconds of lag is
   invisible (counts, lists).
2. **Delete on write** (cache-aside): the writer knows the key. Fails when one DB change affects
   many keys (a product rate change invalidates every cached quote).
3. **Versioned keys / namespaces**: cache `product:v{version}:{id}` where `version` is a small
   value stored in the cache (`product_version`); bump the version to invalidate the whole family
   at once; old keys expire by TTL. No scanning of keys.
4. **Event-driven**: the writer publishes "product 42 changed" (Redis Pub/Sub, a queue, or
   PostgreSQL `NOTIFY`), every cache holder deletes locally. Needed for in-process caches across
   many servers.
5. **Database-driven**: a trigger writes to an `outbox` table or increments a `cache_versions`
   row; cache holders poll it (cheap: one indexed row read per second) or get a `NOTIFY`. Catches
   writes that bypass the application (DBA scripts, batch jobs), which pattern 2 cannot.

Rule: invalidate on the *data*, not on the *query*. Cache entities by id; compose results in the
application; do not cache a 5-table join result keyed by its SQL text unless you can enumerate
what changes it.

### 2.4 Cache stampede (thundering herd, dog-pile)

A hot key expires. 500 concurrent requests miss at once and all run the expensive query; the
database, which was idle because of the cache, gets 500 copies of its heaviest query in 10 ms.
Defences, combinable:

- **Request coalescing (single flight)**: in-process, the first miss for a key does the load;
  others wait on it (a mutex or future per key). Across processes: a Redis lock
  `SET lock:key token NX PX 2000`; the winner loads and sets, losers either wait-and-retry or serve
  stale.
- **Serve stale while revalidating**: keep the value past its logical TTL (two TTLs: soft and
  hard); on soft expiry one request refreshes while others use the old value.
- **Probabilistic early expiration** (XFetch): each reader recomputes early with probability
  rising as expiry approaches (`now - delta * beta * ln(rand()) >= expiry`), so refreshes spread
  out and the herd never forms.
- **TTL jitter** against synchronised expiry of many keys.
- **Warm-up** on deploy: load the hot reference set before taking traffic, and never flush the
  whole cache in production ("FLUSHALL during the day" is a classic outage).

### 2.5 Consistency problems you must name

- **Read-your-writes**: a user updates her address, the next screen shows the old one from
  cache. Fix: delete on write, and for the writing session bypass the cache or write-through the
  specific entity.
- **Cross-entity consistency**: cache holds customer v2 and application v1 which referenced
  customer v1; a screen shows an impossible combination. Caches hold entities, not snapshots;
  if the UI needs a consistent set, read it from the database in one transaction.
- **Negative caching**: caching "not found" prevents repeated DB misses for missing keys
  (lookup of a non-existent user code in a brute-force login attack) but delays the appearance of
  newly created entities; use a short TTL for negatives.
- **Cache as a source of truth** by accident: a counter lives only in Redis; Redis restarts;
  the counter is gone. Anything you cannot rebuild from the database is not a cache.

### 2.6 In-process caches versus Redis

| | In-process (a hash map with TTL in the C server) | Redis |
|---|---|---|
| Latency | ~100 ns (a hash lookup) | ~100 to 300 us (socket round trip on the same host or LAN) |
| Shared across servers | no: each process has its own copy, each warms separately, invalidation must be broadcast | yes: one copy, one invalidation |
| Memory | per process, counts against the server's RAM | separate service with its own limit and eviction |
| Failure modes | process restart = cold cache | Redis down = every read goes to the DB (must degrade gracefully, with a circuit breaker) |
| Data structures | whatever you code | strings, hashes, lists, sets, sorted sets, streams, bitmaps, HyperLogLog, geo |
| Atomic operations across clients | not applicable | INCR, SETNX, Lua scripts, MULTI/EXEC, WATCH |
| Right for | small hot reference data (branches, products, roles, states: KBs to a few MB), per-request memoisation | sessions, rate limits, shared counters, leaderboards, large hot sets, anything many processes must agree on |

Common and correct: both. In-process L1 for reference data with event invalidation; Redis L2
for shared session and per-user state.

### 2.7 Redis: the model

Redis executes commands on a **single thread** over an event loop (`epoll`), one command at a
time, each in microseconds, so there are no locks and every command is atomic. Since 6.0,
**I/O threads** can read requests and write replies in parallel, but command execution is still
one thread. Consequences: a single slow command (`KEYS *`, `SMEMBERS` of a million-member set,
a long Lua script, a huge `DEL`) blocks every client; use `SCAN`, `UNLINK` (async free), and keep
values small. Throughput is ~100k to over 1 M simple ops per second per instance (Redis's own
benchmarks; depends on hardware and pipelining); scale by sharding (Redis Cluster, 16384 hash
slots) rather than threads.

**Data structures** (each with O(1) or O(log n) operations):

| Type | Commands | Cache use |
|---|---|---|
| String (bytes, up to 512 MB) | `GET SET SETEX SETNX INCR INCRBY MGET MSET` | serialised entities (JSON/protobuf), counters, locks |
| Hash | `HGET HSET HMGET HGETALL HINCRBY` | an entity's fields individually updatable; small hashes are memory-efficient (listpack) |
| List | `LPUSH RPUSH LPOP RPOP LRANGE BLPOP` | queues, recent-items |
| Set | `SADD SISMEMBER SMEMBERS SINTER` | membership (roles of a user), tags |
| Sorted set | `ZADD ZRANGE ZRANGEBYSCORE ZRANK ZINCRBY` | leaderboards, rate limiters (sliding window by timestamp), scheduled jobs |
| Stream | `XADD XREADGROUP XACK` | append-only log with consumer groups: event invalidation fan-out |
| Bitmap / HyperLogLog | `SETBIT BITCOUNT` / `PFADD PFCOUNT` | feature flags per user, approximate distinct counts |

**Pipelining**: send many commands without waiting for each reply; Redis processes them in order
and returns all replies. Removes the per-command round trip (the dominant cost): 10,000 `GET`s
pipelined take milliseconds instead of seconds. Not atomic. **MULTI/EXEC** queues commands and
executes them atomically (no interleaving) but without rollback; **WATCH** gives optimistic
check-and-set. **Lua scripts** (`EVAL`) run atomically on the server and are the way to express
"read, decide, write" safely.

### 2.8 Persistence: RDB and AOF

A cache does not strictly need persistence, but a cold restart of a large cache is itself an
outage (stampede), and Redis is often used for sessions and queues too.

- **RDB**: a point-in-time snapshot file (`dump.rdb`) written by a forked child process
  (`BGSAVE`), using the OS's copy-on-write so the parent keeps serving; triggered by
  `save <seconds> <changes>` rules. Compact, fast to load, loses everything since the last
  snapshot. The fork of a 30 GB instance can take hundreds of milliseconds and double memory
  under heavy writes (every page written by the parent is copied).
- **AOF** (append-only file): every write command appended to a log; `appendfsync always`
  (durable, slow), `everysec` (default when enabled: at most one second lost), `no` (OS decides).
  The file is rewritten compactly in the background (`BGREWRITEAOF`); since 7.0 as a multi-part
  base + incremental files. Slower restarts, better durability.
- Both can be on (recommended for anything that is not a pure cache); on restart AOF wins.
  Neither makes Redis a database with transactions and constraints; for money, write to
  PostgreSQL and cache in Redis.

**Replication** is asynchronous (replicas may lag; a failover can lose acknowledged writes);
**Sentinel** provides monitoring and automatic failover for a primary/replica set; **Redis
Cluster** shards keys over 16384 slots across primaries each with replicas, with multi-key
commands restricted to one slot (hash tags `{user:42}:...` to co-locate keys).

### 2.9 Redis as a lock, and why Redlock is debated

The simple lock: `SET lock:resource <random-token> NX PX 30000`. Acquire succeeds only if the key
did not exist; the TTL bounds the lock if the holder dies. Release with a Lua script that deletes
only if the value equals your token (so you never delete someone else's lock after your TTL
expired). This is fine for **efficiency** (avoid duplicate work, like stampede protection) and
wrong for **correctness** (mutual exclusion that money depends on), because:

1. The holder can be paused (GC, VM stall, network partition) past the TTL; the lock expires;
   another client acquires it; the first resumes and writes, believing it still holds the lock.
2. With a single Redis, the lock is lost on failover (asynchronous replication): two holders.

**Redlock** (Antirez's algorithm) acquires the lock on a majority of N independent Redis
instances within a time budget and uses the remaining validity time. Martin Kleppmann's 2016
critique: it still depends on bounded clock drift and on the holder not pausing, which a
distributed lock cannot assume, and it lacks a **fencing token**: a monotonically increasing
number issued with each lock grant that the protected resource (the database) checks, rejecting
writes carrying an older token. The practical conclusion: for correctness, use the database's
own locking (`SELECT ... FOR UPDATE`, advisory locks, unique constraints, or a `version` column
compare-and-set) and let the Redis lock be an optimisation; or use a consensus system (ZooKeeper,
etcd, or a Raft-backed store) that issues fencing tokens. Say both sides; interviewers want to
hear that you know the lock can be held twice.

### 2.10 Your design: in-process caching removing master-table joins

The login and transaction paths joined every request to master tables (`branches`, `loan_products`,
`roles`, `workflow_states`, `document_kinds`): small, almost static, read on every call. Removing
those joins by caching them in the C server is right, and here is the version that survives review:

- **What to cache**: tables under a few thousand rows that change by administrative action, never
  per transaction. Keep the join in SQL for anything else (customers, applications).
- **Shape**: per table, a hash map from natural key to a decoded struct, built at startup with one
  `SELECT` each (not per row), swapped atomically (build a new map, then replace the pointer under
  a mutex or with an atomic pointer; readers never see a half-built map; see 04_Concurrency for
  RCU-style swaps).
- **Invalidation**: a `cache_versions(table_name, version bigint)` row bumped by a trigger on
  each master table; the server polls it every second (one indexed read) or `LISTEN`s to a
  `NOTIFY master_changed` from the same trigger, and reloads only the changed table. TTL of a few
  minutes as a safety net. This catches DBA edits that bypass the application.
- **Consistency boundary**: the cached data is used for *authorisation and validation*
  decisions, so a stale copy for up to a second after an admin change is acceptable and
  documented; anything that must be exact at the instant (a product's rate at approval time) is
  read inside the approving transaction from the database, and the approved rate is stored on the
  application row.
- **Multi-server**: each server instance polls or listens independently; no cross-server
  coordination needed because the database is the source of truth.
- **Measurement**: `pg_stat_statements` before/after showed the master-table reads disappearing
  from the top of `calls`, and p50 latency of the affected RPCs dropping by the round trips saved
  (have your own numbers ready).
- **What Redis would add**: nothing for this data; it would reintroduce a network hop for data
  that fits in a few MB per process. Redis enters the design for sessions, rate limiting of login
  attempts (`INCR` + `EXPIRE` per user code), and anything shared across servers.

## 3. Code

`code/m13_cache_aside.py` (Python 3, `pip install redis psycopg[binary]`) implements cache-aside
with stampede protection against your WSL PostgreSQL and the docker Redis:

```python
def get_branch(code: str) -> dict | None:
    key = f"branch:v{VERSION}:{code}"
    raw = r.get(key)
    if raw is not None:
        return None if raw == b"__NONE__" else json.loads(raw)      # negative cache for missing codes
    lock = r.set(f"lock:{key}", TOKEN, nx=True, px=2000)             # single flight across processes
    if not lock:
        time.sleep(0.02)                                              # a loser: brief wait then retry (bounded)
        return get_branch(code) if _retries_left() else load_from_db(code)
    try:
        row = load_from_db(code)                                      # the expensive part, done once
        ttl = int(TTL * random.uniform(0.9, 1.1))                     # jitter against synchronised expiry
        r.set(key, json.dumps(row) if row else b"__NONE__", ex=ttl if row else 30)
        return row
    finally:
        release_if_owner(f"lock:{key}", TOKEN)                        # Lua: DEL only if value == TOKEN

def update_branch(code: str, **fields):
    with pg.transaction():
        pg.execute("UPDATE branches SET ... WHERE code = %s", ...)
    r.delete(f"branch:v{VERSION}:{code}")                             # delete, never set, after commit
```

`code/m13_redis_cli_demo.sh` shows the primitives from the shell: `SET NX PX`, the owner-checked
release script, pipelining with `--pipe`, `SCAN` versus `KEYS`, `INFO persistence`, and
a `MULTI`/`EXEC` counter with `WATCH`. The owner-checked release:

```lua
-- release.lua: atomic compare-and-delete; never delete a lock you no longer hold
if redis.call("GET", KEYS[1]) == ARGV[1] then
    return redis.call("DEL", KEYS[1])
else
    return 0
end
```

```bash
redis-cli SET lock:branch:BR0001 token123 NX PX 5000
redis-cli EVAL "$(cat release.lua)" 1 lock:branch:BR0001 token123
# pipelining: 100k SETs in one stream
seq 1 100000 | awk '{print "SET k:"$1" v"$1}' | redis-cli --pipe
```

And the PostgreSQL side of the invalidation signal used by the in-process cache:

```sql
CREATE TABLE cache_versions (table_name text PRIMARY KEY, version bigint NOT NULL DEFAULT 1, bumped_at timestamptz NOT NULL DEFAULT now());
CREATE OR REPLACE FUNCTION trg_bump_cache_version() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO cache_versions (table_name) VALUES (TG_TABLE_NAME)
    ON CONFLICT (table_name) DO UPDATE SET version = cache_versions.version + 1, bumped_at = now();
    PERFORM pg_notify('master_changed', TG_TABLE_NAME);              -- delivered at COMMIT, to LISTENing sessions
    RETURN NULL;
END $$;
CREATE TRIGGER branches_bump AFTER INSERT OR UPDATE OR DELETE ON branches
    FOR EACH STATEMENT EXECUTE FUNCTION trg_bump_cache_version();      -- statement-level: one bump per statement
```

## 4. Under the hood (senior-level)

**Why single-threaded is fast.** Every Redis operation touches a few cache lines in a hash table
(`dict`, incremental rehashing to avoid pauses) and the cost is dominated by the socket syscalls,
not the data structure. One thread with `epoll_wait` over thousands of sockets, batching reads and
writes per event-loop iteration, achieves high throughput without any locking or cache-line
contention between cores. The I/O threads in 6.0+ exist because `read()`/`write()` syscalls and
protocol parsing became the bottleneck, not the command execution. Memory allocation uses jemalloc
to limit fragmentation (`INFO memory` shows `mem_fragmentation_ratio`; above 1.5 is a problem).

**Encodings.** Small hashes, sets and sorted sets are stored as compact listpacks (contiguous
bytes, O(n) operations but n is small and cache-friendly) and converted to hash tables or skip
lists above `hash-max-listpack-entries` (128) etc. A hash with 100 fields costs far less memory
than 100 strings. Sorted sets are a skip list plus a hash (O(log n) by score, O(1) by member).

**Expiry and memory accounting.** Keys with TTL are also indexed in an expires dict. The active
expiry cycle runs in the `serverCron` timer and is bounded in CPU time per cycle; a huge number
of keys expiring in the same second causes expiry work to lag and memory to overshoot, which is
another reason for TTL jitter. Eviction under `maxmemory` samples `maxmemory-samples` (5) keys
and evicts the best candidate: approximate LRU/LFU, with an LRU clock per key of 24 bits.

**Fork and copy-on-write.** `BGSAVE` and AOF rewrite fork the process; the child walks the data
structures and writes the file while the parent serves. Linux shares pages until written; on a
write-heavy instance the parent dirties pages and the kernel copies them (4 KB each), so memory
grows during the snapshot (`INFO persistence: rdb_last_cow_size`). Transparent Huge Pages make
this far worse (2 MB copies); disable THP on Redis hosts. The fork itself must copy page tables,
which for a 50 GB instance is tens to hundreds of milliseconds of pause.

**Pub/Sub and Streams semantics.** Pub/Sub is fire-and-forget: a subscriber that is disconnected
misses messages, so it is fine for "please invalidate" (the subscriber reloads on reconnect
anyway) and wrong for anything that must be processed. Streams persist entries and track
consumer-group offsets (`XACK`, pending entries list), giving at-least-once delivery.

**PostgreSQL NOTIFY internals.** Notifications are queued in a shared-memory SLRU queue at
commit and delivered to listening backends, which forward them to clients between statements
(libpq `PQnotifies()` after `PQconsumeInput()`). Payload up to 8000 bytes; the queue has a size
limit (8 GB by default) and a listener that never reads blocks it for everyone (`pg_notification_queue_usage()`).
It is a notification, not a message bus: a disconnected listener misses events, which is why
the version row exists as the authoritative signal and NOTIFY is only the wake-up.

**Atomic pointer swap for the in-process cache.** Build the new map in the loader thread, then
`std::atomic<Map*>::store(new_map, std::memory_order_release)`; readers `load(acquire)` and use
the map for the duration of a request. Freeing the old map must wait until no reader uses it:
`shared_ptr` with `std::atomic_load/atomic_store` (or `std::atomic<std::shared_ptr>` in C++20)
gives this for free at the cost of a refcount increment per read; hazard pointers or epoch-based
reclamation avoid even that. See 04_Concurrency.

## 5. In production

- **Cache hit ratio is the first metric**: `INFO stats` `keyspace_hits / (hits + misses)`.
  Under 90% for a read cache means the TTL is too short, the key space too large for `maxmemory`,
  or the access pattern is not cacheable.
- **Big keys and hot keys**: `redis-cli --bigkeys`, `--hotkeys` (with LFU). A 50 MB hash read
  on every request saturates the network and blocks the event loop; split it or move it in-process.
- **Redis down must not take the application down.** Timeouts of tens of milliseconds, a circuit
  breaker that falls back to the database (and alerts, because the database is now taking the full
  load and may not survive it: capacity plan the database for a cache outage, or at least for a
  cache cold start with stampede protection).
- **Never `KEYS *` or `FLUSHALL` in production.** Rename or disable dangerous commands
  (`rename-command FLUSHALL ""`).
- **Memory limits**: set `maxmemory` and an eviction policy; without them Redis grows until the
  OOM killer picks it or the host swaps (and swapping Redis is a slow-motion outage).
- **Persistence on a cache host** can still be wanted to avoid a cold start; put RDB on a disk
  that can absorb the snapshot write, and watch `rdb_last_bgsave_status`.
- **Serialisation format matters**: JSON is convenient and slow; protobuf/msgpack halve the bytes
  and the CPU. Version the format in the key (`v2:`) so a deploy that changes the struct does not
  read garbage from old entries.
- **The login brute-force limiter**: `INCR login_fail:{branch}:{user}` + `EXPIRE 900` on failure,
  check before the DB lookup; this moved the lockout counter off the database row (which was a
  non-HOT update per failed attempt on an indexed column, module 03) into Redis, with the database
  remaining the record of the final lock state.
- **3 AM**: database CPU spikes every 15 minutes exactly. A batch of cache keys all have the same
  15-minute TTL and expire together; the herd rebuilds them. Add jitter and single-flight; the
  graph flattens.

## 6. Try it

1. **(easy)** Start Redis from `code/docker-compose.yml`, run `code/m13_redis_cli_demo.sh`.
   Done when you can explain each command's output, including why the second `SET NX` fails.
2. **(easy)** Run `code/m13_cache_aside.py` against the loan schema; call `get_branch` 1000
   times and read `INFO stats` hits/misses; update a branch and show the next read misses once.
   Done when hit ratio and the single invalidation are visible.
3. **(medium)** Simulate a stampede: delete a hot key and fire 200 concurrent `get_branch` calls
   (threads or `xargs -P`). Count DB queries with `pg_stat_statements` with and without the lock.
   Done when the locked version shows 1 (or a handful) of DB loads versus ~200.
4. **(medium)** Implement the `cache_versions` trigger and a `LISTEN master_changed` loop in a
   small libpq C program that reloads a branches map on notification. Update a branch from psql
   and watch the reload. Then kill and restart the listener and show the version row lets it
   detect what it missed. Done when both paths work.
5. **(hard)** Demonstrate the double-holder problem: two processes take the Redis lock with a 2 s
   TTL; pause the holder with `kill -STOP` for 3 s; the second acquires; `kill -CONT` the first and
   let it write. Then add a fencing token (a PostgreSQL sequence value taken with the lock and
   checked in the `UPDATE ... WHERE token >= $1`) and show the stale writer is rejected. Done when
   you can explain why the database check, not Redis, provided correctness.
6. **(hard)** Design the Redis layout for sessions (hash per session with TTL), per-user rate
   limiting (sorted set sliding window or fixed window counter), and event fan-out (stream with a
   consumer group per server) for the banking server. Write the commands and the failure analysis
   for Redis restart, replica failover, and a slow consumer. Done when each failure has a stated
   consequence and mitigation.

## 7. Interview questions

**Q: Explain cache-aside and its main race.**
A: On read, check the cache; on miss, read the database and populate the cache with a TTL. On
write, update the database and delete the cache key so the next reader fetches fresh data. The
race is a reader that read the old database value before a write, then populates the cache after
the writer's delete, leaving a stale entry until TTL; mitigations are short TTLs, versioned keys,
or lease-based population.

**Q: Why delete rather than update the cache on write?**
A: Two concurrent writers can apply their cache updates in the opposite order to their database
writes, leaving the cache permanently wrong. Deleting forces the next read to fetch the truth; it
costs one extra miss and is immune to that reordering.

**Q: What is a cache stampede and how do you prevent it?**
A: Many requests miss the same expired hot key simultaneously and all hit the database with the
same expensive query. Prevent it with single-flight (one loader per key, others wait or serve
stale, using an in-process mutex or a Redis SET NX lock), serving stale while revalidating,
probabilistic early refresh, TTL jitter, and warm-up before taking traffic.

**Q: How does Redis achieve high throughput with one thread?**
A: Every command is a few cache-line touches in a hash table; the cost is socket I/O, which an
epoll event loop batches across thousands of connections without locks or cross-core contention.
I/O threads since 6.0 parallelise reading and writing sockets; command execution stays single
threaded and atomic. The price is that one slow command blocks everyone, so you avoid KEYS,
huge collections and long scripts, and shard for more CPU.

**Q: RDB versus AOF?**
A: RDB is a forked point-in-time snapshot: compact, fast to load, loses everything since the last
save, and the fork costs memory via copy-on-write under writes. AOF logs every write with fsync
policies from always to every second: better durability, larger files, slower restart, periodic
rewrite. Use both for anything beyond a pure cache, and neither makes Redis a transactional
system of record.

**Q: Is a Redis lock safe?**
A: Safe for efficiency, not for correctness. A holder paused past the TTL or a failover of an
asynchronously replicated primary can produce two holders. Redlock reduces the single-node
problem but still assumes bounded clock drift and no long pauses and provides no fencing token.
For correctness, the protected resource must check a monotonically increasing token, or you use
the database's own locks and constraints.

**Q: Describe your in-memory caching of master tables.**
A: Small, administratively changed reference tables such as branches, products, roles and
workflow states were loaded once per server into hash maps keyed by natural key and swapped
atomically on reload, removing their joins from every request. Invalidation came from a version
row bumped by triggers and a NOTIFY wake-up, with a TTL safety net, so DBA edits were caught too.
Values that had to be exact at a decision point, like the rate at approval, were read inside the
transaction and stored on the row. Redis was reserved for shared state such as sessions and
login rate limits.

## 8. Recap

- Cache-aside: read-miss populate, write-then-delete; write-through and write-behind trade durability and latency differently.
- TTL with jitter; versioned key namespaces for mass invalidation; event or version-row driven invalidation catches out-of-band writes.
- Stampede defences: single-flight locks, stale-while-revalidate, probabilistic early expiry, warm-up.
- Caches hold entities, not snapshots; anything you cannot rebuild from the database is not a cache.
- In-process for small hot reference data; Redis for shared state across processes.
- Redis: single-threaded command execution over epoll, atomic commands, pipelining removes round trips, Lua for read-decide-write.
- Data structures: strings, hashes, lists, sets, sorted sets, streams; small collections use compact encodings.
- Persistence: RDB fork snapshot (COW cost), AOF with fsync policy; replication async; Sentinel and Cluster.
- Redis locks are for efficiency; correctness needs fencing tokens or database locks.
- Master-table cache: load whole tables, swap atomically, invalidate by version row + NOTIFY, read exact values in-transaction.
