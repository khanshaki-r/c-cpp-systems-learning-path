# 11. Consensus: Raft in plain words

**Time:** ~3 hours   **Prerequisites:** 10

## 1. In plain words

**Consensus** is getting several machines to agree on one value, or on one sequence of values,
even when some of them crash and messages get lost or delayed. "Agree on a sequence" is the useful
form: if every replica agrees on the same ordered log of commands ("debit A 100", "credit B 100")
and applies them in that order, every replica ends in the same state. That is called **state
machine replication**, and it is how etcd, Consul, CockroachDB, TiKV and YugabyteDB keep copies
identical.

**Raft** (Diego Ongaro and John Ousterhout, 2014, "In Search of an Understandable Consensus
Algorithm") is a consensus algorithm designed to be easy to understand. It splits the problem into
three parts: elect one **leader**; the leader **replicates** its log to the others; and a set of
**safety rules** guarantee that once an entry is committed it is never lost or changed, no matter
who becomes leader next.

Analogy: a committee of five keeps the official minutes. One member is chair. Only the chair
writes new lines into the minutes and reads them out to the others; a line is official once a
majority (three) have copied it into their own books. If the chair stops answering the phone, any
member who has waited long enough calls a vote for a new chair for the next "session number". You
only vote for someone whose minute-book is at least as complete as yours, and you vote once per
session. Because any two majorities of five share at least one member, the new chair always has
every official line. That is Raft.

## 2. The real thing

### 2.1 Roles, terms and the two RPCs

Every node is in one of three roles:

```
                 times out,                         receives votes from
   +----------+  starts election   +-----------+    a majority           +--------+
   | Follower | -----------------> | Candidate | ----------------------> | Leader |
   +----------+                    +-----------+                         +--------+
        ^  ^   discovers leader or      |  times out: new election           |
        |  +--- higher term ------------+  (term + 1)                        |
        +--------------------------- discovers higher term ------------------+
```

- **Term**: a number that only increases, used as a logical clock and as a **fencing token**.
  Each term has at most one leader. Every message carries the sender's term. Seeing a higher term
  makes any node adopt it and become a follower immediately. A message with a lower term is
  rejected.
- **RequestVote** (candidate to all): "I am candidate for term T; my last log entry is at index i
  with term t. Vote for me?"
- **AppendEntries** (leader to each follower): "For term T: the entry before these is at index p
  with term pt; here are new entries; my commit index is c." An AppendEntries with no entries is a
  **heartbeat**.

Persistent state on every node (must be fsynced before replying to any RPC): `currentTerm`,
`votedFor`, and the `log[]`. Volatile: `commitIndex`, `lastApplied`; on the leader also
`nextIndex[]` and `matchIndex[]` per follower.

### 2.2 Leader election

1. A follower expects heartbeats. If none arrive within its **election timeout** (randomised, the
   paper uses 150 to 300 ms), it assumes the leader is gone.
2. It becomes a candidate: increments its term, votes for itself, resets its timer, and sends
   RequestVote to everyone.
3. Each node grants **at most one vote per term**, first come first served, and only if the
   candidate's log is **at least as up to date** as its own (compare last entry's term, then
   length). Granting a vote also resets the voter's own election timer.
4. A candidate with votes from a **majority** becomes leader and immediately sends heartbeats,
   which stop other elections.
5. If a candidate receives an AppendEntries from a leader with term >= its own, it steps down.
6. If nobody wins (split vote), the timers expire again (randomly) and a new term starts.

Why randomised timeouts: if all followers timed out together they would all vote for themselves
and split the vote forever. With randomness, one node usually times out first and collects a
majority before the others wake up. The paper's rule of thumb:

```
broadcastTime  <<  electionTimeout  <<  MTBF (mean time between failures)
  (~1 to 20 ms)       (~150 to 300 ms)        (months)
```

### 2.3 Log replication

```
 leader (term 3) log:  idx 1   2   3   4   5   6
                       t1  t1  t2  t3  t3  t3
                       x=1 y=2 z=3 a=4 b=5 c=6        commitIndex = 5

 follower A:           t1  t1  t2  t3  t3             matchIndex[A] = 5
 follower B:           t1  t1  t2                     nextIndex[B] = 4 (catching up)
```

1. A client sends a command to the leader. The leader appends it to its own log (not yet
   committed) and sends it in AppendEntries to every follower.
