# 12. Sharding, partitioning and distributed SQL

**Time:** ~3 hours   **Prerequisites:** 10, 11; `07_Databases_Internals/11_yugabytedb_and_distributed_sql.md` helps

## 1. In plain words

**Sharding** (also called **partitioning**) splits one big table across many machines so that no
single machine has to store all the data or take all the writes. Each piece is a **shard**
(YugabyteDB calls it a **tablet**, CockroachDB a **range**, Cassandra a **token range**). The
**shard key** decides which shard a row lives in. Replication (modules 10 and 11) makes copies of
each shard; sharding decides how to cut the data in the first place. Real systems do both: many
shards, each replicated three times.

Analogy: a bank's paper archive. One room cannot hold every customer file, so you spread them over
ten rooms. You could file by the first letter of the surname (**range** partitioning: A to C in
room 1, and so on), which makes "all customers named B..." easy but leaves the S room overflowing.
Or you could compute a number from the customer id and use it to pick a room (**hash**
partitioning), which spreads files evenly but makes "all customers named B..." a visit to every
room. Either way, a transfer between two customers in different rooms needs two clerks to
coordinate, which is what distributed transactions are about.

**Distributed SQL** (YugabyteDB, CockroachDB, Spanner, TiDB) is the promise that you keep writing
ordinary SQL with ACID transactions while the database does sharding, replication and
distributed transactions underneath. The promise mostly holds, but the cost model changes: a query
that touched one disk page on PostgreSQL may now touch three machines. This module is about
knowing where that cost comes from and designing schemas that avoid it.

## 2. The real thing

### 2.1 Hash sharding vs range sharding

**Hash sharding**: compute `hash(shard_key)`, and each shard owns a range of hash values.

- YugabyteDB YSQL default: the first primary-key column is hashed (`PRIMARY KEY (account_no HASH)`
  or just `PRIMARY KEY (account_no)`). The hash is a 16-bit value in `[0x0000, 0xFFFF]`, and each
  tablet owns a contiguous slice of that space. A table created with 6 tablets gets slices of about
  65536 / 6 hash values each.
- Pros: even spread of data and writes, even for sequential keys (`ACC0000001`, `ACC0000002` land
  on different tablets). No hotspot from monotonically increasing ids.
- Cons: range scans on the shard key are scatter-gather (every tablet); `ORDER BY shard_key LIMIT
  10` must merge from all tablets.

**Range sharding**: each shard owns a contiguous range of the key itself.

- YugabyteDB: `PRIMARY KEY (txn_date ASC, txn_id ASC)` uses range sharding. CockroachDB, TiDB,
  Spanner, HBase and Bigtable are range-sharded by default.
- Pros: range scans and ordered scans hit only the shards covering the range; `WHERE txn_date
  BETWEEN ...` is cheap. Natural for time series and prefix queries.
- Cons: **hotspots** for monotonically increasing keys: every new row (latest timestamp, next
  sequence value) goes to the last shard, which takes all the write load.

**Compound keys** combine both: `PRIMARY KEY ((account_no) HASH, txn_ts ASC, txn_id ASC)` hashes
on the account (spread across tablets) and sorts within the account by time (one account's
statement is a single-tablet ordered range scan). This is the most important pattern in this module.

**Consistent hashing** (section 3) is the third family: shards are points on a hash ring and nodes
own arcs. Used by Dynamo and Cassandra, and in caches and load balancers. YugabyteDB does not
need it because it moves whole tablets (fixed hash slices) between nodes instead of re-hashing
keys; the effect is similar: adding a node moves about 1/N of the data.

**Directory-based** (lookup table) sharding: a metadata service maps key ranges or tenants to
shards explicitly. Flexible (move one big tenant to its own shard) at the cost of a lookup and a
metadata service. YugabyteDB's yb-master tablet map is a directory of tablet boundaries; clients
cache it.

### 2.2 Hotspots

A **hotspot** is a shard that gets much more traffic than its peers. Causes and fixes:

