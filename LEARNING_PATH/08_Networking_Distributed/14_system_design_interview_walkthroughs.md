# 14. System design interview walkthroughs

**Time:** ~4 hours   **Prerequisites:** all of 01 to 13

## 1. In plain words

A **system design interview** is a 45 to 60 minute conversation where you design a service on a
whiteboard: "design a key-value store", "design a rate limiter". There is no single right answer.
The interviewer is checking whether you can turn vague requirements into concrete ones, pick
reasonable components, explain trade-offs with numbers, and go deep where it matters, all while
communicating clearly. For a C++ systems role, expect the deep dives to go lower than in a web
interview: wire protocols, threading models, storage engines, replication.

Analogy: an architect meeting a client. A good architect does not start drawing the facade. They
ask how many people will live there, the budget, the plot, and the must-haves, sketch the floor
plan, then go deep on the parts that are risky (the foundation on clay soil). They say "if you
want a bigger kitchen, we lose the study" out loud. That is the method below.

This module gives the method once, then five walkthroughs chosen for your profile: a key-value
store (your tcpkv project grown into a distributed system), a loan-origination backend (your
domain), a rate limiter, a URL shortener, and a notification queue system. Each ends with
pushback questions an interviewer is likely to ask, with short answers.

## 2. The real thing

### 2.1 The method (45 minutes)

| Minutes | Step | What you produce |
|---------|------|------------------|
| 0 to 5 | **Requirements** | Functional (the 3 to 5 operations that matter); non-functional (scale, latency, availability, consistency, durability, security); explicit out-of-scope list. |
| 5 to 10 | **Estimates** | Requests per second (average and peak), data size and growth, read/write ratio, bandwidth. Round numbers. |
| 10 to 15 | **API and data model** | The calls (RPC or REST) with arguments and results; the main entities and their keys. |
| 15 to 25 | **High-level design** | A box diagram: clients, load balancer, services, storage, caches, queues. Walk one request through it. |
| 25 to 40 | **Deep dives** | Two or three areas the interviewer cares about or that are risky: storage layout, sharding key, replication and failover, consistency, hot keys, idempotency, concurrency model. |
| 40 to 45 | **Wrap-up** | Bottlenecks, failure modes, monitoring, what you would do next with more time. |

Habits that score:
- **Ask before assuming**, but do not stall: "I'll assume 10,000 requests per second peak and 99.9%
  availability; tell me if that's off."
- **State trade-offs as sentences**: "Async replication gives lower latency but can lose the last
  few writes on failover; for balances I'll pay for synchronous quorum writes."
- **Use numbers you can defend**. Latency figures worth knowing (typical orders of magnitude, from
  the widely circulated "latency numbers every programmer should know" list and its updates):
  main memory reference ~100 ns; SSD random read ~tens to ~100 microseconds; round trip within a
  datacenter ~0.5 ms; fsync on SSD ~0.1 to a few ms; cross-continent round trip ~100 to 150 ms.
  1 day = 86,400 s, so 1 million per day is about 12 per second average; peaks are often 2 to 10
  times average.
- **Draw the request path** and narrate it.
- **Name the failure modes** before being asked: what happens when this box dies?
- **Go deep in your strengths**: sockets, framing, thread pools, epoll, PostgreSQL/YugabyteDB,
  C++ memory. Steer deep dives there when the interviewer gives you a choice.

Estimation template:

```
 users / clients          -> requests per day -> average RPS -> peak RPS (x3 to x10)
 bytes per record         -> records per day  -> storage per year (x replication factor)
 bytes per request        -> bandwidth = peak RPS x bytes
 working set to cache     -> often 20% of data serves 80% of reads (rule of thumb, verify)
```

### 2.2 Walkthrough 1: a key-value store, from tcpkv to distributed

**Prompt**: "Design a key-value store. Start with a single node and evolve it to survive node
failures and grow beyond one machine."

**Requirements**: `GET`, `SET`, `DEL` on string keys up to 256 bytes and values up to 1 MB. Single
node first. Then: 1 TB of data, 100,000 ops/s peak, 80% reads, p99 under 10 ms, survive one node
loss without losing acknowledged writes. Out of scope: secondary indexes, transactions across keys.