2. Each AppendEntries carries `prevLogIndex` and `prevLogTerm`. The follower accepts only if its
   own log has an entry at `prevLogIndex` with term `prevLogTerm` (**consistency check**). If not,
   it rejects and the leader retries with `nextIndex - 1` (or a smarter hint) until they find the
   point where the logs agree; then the follower deletes any conflicting entries after that point
   and appends the leader's.
3. When an entry is stored on a **majority** (the leader counts itself), the leader advances its
   **commit index** to it, applies it to its state machine, and replies to the client.
4. Followers learn the commit index from the next AppendEntries (`leaderCommit`) and apply up to
   it.

The **Log Matching Property** follows by induction from the consistency check: if two logs have
an entry with the same index and term, they are identical in every entry up to that index.

### 2.4 The commit index and the current-term rule

An entry is **committed** when the leader that created it has replicated it to a majority. A
committed entry will be present in the log of every future leader. The subtle case (Figure 8 in
the paper): a new leader finds entries from **older** terms in its log that are on a majority. It
may **not** count replicas to commit them directly, because a different future leader could still
overwrite them. Rule: a leader only commits entries from **its own term** by counting replicas;
older entries become committed indirectly when a current-term entry after them commits. That is why
leaders append a **no-op** entry at the start of their term: committing it commits everything
before it. The simulation in section 3 does this.

### 2.5 Safety, in five properties

| Property | Statement | Enforced by |
|----------|-----------|-------------|
| Election Safety | At most one leader per term. | One vote per node per term, majority needed. |
| Leader Append-Only | A leader never overwrites or deletes its own entries. | Leader only appends. |
| Log Matching | Same index and term implies identical prefix. | AppendEntries consistency check. |
| Leader Completeness | An entry committed in term T is in the log of every leader of a later term. | Vote restriction (up-to-date log) + majority intersection + current-term commit rule. |
| State Machine Safety | No two nodes apply different commands at the same index. | Follows from the above. |

Why Leader Completeness holds, in one paragraph: a committed entry is on a majority. A candidate
needs votes from a majority. The two majorities share at least one voter. That voter has the
entry, and it only votes for a candidate whose log is at least as up to date as its own, which
(by the comparison rule and Log Matching) means the candidate also has the entry.

**What Raft does not promise**: that an uncommitted entry survives (a leader that crashes before
replicating to a majority loses it, and the client got no OK, so that is fine), or that the
**client** knows the outcome after a timeout (module 07's unknown outcome; real systems add client
session ids so a retried command is not applied twice).

### 2.6 Membership changes

Adding or removing nodes changes what "majority" means. Switching every node from the old
configuration to the new at once is unsafe: for a moment, the old majority and the new majority
might not intersect, allowing two leaders.

- **Joint consensus** (the Raft paper): go through an intermediate configuration C_old,new in which
  every decision needs a majority of **both** C_old and C_new. Once that is committed, move to
  C_new.