| Cause | Example | Fix |
|-------|---------|-----|
| Monotonic key with range sharding | `txn_id` from a sequence, `created_at` timestamp as first key column | Hash the first column; or prefix with a hash bucket (`bucket = hash(id) % 16`) |
| Low-cardinality shard key | `branch_code` with 250 branches, one head office branch doing 30% of volume | Shard on a higher-cardinality key (`account_no`), keep branch as a secondary column or index |
| Celebrity key | One merchant's settlement account credited by every card transaction | Split the counter: N sub-accounts summed on read; or batch postings and apply periodically |
| Hot range from a batch | End-of-day interest job updating accounts in key order | Process in hash order or in parallel across shards; throttle |
| Secondary index on a monotonic column | Index on `created_at` | Hash-shard the index (`(created_at) HASH` is useless for ranges) or bucket it |

How you see a hotspot: one tablet leader's CPU and RPC queue much higher than others; per-tablet
write metrics; latency only on certain keys. In YugabyteDB, the tablet server UI and
`yb_local_tablets` / per-tablet metrics show it.

### 2.3 Tablet splitting

A table starts with some number of tablets. As it grows, tablets must split, or one will hold
gigabytes and be both a hotspot and slow to move.

- **Pre-splitting**: decide at creation time. `CREATE TABLE ... SPLIT INTO 24 TABLETS` (hash) or
  `SPLIT AT VALUES ((...), (...))` (range). Best when you know the size up front, for example
  before loading a migration. By default YugabyteDB picks a tablet count based on the number of
  tservers and cores (flags such as `ysql_num_shards_per_tserver`; check your version).
- **Automatic tablet splitting**: YugabyteDB splits a tablet when it passes size thresholds that
  depend on how many tablets the table already has (small tables split early, big ones later).
  Enabled by default in recent releases. The split is local and cheap: the tablet's data is not
  rewritten; the two children initially share the parent's RocksDB files (hard links) and filter
  by key range, and compaction cleans up later. Each child is a new Raft group.
- **Manual split**: `yb-admin split_tablet <tablet_id>` for an operator-driven split of a known hot
  tablet.
- **Range tables and splitting**: a range-sharded table with a monotonic key will keep splitting
  its last tablet, but the new last tablet still takes all inserts. Splitting fixes size, not a
  write hotspot on the latest key.
- After splits, the **load balancer** moves tablet replicas and leaders between tservers to even
  out counts.

### 2.4 Two-phase commit (2PC)

A transaction that writes rows on two shards (debit account A on tablet 1, credit B on tablet 2)
must commit on both or neither. **Two-phase commit**:

```
 coordinator                       participant 1              participant 2
     |--- PREPARE ----------------->|                          |
     |--- PREPARE ------------------------------------------->|
     |                    writes data + "prepared" durably, holds locks
     |<-- YES ----------------------|                          |
     |<-- YES -------------------------------------------------|
     | writes COMMIT decision durably  (the commit point)
     |--- COMMIT ------------------>|                          |
     |--- COMMIT --------------------------------------------->|
     |<-- ack ----------------------|<-- ack -------------------|
```

Rules: a participant that voted YES must be able to commit even after a crash (it made its
writes durable), and must **wait** for the decision; it cannot decide on its own. If any
participant votes NO or times out, the coordinator aborts.

**The weakness: blocking.** If the coordinator crashes after participants voted YES but before
they learn the decision, they are stuck holding locks: they cannot commit (maybe someone voted NO)
and cannot abort (maybe the coordinator already decided COMMIT and told someone). Classic XA
transactions across two different databases have this problem, and operators end up resolving
"in-doubt" transactions by hand. Three-phase commit tries to fix it but is not safe under network
partitions, so nobody uses it.

**The fix used by distributed SQL**: make the coordinator's decision record **itself replicated by
consensus**. If the coordinator node dies, the decision is not lost; it is on a majority of the
Raft group that stores it, and any node can read it. Spanner runs 2PC across Paxos groups;
CockroachDB stores a transaction record in a Raft range; YugabyteDB uses a **transaction status
tablet**.