**Estimates**: 1 TB / 64 GB RAM per node: does not fit in one machine's memory, so disk-backed
storage or many nodes. 100,000 ops/s: a well-written epoll server does this on one node for small
values, but storage and replication spread it out. With RF3, 3 TB raw.

**Stage 1: single node (this is tcpkv)**

```
 clients --TCP, tcpkv text protocol--> [ acceptor | epoll loops or thread pool ] --> in-memory map
                                                                                     + append-only log
```

- Protocol: tcpkv from module 04, or a length-prefixed binary protocol for large values.
- Concurrency: module 05 thread pool (simple) or module 06 epoll loops (scales to many idle
  connections). Store: sharded `unordered_map`s each with its own `shared_mutex` to cut contention
  (see `04_Concurrency/04_shared_mutex_and_readers_writers.md`).
- Durability: append every write to a log file (`write` + `fdatasync`, group-committed every few ms)
  and replay on start; periodic snapshot to bound replay time. That is a WAL (see
  `07_Databases_Internals/04_wal_and_crash_recovery.md`).
- Beyond RAM: an LSM tree (memtable, immutable sorted files, compaction) like RocksDB, or a B-tree
  like Berkeley DB. Writes-heavy: LSM. Read-heavy with range scans: B-tree.

**Stage 2: replication for durability and failover**

```
            +-------------------------- Raft group (RF3) --------------------------+
 client --> | leader (node A) --AppendEntries--> follower (node B)                 |
            |                 --AppendEntries--> follower (node C)                 |
            +----------------------------------------------------------------------+
   SET returns OK after the entry is on 2 of 3 logs; GET served by leader under its lease
```

- Raft (module 11). The log *is* the WAL. Clients find the leader (any node redirects with
  "leader is A").
- Reads: from leader with lease (linearizable) or from followers (stale OK) per request flag.

**Stage 3: sharding for size and throughput**

```
                       +--------------------+
 client (smart, caches |  metadata service  |  (its own Raft group: shard map, placement)
 the shard map) -----> +--------------------+
     |
     | hash(key) -> shard 17 -> leader on node D
     v
 +--------+   +--------+   +--------+   +--------+   +--------+
 | node A |   | node B |   | node C |   | node D |   | node E |
 | s1 L   |   | s1 F   |   | s1 F   |   | s17 L  |   | s17 F  |  ... each shard a Raft group,
 | s17 F  |   | s4 L   |   | s4 F   |   | s4 F   |   | ...    |      leaders spread across nodes
 +--------+   +--------+   +--------+   +--------+   +--------+
```

- Fixed hash space split into shards (like YugabyteDB's 16-bit space or Redis Cluster's 16384
  slots), or consistent hashing (module 12) if you prefer Dynamo-style.
- Shards split when large; the metadata service moves replicas and leaders to balance.
- This is Multi-Raft: YugabyteDB, CockroachDB and TiKV in miniature.

**Alternative: leaderless (Dynamo/Cassandra)**: any replica takes writes, N=3, W=2, R=2, hinted
handoff, read repair, last-write-wins or vector clocks. Higher availability under partitions,
weaker consistency (module 10). Say which you pick and why: "For a general-purpose store with
strong consistency I pick Raft per shard; if the interviewer wants always-writable (shopping cart),
Dynamo style."

