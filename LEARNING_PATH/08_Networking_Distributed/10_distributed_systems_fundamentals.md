# 10. Distributed systems fundamentals

**Time:** ~3 hours   **Prerequisites:** 02, 07, 09; `07_Databases_Internals/05_transactions_acid_mvcc.md`

## 1. In plain words

A **distributed system** is several computers that cooperate over a network and try to look like
one. Your banking server talking to a three-node YugabyteDB cluster is one. The moment there is
more than one machine, two new facts rule everything: **parts can fail while the rest keeps
running**, and **there is no shared clock** to say what happened first. Almost every idea in this
module (replication, quorums, consistency models, hybrid clocks) is a way of living with those two
facts.

Analogy: three bank branches keep copies of the same ledger and phone each other to stay in sync.
The phone line sometimes drops. A clerk sometimes goes home sick without telling anyone. Each
branch's wall clock is a few minutes off. Now a customer deposits at branch A and withdraws at
branch B two minutes later. Did B see the deposit? Which happened first, by whose clock? If the
phone line between A and B is down, should B refuse the withdrawal (safe but unhelpful) or allow
it (helpful but maybe wrong)? That last question is CAP, and the rest of the module is the
vocabulary for answering the others precisely.

You already know the single-machine version of these problems: threads with no ordering guarantee
(see `04_Concurrency/07_atomics_and_the_memory_model.md`) and transactions with isolation levels
(see `07_Databases_Internals/05_transactions_acid_mvcc.md`). Distributed systems are those
problems again, with messages that can be lost and machines that can vanish.

## 2. The real thing

### 2.1 Partial failure and the network you actually have

On one machine, a function call either returns or the whole process crashes. Across a network, a
request can:

1. never arrive (packet lost, peer down, cable cut);
2. arrive and be processed, but the reply is lost;
3. arrive and be processed slowly (GC pause, swapping, a disk stall), so the reply is late;
4. arrive at a node that crashes halfway through.

From the caller's side, cases 1 to 4 look **identical**: no reply before the timeout. You cannot
tell "dead" from "slow" from "did the work but the answer got lost". This is **partial failure**,
and it is the root of every hard problem in this course. Module 07 met it as "timeout with unknown
outcome". A distributed database meets it on every replication message.

**The eight fallacies of distributed computing** (L. Peter Deutsch and others at Sun, 1990s) are
the assumptions that break first: the network is reliable; latency is zero; bandwidth is infinite;
the network is secure; topology does not change; there is one administrator; transport cost is
zero; the network is homogeneous.

**System models** are how papers state their assumptions. Know the names:

| Model | Meaning |
|-------|---------|
| Synchronous | Message delay and clock drift have known upper bounds. Easy to reason about; not true of real networks. |
| Asynchronous | No bounds at all. A slow node is indistinguishable from a dead one. |
| Partially synchronous | Bounds exist most of the time but can be violated for a while. Real systems are designed for this. Raft and Paxos are safe always and make progress when the network behaves. |
| Crash-stop / crash-recovery | Nodes fail by stopping (and maybe restarting with their disk). Raft assumes crash-recovery. |
| Byzantine | Nodes can lie or be malicious. Needs 3f+1 nodes to tolerate f liars. Blockchains; not databases inside one bank. |

**FLP impossibility** (Fischer, Lynch, Paterson, 1985): in a purely asynchronous system, no
deterministic algorithm can guarantee consensus if even one node may crash. Practical systems
escape it with timeouts (partial synchrony): they stay safe always and are live "when the network
is calm enough". That is why Raft uses randomised election timeouts (module 11).

**Two Generals problem**: two parties communicating over a lossy channel can never be *certain*
they both agree to act, because the last message could always be lost. TCP's handshake does not
solve it; it just makes the uncertainty small. Distributed commit protocols (2PC, module 12) live
with it by having a durable decision somewhere.

### 2.2 No global clock

Every machine has its own quartz oscillator, and they drift: typical crystal drift is tens of
parts per million, which is a few seconds a day if nobody corrects it. Two kinds of clock:

- **Wall clock** (`CLOCK_REALTIME`, `gettimeofday`, Windows `GetSystemTimePreciseAsFileTime`):
  seconds since the epoch, synchronised by NTP, and **can jump backwards** when NTP steps it or an
  operator changes it.