### 2.5 YugabyteDB's distributed transactions

For a transaction that touches more than one tablet (the single-tablet case takes a fast path with
no status tablet):

1. **Begin**: the transaction gets an id and a home **transaction status tablet** (tablets of a
   system `transactions` table, Raft-replicated like any other). A status record `PENDING` is
   created there.
2. **Writes**: each write goes to the tablet owning the row as a **provisional record** (an
   **intent**), stored in that tablet's separate IntentsDB alongside the transaction id. Intents act
   as locks: a conflicting transaction sees them. They are replicated via that tablet's Raft group,
   so they are durable.
3. **Commit**: the coordinator (the tserver handling the client session) writes `COMMITTED` with a
   commit hybrid time into the **status tablet** record. That single Raft-replicated write is the
   **commit point**. Atomic, because it is one record.
4. **Apply**: asynchronously, the participant tablets are told to convert intents into regular
   versioned records at the commit hybrid time and remove the intents. The client already got OK.
5. **Readers meeting an intent** look up the transaction's status record: committed (with time) ->
   treat as visible if the commit time is within their snapshot; pending -> not visible (or conflict,
   depending on isolation); aborted -> ignore and clean up.

Why this does not block like classic 2PC: the decision lives in a Raft group, so the loss of any
one node (coordinator included) does not lose or hide it. A transaction whose coordinator vanishes
before deciding is eventually aborted by heartbeat expiry on the status tablet.

Concurrency control: conflicts between transactions are detected by intents. Older YugabyteDB
versions used optimistic "fail-on-conflict" (one transaction gets a serialization error and the
client retries, SQLSTATE `40001`); newer versions default to **wait-on-conflict** with distributed
deadlock detection, closer to PostgreSQL behaviour. Either way, your application must retry
`40001` errors. Default isolation in YSQL is snapshot (REPEATABLE READ); SERIALIZABLE and READ
COMMITTED are available (see `07_Databases_Internals/11_yugabytedb_and_distributed_sql.md`).

### 2.6 Distributed indexes

In PostgreSQL an index lives on the same machine as the table. In distributed SQL, **a secondary
index is its own sharded table**, keyed by the indexed columns, with the primary key as the value
(or part of the key). Consequences:

- **Writes**: inserting a row with two secondary indexes writes three tablets (row, index 1, index
  2), probably on three different nodes. That is a distributed transaction (unless everything is
  colocated). Every extra index makes writes slower, more than on a single node.