- **Single-server changes** (Ongaro's thesis, used by most implementations): add or remove **one**
  node at a time. Any majority of the old and new configurations then always intersect. To go from
  3 to 5 nodes you do two single changes.
- New nodes join as **learners / non-voters** first, receive a snapshot and the log, and become
  voters only once caught up, so a slow new node does not stall commits. YugabyteDB adds a new
  tablet peer as a `PRE_VOTER`, bootstraps it from a remote snapshot, then promotes it.
- Replacing a dead node in RF3: add the new one (temporarily 4 voters) then remove the dead one,
  or remove first then add. Remove-first leaves you at 2 voters (no fault tolerance) briefly;
  add-first keeps tolerating one failure throughout. Know why the order matters.

### 2.7 Practical extensions you will hear about

- **Snapshots and log compaction**: the log cannot grow forever. Each node snapshots its state
  machine at some index and discards the log before it. A follower too far behind receives an
  **InstallSnapshot** instead of entries. (YugabyteDB: tablet "remote bootstrap" copies RocksDB
  files.)
- **Pre-vote**: before incrementing its term, a would-be candidate asks "would you vote for me?".
  Prevents a node that was partitioned away (and kept incrementing its term) from forcing a
  needless election when it rejoins.
- **Check-quorum**: a leader that has not heard from a majority for an election timeout steps
  down, so a minority-side leader stops accepting writes quickly. (The simulation leaves it out on
  purpose so you can see a stale leader that cannot commit.)
- **Leader leases and ReadIndex**: linearizable reads without writing to the log (module 10
  section 4).
- **Leadership transfer**: the leader brings a chosen follower fully up to date and tells it to
  start an election immediately. Used for rolling restarts and load balancing leaders.
- **Batching and pipelining**: send many entries per AppendEntries and do not wait for one round
  to finish before starting the next. This is where real throughput comes from.

### 2.8 Raft per tablet in YugabyteDB

YugabyteDB does not run one Raft group for the whole database. Each table is split into **tablets**
(shards, module 12), and **each tablet is its own Raft group** with RF replicas ("tablet peers")
on different `yb-tserver` processes. This is called **Multi-Raft**; CockroachDB (ranges) and TiKV
(regions) do the same.

```
 yb-tserver 1               yb-tserver 2               yb-tserver 3
 +----------------+         +----------------+         +----------------+
 | tablet A  [L]  |         | tablet A  [F]  |         | tablet A  [F]  |
 | tablet B  [F]  |         | tablet B  [L]  |         | tablet B  [F]  |
 | tablet C  [F]  |         | tablet C  [F]  |         | tablet C  [L]  |
 +----------------+         +----------------+         +----------------+
       yb-master (its own Raft group of 3): catalog, tablet placement, load balancing
```

Consequences you should be able to explain:
- **Leaders are spread**: each server leads some tablets and follows others, so write load is
  spread across the cluster. The master's load balancer moves leaders (leadership transfer) to
  keep them even, and can pin preferred leaders to a region.
- **Failure is per tablet**: when tserver 1 dies, only the tablets it led lose their leader. Their
  followers wait out the leader lease, elect new leaders in a few seconds (typical), and carry on.
  Tablets led elsewhere never notice beyond losing one follower.
- **Writes**: a write to one row goes to that row's tablet leader, which appends to its Raft log
  (the WAL), replicates to a majority, applies to its RocksDB-based DocDB storage, and replies.
  With RF3 the commit waits for the leader plus one follower.
- **Reads**: by default from the tablet leader, using the leader lease so no extra round trip is
  needed. Follower reads are opt-in and return slightly stale data.
- **The WAL is the Raft log**. There is no separate database WAL; the Raft log entries are the
  redo log for the tablet.
- **Thousands of Raft groups** on one cluster means heartbeats must be batched per server pair,
  which YugabyteDB does (one RPC carries heartbeats for many tablets).
- **yb-master** is itself a Raft group (usually 3 masters), so losing one master is also
  tolerated.

### 2.9 Contrast: PostgreSQL streaming replication is not consensus

PostgreSQL ships WAL from one **primary** to **standbys** (physical streaming replication). It is
single-leader replication, sync or async (`synchronous_commit`, `synchronous_standby_names` with
`FIRST n` or `ANY n` quorum). What it does not do by itself:

- **Elect a leader.** If the primary dies, nothing inside PostgreSQL promotes a standby. A human or
  an external tool runs `pg_ctl promote` / `pg_promote()`.
- **Prevent split brain.** If the old primary comes back (or was only partitioned), PostgreSQL
  does not stop it from accepting writes. The tool that promoted must also fence it.
- **Guarantee no loss on failover** with async standbys: the promoted standby may lack the last
  commits. The old primary's extra WAL must be discarded (`pg_rewind`) before it can rejoin.
  PostgreSQL marks the fork with a new **timeline** id.

So HA PostgreSQL setups bolt consensus on from outside: **Patroni** keeps the leader key in etcd,
Consul or ZooKeeper (which run Raft or Zab), and only the node holding the key (with a TTL) may be
primary; it demotes itself if it cannot renew the key. **pg_auto_failover** uses a monitor node.
Cloud services do the equivalent internally.

The one-sentence answer to "Why does YugabyteDB need Raft when PostgreSQL does not?": PostgreSQL
has one writable node and leaves failover and fencing to external tools, while YugabyteDB has
thousands of independently-led shards across nodes and must automatically and safely pick a new
leader for each one without losing committed writes, which is exactly what consensus provides.

### 2.10 Paxos in one page

**Paxos** (Leslie Lamport; "The Part-Time Parliament", 1998; "Paxos Made Simple", 2001) is the
original practical consensus algorithm. Basic (single-decree) Paxos agrees on **one** value in two
phases: a proposer picks a ballot number and asks acceptors to **promise** to ignore lower ballots
(phase 1, prepare/promise); if a majority promise, it asks them to **accept** a value, choosing the
value of the highest-ballot proposal already accepted by any of them if one exists (phase 2,
accept/accepted). A value is chosen once a majority accept it.

**Multi-Paxos** runs Paxos for each log slot and optimises by keeping a stable leader that skips
phase 1. It is essentially what Raft is, but the papers left many practical details (leader
election, log gaps, membership) to implementers, which is why Raft was written. Raft's terms are
Paxos ballots; its election is Paxos phase 1; AppendEntries is phase 2.

Who uses what: Google Chubby and Spanner (Multi-Paxos), Apache ZooKeeper (Zab, a Paxos relative),
etcd, Consul, CockroachDB, TiKV, YugabyteDB, Kafka KRaft, MongoDB's replica-set protocol (Raft-like)
use Raft or variants. **Equivalent strength**: both tolerate f crash failures with 2f + 1 nodes;
neither tolerates Byzantine nodes.

## 3. Code

### 3.1 Raft simulation (`code/m11_raft_election_sim.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m11_raft_election_sim.cpp -o raft_sim && ./raft_sim
```

A single process holds five simulated nodes. Time advances in 1 ms ticks. `send` puts a message in
a `std::multimap` keyed by delivery time (2 to 15 ms later). A crashed node stops ticking and
receives nothing; a partition assigns each node a group and drops cross-group messages. Every run
with the same seed is identical, which is how serious Raft implementations are tested.

The scripted scenario prints a timeline:

1. Initial election (one node times out first and wins).
2. Three client commands committed on all five nodes.
3. Leader crashes; a new leader is elected with a higher term and commits a new command.
4. The old leader restarts as a follower and catches up.
5. The leader and one follower are partitioned away from the other three. The isolated leader
   still accepts `set X=LOST` (it does not know it is cut off) but can never commit it. The
   majority side elects a leader in a higher term and commits `set Y=KEPT`. Two nodes claim
   leadership at once, in different terms, which is legal: only one can commit.
6. The partition heals. The stale leader sees the higher term, steps down, and its uncommitted
   entry is overwritten by the consistency check.

Then a fuzz loop runs 30 seeds of 20 simulated seconds with random crashes, restarts, partitions
and 5% message loss, asserting the safety properties on every tick.

The heart of the vote handler:

```cpp
case MsgType::RequestVote: {
    // Only vote for a candidate whose log is at least as up to date as ours. A winner needs a
    // majority of votes and every committed entry is on a majority, so the winner has them all.
    bool log_ok = m.last_log_term > n.last_term() ||
                  (m.last_log_term == n.last_term() && m.last_log_index >= n.last_index());
    bool grant = m.term == n.term && (n.voted_for == -1 || n.voted_for == m.from) && log_ok;
    if (grant) {
        n.voted_for = m.from;
        reset_election_timer(n);
    }
    ...
}
```

The follower side of AppendEntries, including conflict truncation:

```cpp
if (m.prev_index > n.last_index() || n.log[m.prev_index].term != m.prev_term) {
    send(r);                                   // reject: leader will back up nextIndex
    break;
}
int idx = m.prev_index;
for (const Entry& e : m.entries) {
    ++idx;
    if (idx <= n.last_index() && n.log[idx].term != e.term) {
        assert(idx > n.commit_index);          // never truncate a committed entry
        n.log.resize(idx);                     // conflict: drop this entry and all after it
    }
    if (idx > n.last_index()) n.log.push_back(e);
}
n.commit_index = std::max(n.commit_index, std::min(m.leader_commit, idx));
```

The leader's commit rule with the current-term restriction:

```cpp
for (int N = n.last_index(); N > n.commit_index; --N) {
    if (n.log[N].term != n.term) break;        // only count replicas for current-term entries
    int count = 0;
    for (int peer = 0; peer < size(); ++peer) if (n.match_index[peer] >= N) ++count;
    if (count > size() / 2) { n.commit_index = N; break; }
}
```

`check_safety()` runs after every tick: it records the first entry committed at each index and
asserts no node ever commits a different one there (State Machine Safety), and that every leader
holds every entry committed in an earlier term (Leader Completeness). `become_leader` asserts
Election Safety with a `term -> leader` map.

## 4. Under the hood (senior-level)

**Durability before replying.** `currentTerm`, `votedFor` and new log entries must be on stable
storage before a node replies to RequestVote or AppendEntries. If a node votes, crashes, forgets,
and votes again in the same term, two leaders can be elected. Real implementations group-commit:
batch many entries into one `fsync`. The fsync latency of the disk is usually the floor of commit
latency: with RF3 the commit time is roughly `max(leader fsync, fastest follower's network RTT +
fsync)`. On cloud disks typical fsync is around a millisecond or more; on local NVMe, tens to
hundreds of microseconds.

**The leader also has to be up to date to serve reads.** See module 10 section 4: ReadIndex or
leases. Without one of them, a deposed leader in a minority partition serves stale reads.

**Why five nodes is not always better.** Five nodes tolerate two failures but every commit waits
for the third-fastest of five rather than the second-fastest of three, and there is more
replication traffic. RF3 is the common default; RF5 for the metadata layer or when you must
survive a zone plus a node.

**Disruptive servers.** A node that is partitioned keeps timing out and incrementing its term. When
it rejoins, its higher term forces the current leader to step down, causing a needless election.
Pre-vote fixes this. A related problem in the simulation: an ex-leader whose election deadline is
stale would time out immediately after stepping down; the code resets its timer when it steps down.

**Flexible quorums.** Heidi Howard's work (2016) showed that Paxos only needs election quorums
and replication quorums to intersect each other, not each to be majorities. Some systems use this
to make replication quorums smaller across regions. Raft as specified uses majorities for both.

**Raft and the WAL in YugabyteDB.** Each tablet's Raft log is stored as WAL segments on the
tserver. An entry is "replicated" when it is durable in a majority of WALs. Applying it writes into
the tablet's RocksDB memtable; RocksDB's own WAL is disabled because the Raft log already provides
durability. On restart a tablet replays its Raft log from the last flushed point ("tablet
bootstrap"). Log retention is driven by flushes and by lagging followers (YugabyteDB keeps WAL long
enough for followers to catch up, then falls back to remote bootstrap).

**Multi-Raft overhead.** Each Raft group has its own leader, log and timers. Thousands per node
mean heartbeat batching, shared WAL writer threads, and careful per-tablet memory accounting. This
is also why tiny tables in YugabyteDB are often **colocated** into one tablet: fewer Raft groups.

## 5. In production

- **Leader churn from GC or CPU starvation.** A tserver under heavy compaction or swap misses
  heartbeats, its tablets elect new leaders, then it comes back and leadership moves again.
  Symptom: latency spikes and "leader not ready" errors. Look at leader change counters per
  tablet, CPU steal on VMs, and disk latency. Fix the starvation; do not just raise timeouts.
- **Two-node clusters.** Someone deploys RF2 "to save a node". A single node loss stops all writes
  (no majority of 2). Always odd numbers: 3 or 5.
- **All replicas in one rack or zone.** RF3 with all three tservers on one hypervisor means one
  host failure loses quorum for everything. YugabyteDB placement policy (`cloud.region.zone`) must
  put one replica per zone or per failure domain.
- **Clock problems look like Raft problems.** Leases depend on clocks advancing at similar rates;
  a VM paused for 30 seconds wakes up with a lease it believes is valid only if the code uses the
  wrong clock. YugabyteDB uses monotonic time for leases and hybrid time for MVCC; still, monitor
  chrony on every node.
- **PostgreSQL failover runbook**: whoever owns HA PostgreSQL must be able to say what fences the
  old primary. If the answer is "we would notice", it is not fenced.
- **Your migration**: when you moved the bank's data from Berkeley DB (a single-process embedded
  store with no replication) into YugabyteDB RF3, you moved from "durability = one disk" to
  "durability = a majority of three nodes' Raft logs". That is a sentence worth saying in an
  interview.

## 6. Try it

1. **(easy)** Build and run `m11_raft_election_sim` with three different seeds. For each, record
   the time of the first election and the term after the partition heals. Done when: you can
   explain why the term numbers differ between seeds.

2. **(easy)** Change the cluster to 4 nodes and partition it 2-2. Done when: you see that neither
   side elects a leader and can explain it with the majority rule.

3. **(medium)** Set `kElectionMin = kElectionMax = 150` (no randomisation) and count elections and
   terms until a stable leader appears. Done when: you observe split votes and can explain why
   randomisation fixes them.

4. **(medium)** Add **check-quorum** to the leader: if it has not received a successful
   AppendEntries reply from a majority within one election timeout, it steps down. Done when: in
   step 5 of the scenario only one node claims leadership after 1.5 s, and the assertion that
   expects two leaders is updated.

5. **(hard)** Add **pre-vote**: a candidate first sends `PreVote` with `term + 1` without
   incrementing its own term, and only starts a real election if a majority would grant it.
   Done when: a node isolated for 5 seconds rejoins without causing an election (count terms
   before and after).

6. **(hard)** Implement **single-server membership change**: a `ConfigChange` log entry that adds
   node 5 to a 5-node cluster (making 6), taking effect as soon as it is appended. Add node 5 as a
   learner first and promote it only when its `matchIndex` reaches the leader's commit index.
   Done when: the safety fuzz still passes with random add/remove of one node at a time.

## 7. Interview questions

**Q: Explain Raft leader election.**
A: Followers expect heartbeats; one whose randomised timeout expires becomes a candidate,
increments the term, votes for itself and asks for votes. Each node votes once per term and only
for a candidate whose log is at least as up to date. A majority makes a leader, which sends
heartbeats to suppress further elections. Split votes are retried with new random timeouts.

**Q: When is an entry committed in Raft?**
A: When the leader that created it in its current term has stored it on a majority of nodes; the
leader then advances its commit index, applies it and answers the client. Entries from earlier
terms are committed indirectly when a current-term entry after them commits, which is why new
leaders append a no-op.

**Q: Why can't Raft have two leaders in the same term?**
A: Each node votes at most once per term and persists that vote, and a leader needs a majority.
Two majorities of the same cluster always overlap, so two candidates cannot both get a majority in
one term. Two nodes may believe they are leader in different terms, but the older one cannot
commit because its majority has moved to the higher term.

**Q: What is the term used for?**
A: It is a logical clock and fencing token: every message carries it, higher terms make nodes step
down, lower-term messages are rejected. It identifies stale leaders and orders log entries.

**Q: How does a lagging or divergent follower catch up?**
A: The leader's AppendEntries carries the previous index and term; the follower rejects on
mismatch; the leader backs up nextIndex until the logs agree, then the follower truncates
conflicting entries and appends the leader's. Very far behind followers get a snapshot.

**Q: Why does YugabyteDB need Raft when PostgreSQL does not?**
A: PostgreSQL has a single writable primary and leaves failover and split-brain prevention to
external tools like Patroni (which themselves use etcd's Raft). YugabyteDB shards every table
into tablets spread across nodes, and each tablet must elect leaders and replicate synchronously
and safely on its own, so each tablet is a Raft group.

**Q: How do you change cluster membership safely?**
A: Never switch all nodes at once, because old and new majorities might not intersect. Either
joint consensus (decisions need majorities of both configurations) or one server at a time. Add new
nodes as non-voting learners until caught up.

**Q: Paxos versus Raft?**
A: Same fault tolerance (2f + 1 nodes for f crash failures) and equivalent core: Raft terms are
ballots, election is phase 1, AppendEntries is phase 2. Raft specifies a strong leader, log
structure, election and membership in one design, which makes it easier to implement correctly.

## 8. Recap

- Consensus = agree on one ordered log; state machine replication applies it identically everywhere.
- Roles: follower, candidate, leader. Terms only increase and fence stale leaders.
- Election: randomised timeout, one persisted vote per term, up-to-date log restriction, majority.
- Replication: AppendEntries with prevIndex/prevTerm consistency check; truncate conflicts.
- Committed = on a majority, created in the leader's current term; no-op at term start.
- Safety: election safety, append-only leader, log matching, leader completeness, state machine safety.
- Membership: joint consensus or one server at a time; learners first.
- YugabyteDB: one Raft group per tablet (Multi-Raft), leaders spread, Raft log is the WAL,
  leader leases for reads, masters are a Raft group too.
- PostgreSQL streaming replication is replication without consensus; Patroni adds it via etcd.
- Paxos: same power, older, less prescriptive. Raft is Multi-Paxos made concrete.