**Pushback questions**
- *"What if the leader acknowledges a write and then dies?"* It acknowledged only after a majority
  stored it; the next leader must have it (Raft's vote restriction). No loss.
- *"A client times out on SET. Did it happen?"* Unknown. Make SET idempotent (it is, for the same
  value) or add client request ids that the state machine deduplicates.
- *"One key gets 50% of traffic."* Cache it at the client or a proxy for reads (with a short TTL
  or invalidation); for writes, split the key (sub-keys merged on read) or accept that a shard
  leader's throughput is the ceiling.
- *"Why not just use consistent hashing without Raft?"* Then replicas can diverge on failure and
  you need conflict resolution; fine for caches, not for a source of truth.
- *"How do you handle a 1 MB value with your text protocol?"* Length-prefixed bulk strings (tcpkv's
  `$len`) so the parser never scans the value; stream it to disk rather than buffering when large.
- *"How many threads?"* One epoll loop per core with connections assigned round-robin; storage
  fsyncs on a dedicated thread with group commit; Raft per shard driven by the loop owning it.

### 2.3 Walkthrough 2: a loan-origination backend

**Prompt**: "Design the backend for a bank's loan-origination system: branch staff and customers
submit loan applications, documents are uploaded, credit checks run, approvers approve, the loan
is disbursed."

**Requirements**
- Functional: create application; attach documents; run checks (credit bureau, KYC, internal
  limits); multi-level approval workflow by amount; disburse to the core banking system; status
  tracking; audit trail of every change.
- Non-functional: 500 branches, about 20,000 applications per day at peak season (assumed), p99
  under 1 s for interactive screens, no lost applications, strict audit, data residency in the
  country, 99.9% during business hours, integrations that are slow and flaky (bureau API: seconds).
- Out of scope: credit scoring model internals, the core banking ledger itself.

**Estimates**: 20,000 / day is under 1 per second average; even 10x peak is tiny. **Scale is not the
problem; correctness, workflow, integrations and audit are.** Say that explicitly: it shows
judgement. Documents: 10 per application x 500 KB = 100 GB per year (assumed), so object storage,
not database rows.

**High-level design**

```
 branch UI / customer app
        |  HTTPS (REST/JSON) or RPC over TCP from branch terminals
        v
 +---------------+      +------------------------+
 | API gateway   |----->| Loan application svc   |----> YugabyteDB / PostgreSQL (RF3)
 | authn, rate   |      | state machine, rules,  |        applications, applicants, approvals,
 | limit, audit  |      | idempotency keys       |        outbox table, audit log
 +---------------+      +------------------------+
                            |  (same transaction writes an OUTBOX row)
                            v
                       outbox relay -> message queue (Kafka / RabbitMQ / DB-backed queue)
                            |
         +------------------+------------------+--------------------+
         v                  v                  v                    v
  credit-check worker  KYC worker       notification worker   disbursement worker
  (bureau API,         (KYC vendor)     (SMS/email)           (core banking RPC,
   retries, breaker)                                            idempotent by app no)
         |                  |                                        |
         +--- results posted back to the loan service (API or queue) +
 document upload: client -> pre-signed URL -> object storage (S3-compatible, encrypted);
                  DB stores only the reference + checksum
```

**Data model (core)**

```
 loan_application(application_no PK HASH, branch_code, applicant_id, product_code,
                  amount_minor, status, version, created_at, updated_at)
 application_event(application_no, seq, event_type, payload_json, actor, at)
                  PRIMARY KEY ((application_no) HASH, seq ASC)       -- audit + history
 approval(application_no, level, approver_id, decision, at)
 outbox(id, aggregate_id, event_type, payload, created_at, published_at NULL)
```

**Deep dives**
- **Application as a state machine**: `DRAFT -> SUBMITTED -> CHECKS_PENDING -> CHECKS_DONE ->
  PENDING_APPROVAL(L1/L2/L3) -> APPROVED -> DISBURSING -> DISBURSED`, plus `REJECTED`,
  `WITHDRAWN`. Every transition is a function that validates the current state; illegal transitions
  are rejected. Store the state plus an append-only event table for audit.
- **Optimistic concurrency**: two approvers act at once. `UPDATE ... SET status = $new, version =
  version + 1 WHERE application_no = $1 AND version = $expected`; zero rows updated means someone
  else acted first; reload and show.
- **Transactional outbox**: writing the DB row and publishing a message cannot be atomic across two
  systems (dual write problem). Write the event into an `outbox` table in the *same* transaction;
  a relay process publishes unsent outbox rows and marks them sent. At-least-once publish;
  consumers deduplicate by event id (module 10).
- **Slow, flaky integrations**: never call the credit bureau inside the user's request. Queue it;
  worker calls with timeout, retries with jittered backoff, circuit breaker, and a dead-letter
  queue after N failures for manual handling (module 09). The UI shows "checks pending".
- **Disbursement exactly once**: the disbursement RPC to core banking carries the application
  number as the idempotency key; core banking rejects duplicates. State `DISBURSING` is set before
  the call; a reconciler queries core banking for applications stuck in `DISBURSING` to resolve
  unknown outcomes.
- **Audit**: append-only event table, actor and timestamp on every change, no `UPDATE`/`DELETE`
  grants on it for the app user; optionally hash-chained rows for tamper evidence.
- **Database**: one YugabyteDB or PostgreSQL cluster is enough for the volume. RF3 for RPO 0.
  Shard keys per module 12 if distributed.

**Pushback questions**
- *"Why not microservices for everything?"* At this volume a modular monolith plus a few workers
  is simpler to operate; split out services where the change rate or scaling differs
  (integrations, notifications). Say what you would split first and why.
- *"The bureau is down for an hour."* Breaker opens, applications queue in `CHECKS_PENDING`, the
  UI says so, the queue drains when it recovers; alerts fire on queue age, not on each failure.
- *"A message is processed twice."* Consumers are idempotent: the check result is stored keyed by
  `(application_no, check_type, request_id)`; a duplicate is a no-op.
- *"How do you guarantee no lost applications?"* The create call is idempotent with a client
  generated key; the row is committed to an RF3 database before OK; the outbox ensures every
  committed state change produces its event.
- *"Approver limits differ by branch and product."* Rules in configuration tables, versioned, with
  the rule version recorded on each approval for audit.
- *"How would you migrate the existing system's in-flight applications?"* Bulk load plus delta,
  parity check, cutover window (module 13 vocabulary).

### 2.4 Walkthrough 3: a rate limiter

**Prompt**: "Design a rate limiter for an API used by many clients: 100 requests per second per API
key with bursts up to 200, across a fleet of 20 API servers."

**Requirements**: per-key limits (and per-IP for anonymous), configurable per plan; decision latency
under 1 ms added; correct within a small tolerance; must not become a single point of failure;
return `429` with `Retry-After`. Out of scope: billing.

**Estimates**: 1 million keys; 200,000 requests per second total across the fleet (assumed); state
per key ~ 16 bytes (tokens + timestamp) + key, so ~100 MB: fits in memory anywhere.

**Algorithms** (module 09): token bucket (burst + rate, the standard; `code/m09_token_bucket.cpp`),
leaky bucket (smooth output), fixed window (boundary burst problem), sliding window log (exact,
memory heavy), sliding window counter (weighted two windows, good approximation).

**Where to enforce**

```
 option A: local only                         option B: central store
 client -> LB -> [api-1: bucket per key]      client -> LB -> [api-1] --+
                 [api-2: bucket per key]                    [api-2] --+--> Redis (Lua script:
                 ...                                        ...       |     atomic token bucket)
 each server allows limit / 20                              [api-20]--+    per key, ~0.5 ms RTT

 option C: hybrid
 each server keeps a local bucket and periodically (every 100 ms) syncs usage with the central
 store, or leases a batch of tokens ("give me 50 tokens for key K") and spends them locally.
```

- **Local only**: zero latency, no dependency. Inaccurate when the LB does not spread one key's
  traffic evenly (sticky connections), and limits change with fleet size.
- **Central (Redis)**: accurate. Adds a network round trip and a dependency; Redis must be HA
  (replicas + Sentinel, or Redis Cluster sharded by key). Use a Lua script so read-modify-write
  is atomic.
- **Hybrid / token leasing**: near-local latency, near-global accuracy. The usual production answer
  for high volume.
- **Failure policy**: if the central store is unreachable, **fail open** (allow, with local
  fallback limits) for availability, or **fail closed** for protection of a fragile backend.
  State the choice per use case.

**Pushback questions**
- *"Clocks differ between servers; does your bucket break?"* Each bucket is refilled using the
  clock of whoever owns it: the Redis server's time (use `TIME` in the Lua script) or the local
  monotonic clock, never mixed wall clocks from different servers.
- *"Hot key: one customer sends 50,000 requests per second."* Local pre-check with a local bucket
  rejects most before touching Redis; only the residual hits the central store.
- *"How do clients know when to retry?"* `429` with `Retry-After` computed from the bucket's
  `time_until`; plus `RateLimit-Limit/Remaining/Reset` headers.
- *"Distributed limit across regions?"* Per-region budgets (split the global limit) with occasional
  rebalancing; exact global limits across regions cost a WAN round trip per request.
- *"Rate limiting vs load shedding?"* Rate limiting is per-client fairness by policy; load shedding
  protects the server by its own capacity, regardless of who is asking. You want both.

### 2.5 Walkthrough 4: a URL shortener

**Prompt**: "Design a URL shortener like bit.ly."

**Requirements**: create short code for a long URL (optional custom alias, optional expiry);
redirect short code to long URL; basic click counts. Non-functional: 100 million new URLs per
month (assumed), reads 100x writes, redirect p99 under 50 ms, highly available reads, codes not
guessable in sequence (soft requirement).

**Estimates**: 100 M / month ~ 40 writes/s average; reads ~4,000/s average, peaks 10x = 40,000/s.
5 years: 6 billion URLs x ~500 bytes = 3 TB. Code length: base62, 62^7 ~ 3.5 trillion: 7 chars is
plenty.

**API**: `POST /urls {long_url, alias?, expires_at?} -> {code}`; `GET /{code} -> 301/302 Location`.
Use **302** (or 307) if you need click counting, because browsers cache 301 permanently.

**High-level design**

```
 client --> CDN / edge cache --> LB --> redirect service --> cache (Redis / local LRU)
                                              |                   | miss
                                              |                   v
                                              |             KV store: code -> long_url
                                              |             (sharded by code, hash; RF3)
                                              +--> click event --> queue --> analytics aggregator
 client --> LB --> create service --> ID generator --> KV store (insert if absent)
```

**Deep dives**
- **Generating codes**:
  1. Hash the long URL (MD5/SHA-256) and take 7 base62 chars: same URL gives same code
     (dedupe for free) but collisions must be checked and resolved (append a salt, retry).
  2. **Counter + base62**: a unique 64-bit id encoded in base62. No collisions. Ids come from
     ranges leased to each create server by a coordinator (each server gets 1,000 ids at a time,
     like YugabyteDB sequence caching), or a Snowflake-style id (timestamp + machine id +
     sequence). Sequential codes are guessable; scramble with a reversible permutation (e.g. a
     Feistel network over the 64-bit id) if that matters.
  3. Random 7 chars with insert-if-absent (conditional put): simple; collision probability grows
     with fill but stays tiny at 3.5 trillion space.
- **Storage**: a pure key-value access pattern: hash-sharded by code. Any KV or distributed SQL
  store. Custom aliases need a uniqueness check: conditional insert on the same key.
- **Caching**: read-heavy with a hot head (popular links). CDN for the hottest, Redis for the
  rest, LRU local cache in each redirect server for the very top. Cache-aside with TTL; codes are
  immutable so invalidation is rare (only delete/expiry).
- **Analytics**: never write to the DB synchronously per click. Emit an event to a queue; batch
  aggregate counts per code per minute.

**Pushback questions**
- *"301 or 302?"* 301 is cached by browsers and saves load but you lose click data and cannot
  change the target; 302 keeps every click visible.
- *"How do you prevent abuse (phishing links)?"* Rate limit creation per user/IP (walkthrough 3),
  check against safe-browsing lists asynchronously, allow takedown.
- *"The ID coordinator dies."* Servers hold leased ranges and keep working; the coordinator is a
  small Raft-replicated service or a database row with `UPDATE ... RETURNING` to lease ranges.
- *"Hot link from a viral post."* CDN plus local cache absorbs it; the store sees a handful of
  requests.
- *"Expiry?"* Store `expires_at`; check on read; background job or TTL feature to delete.

### 2.6 Walkthrough 5: a notification and queue system

**Prompt**: "Design a system that sends notifications (SMS, email, push) for events from many
services: OTPs, transaction alerts, marketing."

**Requirements**: services submit notification requests; templates and user preferences
(opt-outs, channels, quiet hours); per-channel providers; priorities (OTP within seconds,
marketing can wait hours); retries; delivery status; no duplicate OTP if avoidable; audit for
regulatory alerts. Assumed scale: 50 million notifications per day, peak 5,000/s during batch
campaigns, OTP p99 end-to-end under 5 s.

**High-level design**

```
 producer services --RPC/HTTP--> notification API (validate, idempotency key, enqueue)
                                        |
                                        v
                         +----------------------------------+
                         | durable queue, one topic/queue   |
                         | per priority: otp | txn | bulk   |
                         +----------------------------------+
                           |              |              |
                           v              v              v
                 dispatcher workers (pull, render template, apply preferences,
                 dedupe by key, per-provider token bucket, circuit breaker per provider)
                           |              |              |
                           v              v              v
                      SMS provider A/B   email (SMTP/API)   push (APNs/FCM)
                           |
                 delivery receipts (webhooks) --> status store (by notification id)
 failed after N retries --> dead-letter queue --> ops dashboard / replay tool
```

**Deep dives**
- **Queue choice**: Kafka (partitioned log, consumer offsets, replay, ordering per partition, high
  throughput) vs RabbitMQ (per-message ack, routing, priorities, delayed redelivery) vs a
  database-backed queue (`SELECT ... FOR UPDATE SKIP LOCKED`; simple, transactional with your
  data, fine up to hundreds or low thousands per second, typical). For a bank already on
  PostgreSQL or YugabyteDB, a DB queue for transaction alerts with the outbox pattern is a defensible
  start.
- **Priorities and isolation**: separate queues and worker pools per priority (bulkheads), so a 10
  million message marketing campaign cannot delay OTPs. Do not rely on a single queue with priority
  fields under load.
- **Delivery semantics**: at-least-once from queue to worker. Idempotency key per notification;
  the worker records "sent" with provider message id before acking; a crash between send and
  record can still duplicate, so use the provider's own idempotency key if it supports one.
  For OTPs a duplicate is annoying but harmless; for "your account was debited" alerts, the
  duplicate is confusing, so dedupe on `(event_id, channel)`.
- **Ordering**: usually unnecessary across users; per user, partition by `user_id` (Kafka key) if
  ordering matters (e.g. "debited" before "balance low").
- **Backpressure and provider limits**: providers have rate limits; per-provider token buckets in
  the dispatcher; when a provider is slow, the breaker opens and traffic fails over to provider B.
- **Retries**: exponential backoff with jitter via delayed queues or a "retry at" column;
  max attempts by priority (OTP retries quickly then gives up because a late OTP is useless).
- **Scheduling and quiet hours**: marketing respects time zones and quiet hours: a scheduled
  queue keyed by send time.

**Pushback questions**
- *"Kafka partition count?"* Enough for the parallelism you need in consumers (one consumer per
  partition per group), with headroom since increasing it reshuffles keys; a typical start is tens
  of partitions per topic.
- *"Consumer crashes mid-batch."* Uncommitted offsets are redelivered; idempotency handles the
  duplicates. Commit offsets after the side effect, not before (that would be at-most-once).
- *"A poison message crashes every worker."* Bounded retries, then dead-letter queue; never block
  the partition forever.
- *"How do you know a message was delivered?"* Provider receipt webhooks update status; reconcile
  missing receipts after a timeout; report delivered/failed/unknown per channel.
- *"Exactly-once?"* No: at-least-once with dedupe (module 10). Say it plainly.

## 3. Code

The walkthroughs reuse code from this course. Know where each piece lives so you can say "I built
this":

| Design element | Code |
|----------------|------|
| Text protocol and incremental parser (KV store stage 1) | `code/m04_tcpkv_parser.hpp`, `m04_tcpkv_parser_test.cpp` |
| Length-prefixed framing for large values | `code/m04_framing.cpp` |
| Thread-pool server with graceful shutdown | `code/m05_threadpool_tcpkv_server.cpp` |
| Event-loop server | `code/m06_epoll_echo_server.cpp` |
| Binary RPC with request ids and deadlines (loan workers calling core banking) | `code/m07_rpc.hpp`, `m07_rpc_demo.cpp` |
| HTTP API (URL shortener redirect, notification API) | `code/m08_http_server.cpp` |
| Token bucket (rate limiter, provider limits) | `code/m09_token_bucket.cpp` |
| Hybrid logical clock (timestamps across nodes) | `code/m10_hlc.cpp` |
| Raft election and replication (KV store stage 2) | `code/m11_raft_election_sim.cpp` |
| Consistent hashing (KV store stage 3, cache sharding) | `code/m12_consistent_hash.cpp` |
| The full tcpkv project | `10_Capstone_Projects/tcpkv/` |

A base62 encoder, the kind of five-minute code an interviewer may ask for in the URL shortener:

```cpp
#include <cstdint>
#include <string>

std::string base62(uint64_t n) {
    static const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    if (n == 0) return "0";
    std::string out;
    while (n > 0) {
        out.push_back(digits[n % 62]);
        n /= 62;
    }
    return std::string(out.rbegin(), out.rend());   // most significant digit first
}
```

And the database-backed queue claim, the core of a simple notification worker:

```sql
-- Each worker claims up to 100 due messages without blocking other workers.
WITH next AS (
    SELECT id FROM notification_queue
    WHERE status = 'READY' AND run_at <= now()
    ORDER BY priority, run_at
    LIMIT 100
    FOR UPDATE SKIP LOCKED
)
UPDATE notification_queue q SET status = 'CLAIMED', claimed_at = now(), attempts = attempts + 1
FROM next WHERE q.id = next.id
RETURNING q.*;
```

## 4. Under the hood (senior-level)

What separates a senior answer in these interviews is going one layer down when it matters:

- **"How does your server handle 100,000 connections?"** epoll, one loop per core, per-connection
  state machines with buffers, `SO_REUSEPORT` to spread accepts, file descriptor limits raised,
  memory per connection estimated (module 06), TCP keepalive or app-level heartbeats to drop dead
  peers, TIME_WAIT on the side that closes (module 02).
- **"Where does latency go on a write?"** Client to LB (~0.1 to 0.5 ms in a DC), parse, queueing
  in the pool (rises with utilisation, module 09 section 4), the Raft round (leader fsync plus one
  follower RTT and fsync), apply, reply. The fsync and the queueing usually dominate.
- **"Why not cache everything?"** Cache invalidation and consistency: cache-aside with TTL gives
  stale reads for up to the TTL; write-through costs latency; the thundering herd on a popular
  key's expiry needs request coalescing or early refresh. See `07_Databases_Internals/13_caching_and_redis.md`.
- **"What breaks first at 10x?"** Usually the database's hottest shard or a single-threaded
  component (one Kafka partition, one sequence row, one Redis instance), not the stateless tier.
- **"How do you make it observable?"** Request ids end to end, RED metrics per endpoint, queue age
  (not just length) per queue, per-dependency latency, and SLO burn alerts (module 09 and 13).

**Common mistakes interviewers note**: jumping to a diagram without requirements; designing for
Google scale when the problem is 1 request per second; no numbers; "we'll use Kafka" with no reason;
ignoring failure; never stating a trade-off; going silent while thinking. Narrate.

## 5. In production

Everything in these walkthroughs maps to work you have done or can talk about honestly:

- **KV store**: you built tcpkv (capstone) and you migrated a Berkeley DB key-value store to
  YugabyteDB. You can talk about what changes when a KV store becomes distributed from both ends.
- **Loan origination**: your employer's domain. The state machine, approvals, audit and
  integrations are things you know from production; outbox and idempotency keys are the words for
  patterns you may have implemented with other names.
- **Rate limiter**: per-branch limits on your RPC server (module 09 exercise) are a concrete story.
- **Notifications**: banks send OTPs and transaction alerts; you know the regulatory side (audit,
  retention) that most candidates do not.

Practice by saying the walkthroughs out loud with a timer, drawing on paper. Record yourself once.
Mock interview scripts are in `11_Interview_Prep_Job_Search/11_mock_interview_scripts.md`.

## 6. Try it

1. **(easy)** Do the estimates for each of the five walkthroughs from scratch on paper without
   looking: RPS average and peak, storage per year, bandwidth. Done when: each takes under three
   minutes and your numbers are within 2x of the module's.

2. **(easy)** For the URL shortener, implement `base62` and its inverse and assert round trips for
   10,000 random 64-bit values. Done when: it compiles with the course flags and passes.

3. **(medium)** Run walkthrough 2 (loan origination) aloud in 45 minutes with a timer, drawing the
   diagram from memory. Then list three questions an interviewer could ask that the module did not
   answer, and answer them. Done when: you finished all five method steps inside the time.

4. **(medium)** Implement the hybrid token-leasing rate limiter in a single process: a "central"
   `TokenBucket` (from `m09_token_bucket.cpp`) and 4 local limiter threads that lease 20 tokens at
   a time. Done when: under load the total accepted rate stays within 10% of the central limit and
   the central bucket is called at most 1/20 as often as requests arrive.

5. **(hard)** Design a sixth problem on your own: "a distributed job scheduler that runs
   end-of-day banking batch jobs across a cluster, each job exactly once per business date, with
   dependencies between jobs". Write requirements, estimates, diagram, three deep dives and five
   pushback questions with answers. Done when: it fits on two pages and you can present it in 30
   minutes.

6. **(hard)** Extend `m11_raft_election_sim.cpp` into the KV store stage 2: commands are `SET k v`,
   the state machine is a `std::map`, and a client retries with a request id that the state
   machine deduplicates. Done when: the fuzz loop with crashes and partitions shows every
   acknowledged SET present exactly once and no unacknowledged SET applied twice.

## 7. Interview questions

**Q: Walk me through how you approach a system design question.**
A: Clarify functional and non-functional requirements and state assumptions; estimate load and
storage; define the API and data model; draw the high-level design and walk a request through it;
deep dive into the riskiest parts; finish with bottlenecks, failure modes and monitoring. Trade-offs
are stated out loud throughout.

**Q: How would you make a single-node key-value store survive a node failure?**
A: Replicate each write through a consensus log (Raft) to three nodes and acknowledge after a
majority stores it; the leader serves linearizable reads under a lease; failover is automatic
election. To grow beyond one machine, shard the key space and run a Raft group per shard with a
metadata service holding the shard map.

**Q: How do you avoid the dual-write problem when a service must update its database and publish
an event?**
A: Transactional outbox: write the event to an outbox table in the same database transaction as the
state change, and have a relay publish outbox rows and mark them sent. Delivery is at-least-once, so
consumers deduplicate by event id.

**Q: Local or centralized rate limiting?**
A: Local is fast and dependency-free but inaccurate when traffic for a key is unevenly spread;
central (Redis with an atomic script) is accurate but adds a round trip and a dependency. At scale,
lease batches of tokens from the central store and spend locally. Decide fail-open or fail-closed
per use case.

**Q: How would you generate short codes for a URL shortener?**
A: Either a unique numeric id from leased ranges or a Snowflake-style generator encoded in base62
(no collisions; optionally scrambled to avoid guessable sequences), or a hash or random code with
conditional insert and collision retry. Seven base62 characters give about 3.5 trillion codes.

**Q: How do you keep OTP notifications fast while a 10-million-message campaign is running?**
A: Separate queues and worker pools per priority (bulkheads), per-provider rate limits, and
monitoring of queue age per priority. A single shared queue would put OTPs behind the campaign.

**Q: Kafka or a database-backed queue?**
A: A database queue with SKIP LOCKED is transactional with your data and simple to operate, and is
enough up to moderate throughput. Kafka gives high throughput, replay, ordering per partition and
many independent consumer groups, at the cost of running another distributed system and handling
offsets and partitioning yourself.

## 8. Recap

- Method: requirements, estimates, API and data model, high-level design, deep dives, wrap-up.
- Numbers: 1 M/day is ~12/s; peaks 3 to 10x; DC round trip ~0.5 ms; SSD fsync ~0.1 to a few ms.
- KV store: tcpkv -> WAL -> Raft group -> sharded Multi-Raft with a metadata service.
- Loan origination: scale is small; correctness is the design. State machine, optimistic
  concurrency, outbox, idempotent disbursement, async integrations with breakers, audit.
- Rate limiter: token bucket; local vs central vs leased tokens; fail-open vs fail-closed.
- URL shortener: base62 ids from leased ranges, KV store sharded by code, caches and CDN, 302 for
  analytics, async click events.
- Notifications: per-priority queues, at-least-once plus dedupe, provider limits and failover,
  DLQ, receipts.
- Always state trade-offs and failure modes before being asked. Narrate.