- **Reads via index**: look up the index tablet, get primary keys, then fetch rows from the main
  table's tablets: two network hops, possibly to many tablets. A **covering index** (`CREATE INDEX
  ... ON t (customer_id) INCLUDE (status, amount)`) answers from the index alone: one hop
  (index-only scan).
- **Unique indexes** are enforced globally: the index tablet for that value rejects a duplicate,
  regardless of which shard the row lives in. Works, and costs a distributed transaction.
- **Index sharding matters**: an index on `created_at` should be range-sharded (`created_at ASC`)
  if you query ranges, which makes it a write hotspot. Common compromise: hash on a bucket or on
  a higher-cardinality leading column.
- **Local vs global indexes**: some systems (Cassandra, Elasticsearch, partitioned PostgreSQL)
  have **local** indexes per shard; queries on them scatter to all shards. YugabyteDB secondary
  indexes are **global**.

### 2.7 Cross-shard joins and their cost

A join between two tables on two sets of tablets means the query layer pulls rows over the network.
Cost drivers: number of round trips, rows shipped, and fan-out (how many tablets each step touches).

- **Nested loop with a remote inner side**: one round trip per outer row. 10,000 outer rows times
  1 ms is 10 seconds. The classic distributed SQL performance bug.
- **Batched nested loop join**: send the inner lookups in batches (`IN (k1, ..., k1024)`).
  YugabyteDB has this (`yb_bnl_batch_size`); one round trip per batch.
- **Hash join**: pull both sides to the query node and join in memory. Good when both sides are
  filtered small.
- **Pushdown**: filters, projections, aggregates and `LIMIT` pushed to the tablets so less data
  crosses the network. `EXPLAIN (ANALYZE, DIST)` in YugabyteDB shows storage read requests and rows
  per step.
- **Avoid the join's network cost by design**:
  - **Colocation**: put small, frequently joined reference tables (branch master, product codes,
    currency table, GL chart) into one colocated tablet (`CREATE DATABASE ... WITH COLOCATION =
    true`, or per-table options). One Raft group, one hop.
  - **Same shard key prefix**: `account` and `account_txn` both hashed on `account_no`. Each
    query becomes "one account", so it touches one tablet per table. Note: YugabyteDB does not
    guarantee that two tables' tablets for the same hash range live on the same node, so this
    reduces fan-out, not hops.
  - **Denormalise** the few columns the hot path needs (branch name on the statement row).

### 2.8 Choosing shard keys for banking tables

Banking schemas are full of **business codes**: branch code, product code, customer id, account
number, loan application number, GL code. The questions to ask for each table:

1. What is the most frequent access path? (By account? By customer? By branch and date?)
2. Is the key's cardinality high enough to spread load? (250 branches: no. 10 million accounts: yes.)
3. Is it monotonic? (Sequence-generated application numbers: yes. Hash them.)
4. Which tables are joined on the hot path? (Give them the same leading key.)
5. Is the table small and static? (Colocate it.)

Worked example:

```sql
-- Reference data: small, read everywhere, joined constantly. Colocate.
CREATE TABLE branch (
    branch_code  text PRIMARY KEY,
    branch_name  text NOT NULL,
    region_code  text NOT NULL
);  -- in a colocated database, or WITH (COLOCATION = true)

-- Customers: accessed by customer id. Hash on it.
CREATE TABLE customer (
    customer_id  text,
    branch_code  text NOT NULL,
    full_name    text NOT NULL,
    PRIMARY KEY (customer_id HASH)
);

-- Accounts: accessed by account number. Hash on account_no, not branch_code
-- (branch_code is low-cardinality and skewed: head office branches are huge).
CREATE TABLE account (
    account_no     text,
    customer_id    text NOT NULL,
    branch_code    text NOT NULL,
    product_code   text NOT NULL,
    balance_minor  bigint NOT NULL,
    PRIMARY KEY (account_no HASH)
);
CREATE INDEX account_by_customer ON account (customer_id HASH) INCLUDE (account_no, product_code);

-- Transactions: the statement query is "one account, newest first". Hash on account,
-- range on time within the account: one tablet, already sorted.
CREATE TABLE account_txn (
    account_no     text,
    txn_ts         timestamptz,
    txn_id         bigint,
    branch_code    text   NOT NULL,   -- denormalised from account for the branch report
    txn_date       date   NOT NULL,   -- business date, not wall-clock date
    amount_minor   bigint NOT NULL,
    narration      text,
    PRIMARY KEY ((account_no) HASH, txn_ts DESC, txn_id DESC)
);

-- Branch end-of-day report needs "all transactions of a branch on a date". Do not make
-- branch_code the shard key of the table for that: use a separate index (or a reporting copy).
-- If one large branch's day makes a single index tablet hot, add a small bucket column
-- (hash(txn_id) % 8) to the hash part of the index and query all 8 buckets.
CREATE INDEX txn_by_branch_day ON account_txn ((branch_code, txn_date) HASH, txn_id);