- **Monotonic clock** (`CLOCK_MONOTONIC`, `std::chrono::steady_clock`, Windows
  `QueryPerformanceCounter`): only goes forward, meaningless across machines. Use it for timeouts
  and durations, always. Using wall time for a timeout is a classic bug that fires on a leap second
  or an NTP step.

**NTP** (Network Time Protocol) estimates the offset to a time server from four timestamps of a
request/response, assuming the path is symmetric. On a LAN with a nearby server, typical accuracy
is around a millisecond; across the internet, typically tens of milliseconds; under asymmetric
routes or a loaded server, worse, and NTP cannot detect asymmetry. `chrony` (default on Ubuntu
and RHEL) converges faster than classic `ntpd`. NTP **slews** small offsets (runs the clock slightly
fast or slow) and **steps** large ones (jumps). **PTP** (IEEE 1588) with hardware timestamping gets
to microseconds or better inside a datacenter. Cloud providers offer precise time services (AWS
Time Sync, Google's public NTP with leap smearing).

**Leap seconds**: an extra second inserted at 23:59:60 UTC. Linux used to step backwards; big
providers now **smear** it over many hours instead. Bugs from the 2012 leap second (livelocked
threads in futex code on some kernels) are the reason to know the term.

Because clocks disagree, "event A happened at 10:00:00.003 on node 1 and B at 10:00:00.001 on node
2" does **not** tell you B happened first. You need logical time.

**Lamport clocks** (Lamport, 1978): each node keeps a counter. Increment before each event; send
the counter with each message; on receive set `counter = max(local, received) + 1`. Guarantee: if A
**happened-before** B (A on the same node earlier, or A is a send and B its receive, or a chain of
those), then `L(A) < L(B)`. The converse is false: a smaller number does not prove causality.
Lamport timestamps have nothing to do with real time.

**Vector clocks**: each node keeps a vector of counters, one per node. They capture causality
exactly (you can tell "A before B", "B before A" or "concurrent"), at the cost of O(N) size.
Dynamo-style stores used them to detect conflicting concurrent writes.

**Hybrid logical clocks (HLC)** (Kulkarni, Demirbas et al., 2014) combine both: a timestamp
`(l, c)` where `l` tracks the largest physical time seen and `c` is a small logical counter.
Properties:

- `l` is always at least the local physical clock, and never more than the maximum clock skew
  ahead of it. So HLC values are **close to real time** and can be compared with wall-clock ranges.
- If A happened-before B then `HLC(A) < HLC(B)`, like Lamport.
- It fits in 64 bits.

**YugabyteDB's "hybrid time" is an HLC**: microseconds of physical time in the high bits and a
12-bit logical counter in the low bits of one `uint64`. Every Raft message and every RPC between
tablet servers carries the sender's hybrid time, and the receiver bumps its own clock past it. MVCC
versions are stamped with hybrid time, and a snapshot read "as of time T" means "all versions with
hybrid time <= T". CockroachDB uses HLC the same way. Google Spanner instead uses **TrueTime**: GPS
and atomic clocks give each node an interval `[earliest, latest]` guaranteed to contain true time,
and a commit **waits out** the uncertainty (commit wait) before becoming visible. HLC databases
cannot wait out an unknown bound, so they assume one (`max_clock_skew_usec` in YugabyteDB) and
handle the ambiguity on reads (section 4).

### 2.3 CAP, plainly

**CAP** (Brewer's conjecture 2000, proved by Gilbert and Lynch 2002): when there is a **network
partition** (some nodes cannot talk to others), a system that replicates data must choose between:

- **Consistency** (here it means **linearizability**, section 2.5): every read sees the latest
  completed write, as if there were one copy; or
- **Availability**: every request to a non-failed node gets a non-error response.

It cannot have both during the partition. That is the whole theorem. Things people get wrong:

- "Pick two of three" is misleading. Partitions are not optional; they happen. The real choice is
  **what you do during a partition**: refuse some requests (CP) or answer with possibly stale or
  conflicting data (AP).
- C in CAP is not C in ACID. ACID's C means "constraints hold". CAP's C means linearizable.
- A in CAP is very strict: *every* non-failed node must answer. A Raft cluster where the majority
  side keeps serving is "CP" by the definition, yet highly available in everyday language.
- When there is no partition, CAP says nothing.

Examples:
- **YugabyteDB, CockroachDB, Spanner, etcd, ZooKeeper: CP.** A tablet whose Raft group lost its
  majority refuses writes (and strong reads) until the majority is back. The minority side errors.
- **Cassandra, DynamoDB (default), Riak: AP (tunable).** Any replica accepts writes; conflicts are
  resolved later (last-write-wins by timestamp, or vector clocks, or CRDTs).
- **A single PostgreSQL primary with async standbys**: not partition-tolerant in the CAP sense. If
  the primary is cut off, the clients that can reach it continue; those that cannot get errors. If
  someone promotes a standby on the other side, you can get two primaries (split brain, section 2.8).

### 2.4 PACELC

**PACELC** (Daniel Abadi, 2010): **if P**artition, choose **A** or **C**; **E**lse (normal
operation), choose **L**atency or **C**onsistency. This is the more useful statement, because the
"else" case is 99.99% of your life. Strong consistency costs a round trip to a quorum on every
write (and on some reads), even when nothing is broken.

| System | P: A or C | E: L or C | Notes |
|--------|-----------|-----------|-------|
| YugabyteDB / CockroachDB / Spanner | C | C | Every write waits for a Raft majority. Optional follower reads trade C for L. |
| Cassandra / DynamoDB | A | L | Tunable per request with consistency levels; default favours latency. |
| PostgreSQL + async standby | (C on primary) | L | Commit returns before the standby has the WAL. A failover can lose the last commits. |
| PostgreSQL + synchronous standby | C (blocks if standby gone) | C | Commit waits for the standby's flush. |
| MongoDB (majority write concern) | C | C | With `w:1` it is closer to L. |

For a bank, the ledger is PC/EC: a wrong balance is worse than a slow one. The customer-facing
"recent transactions" screen could be EL (follower read, a second stale) without harm.

### 2.5 Consistency models with concrete examples

A **consistency model** is a contract: which values a read is allowed to return given the writes
that happened. From strongest to weakest:

**Linearizability** (atomic consistency). The system behaves as if there is a single copy and each
operation takes effect at one instant between its start and its end. Once a write completes, every
later read (by anyone, anywhere) sees it or something newer.
- Example: teller 1 posts a deposit and gets "OK" at 10:00:00. Teller 2 at another branch queries
  the balance at 10:00:01. Linearizable: teller 2 sees the deposit. Always.
- Who gives it: Raft-based systems reading from the leader with a valid lease (YugabyteDB default
  reads), etcd, ZooKeeper `sync`+read, Spanner.
- Cost: a quorum round trip per write, and reads must go to (or check with) the leader.

**Sequential consistency**. All nodes see all operations in **one** total order that respects each
client's own program order, but that order need not match real time.
- Example: teller 1 posts deposit D at 10:00:00 and gets OK. Teller 2 reads at 10:00:01 and does
  not see D yet, but will see it later, and every observer agrees D happened before any later
  operation by teller 1. Allowed under sequential, forbidden under linearizable.
- Who gives it: ZooKeeper reads without `sync` (ZooKeeper's actual guarantee is close to this per
  session).

**Causal consistency**. Operations that are causally related (one could have influenced the other)
are seen by everyone in that order. Concurrent operations may be seen in different orders by
different nodes.
- Example: a customer posts "salary credited" and then their spouse, *having seen it*, posts "rent
  paid". Causal: no one sees "rent paid" without "salary credited". Two unrelated deposits at two
  branches may appear in either order to different readers.
- Who gives it: MongoDB causal sessions, COPS-style research systems. It is the strongest model
  that stays available during partitions.

**Session guarantees** (weaker, but what users notice):
- **Read-your-writes**: after you write, *you* see your write. Violated when your write goes to the
  leader and your next read goes to a lagging follower. The customer transfers money, the app
  refreshes from a read replica, and the transfer is "missing". Fix: read from the leader for a
  while after a write, or carry the write's timestamp and read at or after it.
- **Monotonic reads**: you never see time go backwards. Violated when two consecutive reads hit two
  replicas with different lag: balance shows 900, then 1000 again, then 900. Fix: stick a session
  to one replica, or carry the last-seen timestamp.
- **Monotonic writes** and **writes-follow-reads** complete the set.

**Eventual consistency**. If writes stop, all replicas eventually converge to the same value. No
promise about what you read before that.
- Example: DNS. Change a record; for up to the TTL, some resolvers return the old address.
- Who gives it: Cassandra at consistency level ONE, DynamoDB default reads, async read replicas,
  caches.

**How this relates to isolation levels.** Isolation (read committed, snapshot, serializable) is
about **concurrent transactions** on the data; consistency models are about **replicas and
real-time order** of single operations. The combination of serializable isolation and
linearizability is **strict serializability** (Spanner calls it external consistency): transactions
appear in a serial order that matches real time. YugabyteDB SERIALIZABLE on a single cluster aims
at this; snapshot isolation is its default. The Jepsen site's consistency-model map is the best
one-page reference.

### 2.6 Replication topologies

**Replication** = keeping copies of the same data on several nodes, for fault tolerance (a copy
survives a dead node), read scaling, and locality.

**Single-leader** (primary/replica, master/slave). All writes go to one leader; it ships a log of
changes to followers; followers apply it in order. Reads go to the leader (fresh) or followers
(maybe stale).
- PostgreSQL streaming replication, MySQL replication, MongoDB replica sets, Kafka partitions, and
  **each Raft group** (a YugabyteDB tablet is single-leader per tablet).
- Simple, no write conflicts. The leader is a bottleneck and a single point of failure until
  failover.

**Multi-leader** (active-active writes). Several nodes accept writes and replicate to each other
asynchronously.
- Used across datacenters (each DC has a leader), for offline clients (a phone app that syncs
  later), and for collaborative editing.
- The hard part is **write conflicts**: branch A sets the account's address to X while branch B
  sets it to Y, at the same time, on different leaders. Resolution: last-write-wins by timestamp
  (silently loses a write), merge functions, CRDTs (conflict-free replicated data types; counters
  and sets that merge mathematically), or application-level resolution.
- YugabyteDB **xCluster** bidirectional replication between two clusters is multi-leader with
  last-writer-wins at the row level; you must design so the same row is not written on both sides.
- For a ledger, multi-leader on the same accounts is a bad idea. Partition ownership instead: each
  region owns its accounts.

**Leaderless** (Dynamo-style). Any replica accepts reads and writes; the client (or a coordinator)
sends each write to N replicas and waits for W acknowledgements, and each read to R replicas.
- Cassandra, Riak, DynamoDB internally, ScyllaDB.
- Repair mechanisms: **read repair** (a read that sees a stale replica writes the newer value
  back), **anti-entropy** (background Merkle-tree comparison), **hinted handoff** (if a replica is
  down, another node holds the write as a "hint" and forwards it later).

### 2.7 Synchronous vs asynchronous replication

```
 synchronous                                asynchronous
 client -> leader: write                    client -> leader: write
           leader -> follower: log entry              leader: commit locally
           follower: fsync, ack                       leader -> client: OK
           leader: commit                             ... later ...
           leader -> client: OK                       leader -> follower: log entry
```

- **Synchronous**: the client's OK means the change is on more than one machine. Survives the
  leader dying immediately after OK (RPO = 0). Costs at least one network round trip per commit,
  and if the synchronous follower is down the leader **blocks** (unless you have more followers).
- **Asynchronous**: OK means "on the leader's disk". Fast, unaffected by follower health. If the
  leader dies before shipping, the last commits are **lost** on failover (RPO > 0, typically the
  replication lag: milliseconds normally, minutes under load).
- **Semi-synchronous / quorum**: wait for *some* followers. PostgreSQL's
  `synchronous_standby_names = 'ANY 1 (s1, s2)'` waits for any one of two. **Raft is quorum
  replication**: the leader waits for a majority (including itself), so with RF3 it waits for one
  follower, whichever answers first. One slow follower does not slow commits; losing one node
  loses nothing.

### 2.8 Failover and split brain

**Failover**: the leader is detected dead and a follower is promoted. Steps: detect (heartbeats
stop for longer than a timeout), choose the new leader (most up-to-date follower), reconfigure
clients (VIP move, DNS change, or clients ask "who is leader"), and make sure the old leader stays
out.

What goes wrong:
- **False detection**: the leader was slow (GC pause, network blip), not dead. Now you fail over
  unnecessarily, or worse, the old leader comes back believing it is still in charge.
- **Lost writes**: with async replication, the promoted follower lacks the last commits. When the
  old leader returns, its extra writes conflict. GitHub's 2018 incident (a 43-second network
  partition between US East Coast sites led to a MySQL failover and hours of reconciling writes
  made on both sides) is the well-known public example.
- **Split brain**: two nodes both believe they are the leader and both accept writes. Divergent
  data that cannot be merged automatically. For a bank: the same money spent twice.

Defences:
- **Quorum / majority**: a leader must hold a majority to act. Two disjoint majorities cannot
  exist, so two leaders cannot both commit. This is Raft's answer (module 11), and why you deploy 3
  or 5 nodes, never 2. With 2 nodes, neither side of a partition is a majority; either both stop
  (safe) or you allow one side to proceed with a tie-breaker (a **witness** or arbiter node).
- **Fencing tokens**: every leadership grant comes with a number that only increases (Raft's
  **term**, ZooKeeper's zxid, an epoch). Storage and downstream services reject requests carrying
  an older token. A zombie leader waking from a 30-second GC pause has an old token and is refused.
- **STONITH** ("shoot the other node in the head"): the cluster manager power-cycles or isolates
  the old leader via IPMI or a cloud API before promoting. Pacemaker clusters do this.
- **Leases**: the leader may act only while its lease (a time-bounded grant from a quorum) is
  valid, and followers will not elect a new leader until the lease could have expired. Leases
  depend on clocks running at roughly the same **rate** (not the same time), which is a much weaker
  assumption.

### 2.9 Quorums

With **N** replicas, write to **W** and read from **R**. If **R + W > N**, every read set overlaps
every write set in at least one replica, so a read sees the latest acknowledged write (given
versions to tell newest from older).

| N | W | R | Effect |
|---|---|---|--------|
| 3 | 2 | 2 | Classic majority quorum; tolerates 1 failure for both reads and writes. |
| 3 | 3 | 1 | Fast reads; any one node down blocks writes. |
| 3 | 1 | 3 | Fast writes; any node down blocks reads; data on one node until replicated. |
| 3 | 1 | 1 | Eventual consistency; fastest; reads may be stale. |
| 5 | 3 | 3 | Tolerates 2 failures. |

Caveats that interviewers like:
- R + W > N is **not** linearizability by itself. With concurrent writes, sloppy quorums, or a
  write that succeeded on fewer than W nodes before failing, a reader can see a new value and a
  later reader an old one. Dynamo-style stores are eventually consistent with "usually fresh" reads.
- **Sloppy quorum**: if the home replicas are down, write to other nodes (hinted handoff) to stay
  available. Then R + W > N no longer guarantees overlap.
- **Majority quorum in consensus** (Raft, Paxos) is different: N = 2f + 1 tolerates f failures, a
  leader needs a majority to be elected and to commit, and any two majorities intersect. That
  intersection is what carries committed data from one leader to the next.

Fault tolerance table to memorise: 3 nodes tolerate 1 failure, 5 tolerate 2, 7 tolerate 3. 4
nodes still tolerate only 1 (majority of 4 is 3), so even sizes add cost without adding tolerance.

### 2.10 Exactly-once: the myth and the practice

Delivery guarantees between a sender and a receiver over an unreliable network:

- **At-most-once**: send once, never retry. Messages can be lost. ("Fire and forget".)
- **At-least-once**: retry until acknowledged. Messages can be duplicated (the ack was lost, not
  the message).
- **Exactly-once delivery**: impossible in general over a lossy network with failing nodes. This is
  the Two Generals problem again: the sender can never be sure whether the receiver processed
  the last attempt.

What systems actually provide is **exactly-once processing (effect)**:

```
 exactly-once effect = at-least-once delivery + idempotent processing (or deduplication)
```

- **Idempotency key**: the client generates a unique id per logical operation (the branch's
  transaction reference). The server stores it **in the same transaction** as the effect (a unique
  constraint on `posting.idempotency_key`). A retry hits the constraint and returns the stored
  result instead of posting twice. Module 07 built this vocabulary.
- **Dedup table with expiry**: remember processed message ids for longer than the maximum retry
  window.
- **Naturally idempotent operations**: "set balance to 1000" is idempotent; "add 100" is not.
  Design state changes as "set to" or "apply event #N" where possible.
- **Kafka's exactly-once semantics** = idempotent producers (sequence numbers per partition so the
  broker drops duplicates) plus transactions that atomically write output and commit consumer
  offsets. It is exactly-once **inside Kafka**. The moment your consumer calls an external system
  (sends an SMS, calls a payment API), you are back to at-least-once plus idempotency.

The interview sentence: "There is no exactly-once delivery; there is at-least-once delivery with an
idempotent receiver, and I make the dedup check and the side effect atomic."

## 3. Code

### 3.1 Hybrid logical clock simulation (`code/m10_hlc.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m10_hlc.cpp -o hlc && ./hlc
```

Three simulated nodes with skewed clocks (B runs 5 ms fast, C runs 8 ms slow) exchange messages
with random delays. Halfway through, A's clock is stepped back 20 ms, like an NTP step. The program
counts how often raw wall-clock timestamps say a message was received **before** it was sent
(hundreds of times in a typical run), and asserts that the HLC orders every send before its
receive, never goes backwards on any node, and never runs ahead of the fastest physical clock.

The whole algorithm is two functions:

```cpp
struct Hlc { int64_t l = 0; uint32_t c = 0; };   // l: microseconds, c: logical counter

Hlc local_or_send(int64_t true_us) {
    int64_t pt = physical(true_us);                // this node's (skewed) wall clock
    int64_t l_new = std::max(h_.l, pt);
    h_.c = (l_new == h_.l) ? h_.c + 1 : 0;         // physical clock did not advance: bump counter
    h_.l = l_new;
    return h_;
}

Hlc receive(const Hlc& m, int64_t true_us) {
    int64_t pt = physical(true_us);
    int64_t l_new = std::max({h_.l, m.l, pt});
    if (l_new == h_.l && l_new == m.l) h_.c = std::max(h_.c, m.c) + 1;
    else if (l_new == h_.l)            h_.c = h_.c + 1;
    else if (l_new == m.l)             h_.c = m.c + 1;   // sender was ahead: inherit its counter
    else                               h_.c = 0;         // our physical clock is ahead of both
    h_.l = l_new;
    return h_;
}

// YugabyteDB-style packing: one uint64 compare orders two hybrid times.
uint64_t packed() const { return (static_cast<uint64_t>(l) << 12) | c; }
```

Read the output lines marked `[RAW CLOCK WRONG]`: the send's physical time is later than the
receive's, but the HLC values are correctly ordered because the receiver adopted the sender's `l`
and bumped `c`.

### 3.2 An idempotent posting, the SQL half

```sql
-- The dedup check and the effect commit together or not at all.
CREATE TABLE posting (
    posting_id      bigint GENERATED ALWAYS AS IDENTITY,
    idempotency_key text   NOT NULL UNIQUE,   -- branch code + branch txn reference
    account_no      text   NOT NULL,
    amount_minor    bigint NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (posting_id)
);

-- Retry-safe insert: a duplicate returns the original row instead of posting twice.
INSERT INTO posting (idempotency_key, account_no, amount_minor)
VALUES ('BR017-T000981', 'ACC0004242', 50000)
ON CONFLICT (idempotency_key) DO NOTHING
RETURNING posting_id;
-- If RETURNING gave no row, SELECT the existing one by idempotency_key and return it.
```

## 4. Under the hood (senior-level)

**Why HLC needs a skew bound for reads.** A snapshot read at hybrid time T on tablet X wants every
write with timestamp <= T. But a write committed on another node whose clock is behind could get a
timestamp slightly <= T *after* our read started, because that node's clock had not caught up. The
reader cannot know whether such a write exists. YugabyteDB handles this with an **uncertainty
window**: if a read at T finds a value with timestamp in `(T, T + max_clock_skew]`, it cannot tell
whether that write happened before or after the read in real time, so it **restarts the read** at
a later time ("Restart read required" in logs and errors, retried internally where possible).
`max_clock_skew_usec` defaults to 500 ms in YugabyteDB (check your version's docs). If real skew
exceeds the configured bound, correctness is no longer guaranteed, which is why YugabyteDB
servers refuse to run (crash with a clock-skew error) when they detect their clock is too far off,
and why you monitor NTP/chrony on every database node. Spanner avoids the restart by waiting out
the uncertainty at commit time instead (commit wait), which needs TrueTime's tight bounds.

**HLC counter overflow.** The logical counter `c` grows only while physical time does not advance
past `l`. With microsecond physical time, 12 bits (4096 events per microsecond per node) is
plenty. If a node's clock is stepped back a long way, `l` stays ahead and `c` carries the ordering
until physical time catches up. The demo shows this after the 20 ms step.

**What a "linearizable read" costs in Raft.** A leader cannot just read its local state: it might
have been deposed a moment ago without knowing (a new leader already committed newer writes). Two
fixes: (1) **ReadIndex**: record the commit index, send a heartbeat round to confirm leadership
with a majority, wait until the state machine has applied that index, then read. One round trip
per read (batched). (2) **Leader lease**: followers promise not to elect a new leader for a lease
period after acknowledging the leader; within the lease the leader reads locally with no round
trip. YugabyteDB uses leader leases (default lease around 2 seconds; this is part of why a dead
leader's tablets are unavailable for a few seconds before a new one is elected). Leases rely on
bounded clock **drift rate**, not synchronised time.

**Replication lag is a queue.** Async replication lag = time the follower is behind. It grows when
the follower applies slower than the leader writes (single-threaded replay, a long query on the
standby holding back replay because of `hot_standby_feedback` or conflicts, slow disk). PostgreSQL
exposes it in `pg_stat_replication` (`write_lag`, `flush_lag`, `replay_lag`). Raft followers
expose it as how far their applied index trails the leader's commit index.

**Why majorities, mathematically.** Two subsets of size > N/2 of an N-set must intersect (their
sizes sum to more than N). Any decision made by one majority is therefore seen by at least one
member of every future majority. Raft's election vote restriction (module 11) uses that member to
guarantee the next leader already has every committed entry.

**Timeouts are a guess.** Every failure detector in an asynchronous network is either too eager
(false positives: needless failovers) or too slow (long unavailability). Production systems use
heartbeats every 100 ms to 1 s and declare failure after a few missed ones, and accept that GC
pauses and VM stalls occasionally trigger a needless election. The **phi accrual detector**
(Cassandra, Akka) outputs a suspicion level from the observed heartbeat-interval distribution
instead of a yes/no.

## 5. In production

- **Read-your-writes on a read replica.** A loan officer saves an application; the next screen
  reads from a replica 300 ms behind and shows "not found"; the officer saves again, creating a
  duplicate. Fixes: read from the primary for the session's next few seconds, or route reads by
  "this session wrote recently", or carry the commit timestamp. With YugabyteDB, follower reads are
  off by default, so the default is safe; turn them on only for screens that tolerate staleness
  (`yb_read_from_followers` plus a staleness bound in recent versions).
- **Wall clock used for ordering.** Two app servers stamp postings with `now()` from their own
  clocks, and a report orders by that column. When one server's clock is 2 seconds fast, postings
  appear out of order and "balance after" columns look wrong. Fix: take the timestamp from one
  place (the database's commit time) or use a sequence; never order across machines by local wall
  time.
- **NTP not running on a database node.** A YugabyteDB node with a clock drifting beyond the skew
  bound triggers read restarts and eventually refuses to start. Always check `chronyc tracking`
  (offset, frequency) as part of node health. Alert when offset exceeds a fraction of the bound.
- **Split brain with a two-node active-passive pair.** A heartbeat cable failed; both nodes
  promoted themselves; the VIP flapped between them; writes landed on both. Root cause: no quorum
  and no fencing. Fix: a third vote (witness), and STONITH or storage fencing.
- **"Exactly once" queue that wasn't.** A notification consumer marked messages done after sending
  the SMS; a crash between sending and marking resent SMS after restart. Customers received the
  same OTP twice. That is the at-least-once you always have; the fix is a dedup key per message at
  the SMS gateway or accepting duplicates for messages where they are harmless.
- **Your RPC server is a distributed system already.** Branch client, app server and database
  are three nodes with partial failure between each pair. Every timeout in your logs is a case of
  section 2.1. When you describe it in an interview, use these words: unknown outcome, idempotency
  key, quorum-replicated database, failover, fencing.

## 6. Try it

1. **(easy)** Build and run `m10_hlc`. Pick one `[RAW CLOCK WRONG]` line and explain, with the
   numbers printed, which branch of the receive rule fired and why the HLC result is correct.
   Done when: you can explain it without looking at the code.

2. **(easy)** On WSL run `chronyc tracking` (install `chrony` if needed) or `timedatectl
   timesync-status`. Note the current offset and frequency error. Done when: you can say how many
   microseconds your clock drifts per second at the reported frequency error.

3. **(medium)** Add a Lamport clock alongside the HLC in `m10_hlc.cpp` and print both for every
   delivered message. Done when: you can show two events where the Lamport clock orders them but
   the events are concurrent (no happened-before), and explain why HLC has the same limitation.

4. **(medium)** For each screen of your banking application (balance inquiry, posting, statement,
   loan dashboard, end-of-day report), write which consistency model it needs and whether a
   follower read with 1 s staleness would be acceptable. Done when: every screen has a one-line
   justification.

5. **(hard)** Implement a tiny leaderless store simulation: N=3 replicas as in-memory maps with
   versioned values, a coordinator doing W-of-N writes and R-of-N reads with read repair, and
   random node failures. Show a stale read with R=1, W=1 and none with R=2, W=2 (no concurrent
   writes). Done when: asserts verify both behaviours over 10,000 random operations.

6. **(hard)** Write the "unknown outcome" decision table for your employer's posting RPC: for each
   failure point (before send, during send, after server commit, during reply), what the client
   sees, what state the server is in, and what a retry does today. Then propose the idempotency
   change that makes every retry safe. Done when: the table has no "it depends" cell.

## 7. Interview questions

**Q: What is partial failure and why does it make distributed systems hard?**
A: Some components fail while others keep running, and a caller cannot distinguish a dead node
from a slow one or from a lost reply. Every remote call can end in "unknown outcome", so you need
timeouts, idempotent retries, and protocols that stay correct when messages are lost or delayed.

**Q: State CAP correctly.**
A: During a network partition, a replicated system must choose between linearizable consistency
and availability (every non-failed node answers). Partitions are not optional, so the real choice
is behaviour during a partition. YugabyteDB and etcd choose consistency (minority side refuses);
Cassandra chooses availability (any replica answers, conflicts resolved later).

**Q: What does PACELC add?**
A: It covers normal operation: even without a partition you trade latency for consistency, because
strong consistency needs a quorum round trip per write. Spanner and YugabyteDB are PC/EC;
Cassandra is PA/EL; PostgreSQL with async replicas is effectively EL for replicas.

**Q: Linearizable versus sequential versus eventual consistency?**
A: Linearizable: once a write completes, every later read anywhere sees it, as if one copy existed.
Sequential: everyone agrees on one order that respects each client's own order, but it may lag real
time. Eventual: replicas converge if writes stop, with no promise before that. Example: a deposit
confirmed at 10:00:00 must be visible to another branch at 10:00:01 under linearizability only.

**Q: Why can't you order events by wall-clock timestamps across machines?**
A: Clocks drift and are corrected imperfectly (NTP is typically millisecond-accurate on a LAN and
can step backwards), so a later event on a slow clock can get an earlier timestamp. Use logical
clocks (Lamport, vector) or hybrid logical clocks, which respect causality and stay close to real
time.

**Q: What is a hybrid logical clock and where is it used?**
A: A timestamp (physical time, logical counter) that is always at least local physical time, at
most the max skew ahead of it, and increases along every causal chain. YugabyteDB and CockroachDB
stamp MVCC versions and transactions with it; YugabyteDB packs it into 64 bits with a 12-bit
counter and assumes a max clock skew, restarting reads that fall inside the uncertainty window.

**Q: Explain R + W > N.**
A: With N replicas, writing to W and reading from R guarantees the read set overlaps the latest
write set when R + W > N, so the reader can see the newest version. It does not by itself give
linearizability (concurrent writes, partial failed writes, sloppy quorums break it).

**Q: Is exactly-once delivery possible?**
A: Not over a lossy network with failures: the sender can never know whether the last attempt was
processed. Systems provide exactly-once effect: at-least-once delivery plus idempotent processing
or deduplication, with the dedup record committed atomically with the side effect.

## 8. Recap

- Partial failure: dead, slow and "done but reply lost" look the same. Every remote call can end
  in unknown outcome.
- Use monotonic clocks for timeouts; wall clocks drift and jump. NTP is typically ms on a LAN.
- Lamport clocks respect causality; vector clocks detect concurrency; HLC adds closeness to real
  time. YugabyteDB hybrid time = HLC packed into 64 bits with a 12-bit counter.
- CAP: during a partition choose linearizable or available. PACELC: otherwise choose latency or
  consistency. YugabyteDB is PC/EC.
- Consistency models from strong to weak: linearizable, sequential, causal, session guarantees,
  eventual. Know one example of each.
- Replication: single-leader, multi-leader (conflicts), leaderless (quorums, read repair, hints).
- Sync replication: RPO 0, latency and blocking cost. Async: fast, loses the tail on failover.
  Raft is majority-quorum replication.
- Split brain is prevented by majorities, fencing tokens (terms), STONITH, leases. Never 2 nodes.
- 3 nodes tolerate 1 failure, 5 tolerate 2; even sizes add nothing.
- Exactly-once effect = at-least-once + idempotency key committed with the effect.