-- Loan applications: numbers come from a sequence (monotonic). Hash shard so inserts spread.
CREATE TABLE loan_application (
    application_no  bigint,
    branch_code     text NOT NULL,
    status          text NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (application_no HASH)
);
```

Notes that show seniority:
- **Sequences in distributed SQL**: a sequence is a single row somewhere; every `nextval` would be
  a network round trip. YugabyteDB caches sequence values per connection (`CACHE` clause and a
  server-side minimum cache). Consequence: values are unique but not gap-free and not in commit
  order across connections. If a regulator needs gap-free voucher numbers, generate them
  differently (per-branch counter row updated in the transaction, accepting that it serialises
  that branch).
- **Money columns**: integer minor units (`bigint` paise) or `numeric`, never `float`.
- **Uniqueness of business codes**: a unique index on `(branch_code, voucher_no)` is a global
  index: correct, but each insert is a distributed transaction.

## 3. Code

### 3.1 Consistent hashing ring (`code/m12_consistent_hash.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m12_consistent_hash.cpp -o chash && ./chash
```

The ring is a `std::map<uint64_t, std::string>` from position to node. Each physical node is
inserted `vnodes` times (`"node-3#0"`, `"node-3#1"`, ...). Lookup is `lower_bound` with wrap-around:

```cpp
const std::string& owner(std::string_view key) const {
    auto it = ring_.lower_bound(hash64(key));
    if (it == ring_.end()) it = ring_.begin();   // wrap around the circle
    return it->second;
}

// Replicas: walk clockwise collecting DISTINCT physical nodes (Dynamo's preference list).
std::vector<std::string> replicas(std::string_view key, size_t rf) const;
```

What the program measures and asserts on 200,000 banking-style keys (`BR017-ACC0004242`):

1. **Balance vs virtual nodes**: with 1 point per node the most loaded node gets well over 1.5 times
   the mean (in a typical run); with 200 points per node it is within about 20% of the mean. The
   assert requires under 1.25.
2. **Adding a sixth node** moves about 1/6 of keys (asserted between 10% and 25%), and every moved
   key moves **to the new node**. The same change with `hash % N` moves about 5/6 of keys.
3. **Removing a node** moves only that node's keys, and they spread over the survivors instead of
   all landing on one neighbour (a benefit of virtual nodes).
4. **RF=3 preference lists** contain three distinct physical nodes, the first being the owner.

The hash is FNV-1a followed by the splitmix64 finalizer. Plain FNV on near-identical strings like
`node-1#0`, `node-1#1` clusters badly; a good finalizer matters more than the base hash.

### 3.2 Seeing sharding in YugabyteDB

```sql
-- Which hash value does a key map to? (YugabyteDB function.)
SELECT yb_hash_code('ACC0004242'::text);           -- a number in [0, 65535]

-- Tablet layout of a table (yugabyted / yb-admin), from a shell:
-- yb-admin -master_addresses <m1:7100,...> list_tablets ysql.bankdb account

-- Does this query touch one tablet or all of them?
EXPLAIN (ANALYZE, DIST) SELECT * FROM account_txn
WHERE account_no = 'ACC0004242' ORDER BY txn_ts DESC LIMIT 20;
-- Look for one storage read request and an Index Scan on the primary key with no Sort node.
```

## 4. Under the hood (senior-level)

**How a YSQL row becomes a DocDB key.** For `PRIMARY KEY ((account_no) HASH, txn_ts DESC,
txn_id DESC)`, the DocDB key is `[16-bit hash of account_no][account_no][txn_ts desc-encoded]
[txn_id desc-encoded][column id] + hybrid time`. The 16-bit hash prefix decides the tablet; the
rest keeps rows of one account sorted inside the tablet's RocksDB. The hash code is stored in the
key, which is why `yb_hash_code` can be used for parallel full scans by hash range.

**Why automatic splitting is cheap.** RocksDB SST files are immutable. A split creates two child
tablets that reference the parent's files with key-range bounds; no data is copied at split time.
Compactions later rewrite each child's files to drop keys outside its range. The expensive part is
afterwards: moving one child's replicas to another node, which is a file copy (remote bootstrap).

**The cost of a distributed transaction, counted.** A two-tablet write with RF3 in one region: two
intent writes (each a Raft round: leader fsync plus one follower), one status-tablet write (one
more Raft round), then async apply (more Raft rounds, off the critical path). Typical
single-region latency for that is a few milliseconds versus around a millisecond for a single-row
fast-path write (typical; measure your own). Across regions every Raft round becomes a WAN round
trip, which is why geo-distributed deployments place leaders (preferred zones) and use tablespaces
to keep a row's replicas close to its users.

**2PC atomicity vs isolation.** 2PC gives **atomicity** across shards. **Isolation** comes from
MVCC plus intents plus hybrid-time snapshots: a reader at snapshot time T sees a committed
transaction's writes on every tablet or on none, because all of them carry the same commit hybrid
time. That is why a distributed snapshot needs a cluster-wide notion of time (HLC, module 10).

**Consistent hashing internals.** With V virtual nodes per server, the load on a server is the sum
of V roughly independent arc lengths, so its relative standard deviation shrinks about as
1/sqrt(V). Memory is O(N V) and lookup O(log(N V)). Alternatives: **jump consistent hash** (Lamping
and Veach, Google, 2014: no memory, perfect balance, but buckets can only be added or removed at
the end), **rendezvous (highest random weight) hashing** (score every node per key, take the top:
O(N) per lookup, trivially gives replica lists), and **Maglev hashing** (Google's load balancer:
a lookup table with minimal disruption).

**Resharding with a fixed hash space.** YugabyteDB and Redis Cluster (16384 slots) avoid
re-hashing keys entirely: the hash space is fixed and only the assignment of slices to nodes
changes. Moving a slice is a bulk copy plus a cutover of ownership, which is simpler to reason about
than re-hashing a ring.

## 5. In production

- **The head-office hotspot.** A schema keyed by `(branch_code, account_no)` with `branch_code`
  hashed put the head office's millions of accounts in one tablet's slice. Postings for that
  branch had high latency while others were fine. Re-keying to `account_no HASH` (with a
  secondary index on branch) fixed it. The lesson: never let a skewed business code be the hash
  key alone.
- **Migration load into too few tablets.** A bulk load into a freshly created table with the
  default tablet count drove a few tablets at a time and kept splitting during the load. Pre-split
  the table (`SPLIT INTO N TABLETS`, sized to tservers times cores) before loading, load in hash
  order or in parallel streams, and create secondary indexes after the bulk load where possible.
  Your Berkeley DB to YugabyteDB migration engine is exactly this workload.
- **The N+1 query over the network.** An ORM loaded 500 accounts and then each account's last
  transaction in a separate query: 501 round trips, fine on PostgreSQL on localhost, slow on a
  three-node cluster. Batch (`WHERE account_no = ANY($1)`) or join.
- **Serialization retries not handled.** Moving to distributed SQL surfaced `40001` errors under
  contention that the old single-node code never saw. The app must retry the whole transaction
  with backoff (module 09), and the transaction must be idempotent or keyed.
- **Unique constraint on a monotonic business code.** `UNIQUE (voucher_no)` where vouchers are
  issued sequentially: the unique index is range-hot. Hash it or include a bucket prefix.
- **Cross-region transactions.** A table with replicas in two regions and leaders in one: every
  write from the other region pays a WAN round trip per Raft round. Use leader preference per
  tablespace so each region's data is led locally.

## 6. Try it

1. **(easy)** Build and run `m12_consistent_hash`. Change `vnodes` to 1, 5, 20 and 500 and record
   max/mean. Done when: you have a small table and can explain the 1/sqrt(V) trend.

2. **(easy)** For five tables in your employer's schema (or the examples in section 2.8), write the
   shard key you would choose and one sentence why. Done when: each answer names the access path
   and the cardinality.

3. **(medium)** Add jump consistent hash (`int32_t jump(uint64_t key, int32_t buckets)`, eight
   lines from the paper) to the program and measure balance and movement when going from 5 to 6
   buckets. Done when: you can state when you would use jump hash instead of a ring.

4. **(medium)** On a local YugabyteDB (`yugabyted start` in Docker or WSL, three nodes with
   `--join`), create `account_txn` from section 2.8, load 1 million rows across 10,000 accounts,
   and compare `EXPLAIN (ANALYZE, DIST)` for "one account's last 20 transactions" with a version
   keyed `PRIMARY KEY (txn_id HASH)`. Done when: you can point at the read requests and sort
   node that differ.

5. **(hard)** Write a 2PC simulation: one coordinator, three participants, each with a durable
   log (vector). Inject coordinator crashes after PREPARE. Show participants blocked holding
   locks. Then move the decision into a "replicated" status record (reuse the Raft simulation or
   just a majority-of-three array) and show recovery. Done when: asserts prove atomicity in
   both versions and the blocking case is visible in the first.

6. **(hard)** Design the sharding and colocation plan for a loan-origination schema with 12
   tables (applications, applicants, documents, credit checks, approvals, disbursements, reference
   tables). For each table state shard key, hash or range, colocated or not, secondary indexes,
   and the hot query each choice serves. Done when: every hot query touches one tablet per table
   or you have written why it cannot.

## 7. Interview questions

**Q: Hash versus range sharding?**
A: Hash sharding spreads keys evenly by hash, which avoids hotspots on sequential keys but makes
range scans touch every shard. Range sharding keeps keys sorted so range scans are cheap, but
monotonic keys hit the last shard. Compound keys (hash on the entity, range on time within it) get
both for the common "one entity's history" query.

**Q: What causes hotspots and how do you fix them?**
A: Monotonic keys under range sharding, low-cardinality or skewed shard keys, celebrity rows, and
batch jobs walking keys in order. Fix by hashing or bucketing the key, choosing a higher-cardinality
shard key, splitting hot counters into sub-rows, and spreading batch work across shards.

**Q: How does consistent hashing work and why use virtual nodes?**
A: Nodes and keys hash onto a ring; a key belongs to the next node clockwise. Adding or removing a
node moves only about 1/N of keys. Virtual nodes (many points per server) even out arc sizes and
spread a dead node's keys over many survivors instead of one neighbour.

**Q: Explain two-phase commit and its weakness.**
A: The coordinator asks all participants to prepare (make writes durable, hold locks, vote); if all
vote yes it records commit and tells them, otherwise abort. If the coordinator dies after the votes
and before the decision reaches participants, they block holding locks. Distributed SQL fixes this
by storing the decision in a consensus-replicated record.

**Q: How does YugabyteDB commit a multi-shard transaction?**
A: Writes go to each tablet as provisional records (intents) replicated by that tablet's Raft
group. Commit is a single Raft-replicated write of COMMITTED with a commit hybrid time to the
transaction's status tablet. Intents are applied to regular records asynchronously, and readers
that meet an intent check the status tablet.

**Q: Why are secondary indexes more expensive in distributed SQL?**
A: Each index is its own sharded table, usually on other nodes, so a write with indexes becomes a
distributed transaction across tablets, and a non-covering index read takes two hops. Use covering
indexes for hot reads and keep index count low on write-heavy tables.

**Q: How would you choose shard keys for a core-banking schema?**
A: By the hot access path and cardinality: accounts hashed on account number, transactions hashed
on account number with time as a range component, customers on customer id, small reference tables
colocated. Avoid skewed business codes like branch code as the only hash key, and hash anything
sequence-generated.

## 8. Recap

- Sharding splits data; replication copies each shard. Distributed SQL does both under SQL.
- Hash: even spread, scatter for ranges. Range: cheap scans, hot tail for monotonic keys.
  Compound `(entity) HASH, time` covers both for entity histories.
- YugabyteDB: 16-bit hash space split across tablets; each tablet a Raft group; auto splitting
  is cheap (shared SST files); pre-split before bulk loads.
- Consistent hashing moves ~1/N keys on membership change; virtual nodes balance it.
- 2PC blocks if the coordinator dies; storing the decision in a Raft group removes the blocking.
- YugabyteDB transactions: intents + status tablet commit record + async apply; retry `40001`.
- Secondary indexes are global sharded tables: writes become distributed, covering indexes save a hop.
- Cross-shard joins cost round trips: batch, push down, colocate, share shard key prefixes.
- Banking keys: high-cardinality business codes (account, customer) as hash keys; branch code
  as a column or index, not the shard key.
