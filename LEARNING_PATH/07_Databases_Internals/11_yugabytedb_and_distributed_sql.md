# 11. YugabyteDB and distributed SQL

**Time:** ~3 hours   **Prerequisites:** 03, 05, 06, 10 (and 08_Networking_Distributed for Raft)

## 1. In plain words

YugabyteDB is PostgreSQL's upper half (parser, planner, executor, PL/pgSQL, the wire protocol)
bolted onto a different lower half: instead of heap files and WAL on one machine, every table is
cut into **tablets** (shards), each tablet is stored in its own RocksDB instance, and each
tablet is copied to three nodes that agree on every write using the **Raft** consensus protocol.
The result speaks PostgreSQL, survives a node dying without losing a committed transaction, and
spreads data and load across machines.

The analogy: PostgreSQL is one bank branch with one vault. YugabyteDB is a chain of branches
where every account's folder is kept in three branches at once; any change is written in all
three before it counts, and a chosen branch (the leader) coordinates each folder. Customers still
fill in the same forms (SQL), but every form now involves a phone call or two between branches
(network hops), and where a folder lives (which branch) is decided by a hash of the account number
unless you ask otherwise. That phone call is why a query that was fine on one machine can be slow
on the cluster, and why index design changes.

You ran production on YugabyteDB. This module explains what the engine was doing, so that
"multi-node cluster" on your resume is a thing you can draw on a whiteboard.

## 2. The real thing

### 2.1 Architecture

```
  clients (libpq, JDBC ...) --- PostgreSQL wire protocol, port 5433 ---> any node
                                                                           |
  +-------------------------- node 1 ------------------------+   +------ node 2 ------+  +--- node 3 ---+
  | YB-TServer process                                       |   | YB-TServer         |  | YB-TServer   |
  |  +-------------------------------+                       |   |  postgres (YSQL)   |  |  postgres    |
  |  | postgres backend (YSQL layer) |  parser, analyzer,    |   |  DocDB tablets:    |  |  DocDB:      |
  |  |  = modified PostgreSQL        |  planner, executor,   |   |   T1 follower      |  |   T1 follower|
  |  +---------------+---------------+  PL/pgSQL, catalog    |   |   T2 leader        |  |   T2 follower|
  |                  | pggate (RPC to tablet servers)        |   |   T3 follower      |  |   T3 leader  |
  |  +---------------v---------------+                       |   +--------------------+  +--------------+
  |  | DocDB: tablets hosted here    |                       |
  |  |  tablet T1 (leader)  -> Raft log + RocksDB (regular + intents)                   |
  |  |  tablet T2 (follower)-> Raft log + RocksDB                                       |
  |  |  tablet T3 (follower)                                                            |
  |  +-------------------------------+                       |
  +----------------------------------------------------------+
  YB-Master processes (3, Raft group): system catalog location, tablet-to-node map,
  load balancing, DDL coordination, tablet splitting.
```

- **YB-TServer**: one per node; hosts tablets and runs the YSQL (PostgreSQL) processes. Any
  node accepts connections; the backend that receives your query is the **coordinator** for it.
- **YSQL**: a fork of PostgreSQL (long based on 11.2; recent releases moved to a PostgreSQL 15
  base). Same grammar, types, planner, PL/pgSQL, extensions that do not touch storage. The heap
  access method and B-tree AM are replaced by **pggate** calls that read and write DocDB.
- **DocDB**: the distributed document store. Each tablet is a Raft group with a leader and
  followers, and each replica is one RocksDB instance (actually two: **RegularDB** for committed
  data and **IntentsDB** for provisional writes of in-flight transactions). RocksDB's own WAL is
  disabled; the Raft log *is* the WAL.
- **YB-Master**: a small Raft group of three that keeps the catalog of tablets and their
  locations, runs the load balancer that moves tablet replicas and leaders between nodes, and
  coordinates DDL.
- **YCQL** is a separate Cassandra-compatible API on the same DocDB; ignore it unless asked.

### 2.2 How a row becomes keys in DocDB

A PostgreSQL row of `app_users (id PK, branch_code, user_code, status, ...)` is stored as a
**document** keyed by the primary key, with one sub-key per column (recent versions pack all
columns of a row into one value, "packed rows", on insert; updates still write per-column
entries until compaction repacks):

```
 DocKey: [hash(id) : 2 bytes][id]                      <- the partition
   SubKey: column_id(branch_code)  @ HT=t1  -> "BR0456"
   SubKey: column_id(user_code)    @ HT=t1  -> "U00123456"
   SubKey: column_id(status)       @ HT=t5  -> "LOCKED"      <- a later update: new version, newer timestamp
   SubKey: column_id(status)       @ HT=t1  -> "ACTIVE"
```

`HT` is a **hybrid timestamp** (physical clock microseconds plus a logical counter), DocDB's
transaction time. Multiple versions of a sub-key sit next to each other in the LSM, newest
first; a read at time T seeks to the first version <= T. Old versions are garbage-collected
during compaction after `timestamp_history_retention_interval_sec` (default 900 s). That is
MVCC without `xmin/xmax` and without VACUUM. Deletes are tombstones.

A **secondary index** is its own DocDB table: key = (indexed columns, primary key), value = the
INCLUDE columns, if any. An index lookup is therefore one read on the index tablet to get the
primary key, then one read on the main table's tablet (possibly on another node) to get the
row, unless the index covers the query.

### 2.3 Tablets and sharding

Every table and index is split into tablets. The split rule is set by the primary key (or index
key) definition:

- **Hash sharding** (YSQL default for the first PK column): the first column (or an explicit
  group) is hashed into a 16-bit space `[0, 65536)`, and each tablet owns a contiguous range of
  hash values. Rows are spread evenly across tablets, so inserts and point lookups scale
  linearly; but rows with adjacent key values live on different tablets, so a **range query on
  the hash column is a scatter to every tablet** and `ORDER BY` on it needs a sort.
- **Range sharding** (`ASC`/`DESC` on the first PK column): tablets own key ranges in sort
  order. Range scans and ordered reads touch few tablets, but a monotonically increasing key
  (timestamps, sequences) sends every insert to the last tablet: a **hotspot**.

```sql
CREATE TABLE events (                               -- default: hash on the first PK column
    id bigint, at timestamptz, payload jsonb,
    PRIMARY KEY (id HASH)                            -- explicit form of the default
);
CREATE TABLE audit_by_app (                         -- hash on application_id, range within
    application_id bigint, at timestamptz, note text,
    PRIMARY KEY (application_id HASH, at DESC)       -- all of one application's audit on one tablet, newest first
);
CREATE TABLE codes (                                -- range sharded: ordered scans over the whole key space
    code text, name text,
    PRIMARY KEY (code ASC)
) SPLIT AT VALUES (('F'), ('M'), ('T'));             -- pre-split to avoid a single tablet at start
```

Tablet count: by default a function of cores per node (often 8 per table per node on larger
machines, fewer on small ones; `ysql_num_shards_per_tserver`), with **automatic tablet
splitting** as tablets grow past a size threshold. Too many tablets per node (thousands) costs
memory and Raft heartbeat overhead; **colocation** (below) is the answer for many small tables.

### 2.4 Replication, leaders, consistency

Each tablet's three replicas (`replication factor` RF = 3; RF = 5 tolerates two failures) form a
Raft group. One is the **leader**; it serialises all writes to that tablet: append the entry to
its Raft log, send it to followers, and once a **majority** (2 of 3) has written it to their
logs, the entry is committed and applied to RocksDB. See 08_Networking_Distributed for the
protocol; what matters here:

- A committed write survives the loss of any one node, with no data loss. There is no
  asynchronous replica lag to reason about for correctness.
- A node failure makes the tablets it led unavailable for a few seconds (leader election,
  `raft_heartbeat_interval_ms` x `leader_failure_max_missed_heartbeat_periods`, typically 3 to 6 s
  by default) and then they continue. Clients see errors or latency for that window and retry.
- **Reads go to the leader** by default, so they are strongly consistent (linearisable per
  tablet). Leaders hold **leases** so they can serve reads without a Raft round trip.
  **Follower reads** (`SET yb_read_from_followers = true` plus a read-only transaction with
  `yb_follower_read_staleness_ms`) trade a bounded staleness for lower latency and load spreading.
- Replica placement is controlled by cloud/region/zone labels and **tablespaces**, so RF = 3
  across three zones survives a zone outage; across three regions it survives a region but
  every commit pays an inter-region round trip.

### 2.5 Distributed transactions

A single-row write whose row lives on one tablet takes a **fast path**: the leader applies it
through Raft, done, ~1 ms in-zone. Anything touching rows on more than one tablet (a multi-row
UPDATE, an insert into a table with secondary indexes, a FK check) is a **distributed
transaction**:

1. The coordinator picks a **transaction status tablet** and creates a status record.
2. Writes go to each involved tablet as **provisional records (intents)** in IntentsDB, with
   the transaction id; conflicts with other intents or committed data at a newer timestamp are
   detected here (and, depending on the conflict mode, the transaction waits or fails and retries).
3. `COMMIT`: the coordinator writes COMMIT with a commit hybrid timestamp to the status tablet
   (one Raft commit). The transaction is now durable and visible.
4. Asynchronously, each participant tablet **applies** intents into RegularDB with the commit
   timestamp and removes them.

Readers that meet an intent look up the status tablet to learn whether it committed (and when),
so they never see partial transactions. Isolation levels in YSQL:

| YSQL level | Meaning | Notes |
|---|---|---|
| `READ COMMITTED` | per-statement snapshot, like PostgreSQL | only when `yb_enable_read_committed_isolation = true`; otherwise it is silently treated as snapshot isolation |
| `REPEATABLE READ` = **Snapshot** (default) | one snapshot per transaction; conflicts abort with 40001 | the default differs from PostgreSQL's READ COMMITTED |
| `SERIALIZABLE` | pessimistic: reads take row locks in DocDB so conflicting writers wait or abort | not SSI; more aborts, no write skew |

Clock skew: hybrid timestamps assume node clocks differ by less than `max_clock_skew_usec`
(default 500 ms); reads whose window overlaps the uncertainty are retried internally. Run NTP
or chrony on every node; a node with a bad clock is refused.

### 2.6 What changes versus single-node PostgreSQL

| Topic | PostgreSQL | YugabyteDB (YSQL) |
|---|---|---|
| Primary key storage | heap + separate B-tree | the table *is* keyed by the PK in DocDB (clustered); no heap, no TID |
| Default PK sharding | n/a | **hash on the first column**: `WHERE id BETWEEN` and `ORDER BY id` fan out and sort |
| Point lookup cost | ~4 page reads, microseconds | one RPC to the tablet leader, ~1 ms in-zone, more cross-zone/region |
| Sequential scan | one file, OS read-ahead | parallel RPCs to every tablet leader, merged at the coordinator; still "fast enough" for small tables, terrible for login paths |
| Secondary index lookup | index pages + heap page | index tablet RPC + main table RPC (2 hops) unless covering |
| Secondary index write | same transaction, same page writes | turns a single-row insert into a distributed transaction |
| Sequences | per-session `CACHE 1`, gaps on rollback | cached per node (`ysql_sequence_cache_minval`, default 100): ids are not monotonic across nodes |
| Statistics | autovacuum runs ANALYZE | historically no autovacuum: **run `ANALYZE` yourself** after loads (auto-analyze arrived recently and may be off); without it the planner assumes 1000 rows per table and picks nested loops |
| VACUUM | required | not needed (compaction GC); `VACUUM` is a no-op |
| Isolation default | READ COMMITTED | Snapshot (REPEATABLE READ); client must retry 40001 |
| Large deletes | bloat until vacuum | tombstones slow scans until compaction; delete in batches, consider partition drop |
| Many small tables | fine | each is tablets x RF Raft groups: use **colocation** |
| DDL | transactional, fast | coordinated through YB-Master, slower, some ALTERs unsupported or non-transactional; `CREATE INDEX` is online with asynchronous backfill |
| Latency profile | sub-millisecond possible | per-statement floor of ~1 ms; **round trips dominate**: batch, use procedures |
| Hotspots | rarely matter | range-sharded time keys and `bigserial` with range sharding pile onto one tablet |
| xid wraparound | monitor | does not exist |

**Colocated tables**: a database created `WITH COLOCATION = true` puts all its tables (unless
opted out) into one tablet, so small reference tables (branches, roles, workflow_states,
products) are joined without fan-out and do not multiply Raft groups. Large tables opt out with
`WITH (COLOCATION = false)` and get their own tablets. For your schema: colocate everything
except `app_users`, `loan_applications`, `documents`, `payments`, `application_audit`.

### 2.7 Index design on a cluster

Rules that extend module 03:

1. **Choose the sharding of the index explicitly.** `CREATE INDEX ON app_users (branch_code,
   user_code)` hashes on `branch_code` by default: a lookup with both columns goes to exactly one
   tablet (good); a lookup by `branch_code` alone goes to one tablet (good); a range over
   `user_code` within a branch stays on one tablet (good, since the second column is range
   within the hash). Write it explicitly as `(branch_code HASH, user_code ASC)` so nobody has to
   guess. If you need range scans on the first column, use `ASC`, and accept the hotspot risk.
2. **Cover hot point lookups** with INCLUDE: an Index Only Scan is one RPC instead of two, and on
   a cluster the second RPC may cross a zone. The login index from module 03 is the example:

   ```sql
   CREATE UNIQUE INDEX app_users_login_idx
       ON app_users ((branch_code, user_code) HASH)          -- hash both: even spread, exact match goes to one tablet
       INCLUDE (password_hash, status, failed_attempts);
   ```

3. **Every secondary index makes writes distributed.** Three indexes on `app_users` turn a
   last-login update into a transaction across up to four tablets. Index only what you read.
   (Updates to non-indexed columns are cheap, the DocDB analogue of HOT.)
4. **Unique indexes cost an extra read** at insert time on the index tablet. Needed for
   correctness; know the cost.
5. **Hash on a low-cardinality column is a hotspot**: `status HASH` puts 98% of rows in the
   `ACTIVE` bucket on one tablet. Hash on the high-cardinality column, range on the rest, or use
   a partial index.
6. **Index backfill is online**: `CREATE INDEX` returns quickly, backfills in the background,
   and the index is unusable until `indisvalid`; check `pg_index` or the master UI before relying
   on it. A failed backfill leaves an invalid index to drop.
7. Partial and expression indexes work; GIN exists for jsonb; GiST, BRIN and hash index types
   do not exist (everything is a DocDB table; the index "type" is the key encoding).

### 2.8 Reading a multi-node EXPLAIN

```
EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';

 Index Only Scan using app_users_login_idx on app_users (actual time=1.102..1.104 rows=1 loops=1)
   Index Cond: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
   Heap Fetches: 0
   Storage Index Read Requests: 1                 <- one RPC to one tablet leader
   Storage Index Read Execution Time: 0.914 ms    <- time spent in DocDB including network
 Planning Time: 0.090 ms
 Execution Time: 1.150 ms
 Storage Read Requests: 1
 Storage Rows Scanned: 1
 Storage Write Requests: 0
 Catalog Read Requests: 0                         <- non-zero means the catalog cache missed (first call per connection)
 Storage Execution Time: 0.914 ms
```

Compare the pre-index plan:

```
 Seq Scan on app_users (actual time=118.414..2410.882 rows=1 loops=1)
   Storage Filter: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
   Storage Table Read Requests: 196                <- paged reads across every tablet
   Storage Table Read Execution Time: 2380.117 ms
   Storage Table Rows Scanned: 2000000
```

(Shape and magnitudes typical of a small 3-node docker cluster; numbers vary.) What to read:
**Storage Read Requests** (the number of RPC round trips; minimise), **Rows Scanned** versus rows
returned (filter pushdown efficiency: `Storage Filter` means DocDB applied it, `Filter` means
rows came back to the coordinator first), **Catalog Read Requests** (schema cache misses; a
connection pool with long-lived connections avoids them), and whether the scan is an
`Index Only Scan` (1 hop) or `Index Scan` (2 hops per row, batched as `Storage Table Read
Requests`). The plan tree itself is PostgreSQL's; the costs are PostgreSQL's constants with
YugabyteDB's cost model (`yb_enable_base_scans_cost_model` in newer versions) adjusting for RPCs.

## 3. Code

Start a cluster: `code/docker-compose.yml` runs a single-node `yugabyted` (enough for the API;
add nodes for the Raft behaviour), plus PostgreSQL and Redis.

```bash
cd code && docker compose up -d yugabyte
docker exec -it yb bin/ysqlsh -h yb            # PostgreSQL-compatible shell, port 5433
# or from WSL with the stock client:
psql -h localhost -p 5433 -U yugabyte -d yugabyte
# web UI: http://localhost:15433 (yugabyted) and http://localhost:7000 (master: tablets, leaders)
```

`code/m11_yugabyte_demo.sql` runs on YSQL:

```sql
-- 1. Colocated database for the reference tables, with the big tables opted out
CREATE DATABASE loans_yb WITH COLOCATION = true;
\c loans_yb
CREATE TABLE branches (code text PRIMARY KEY, name text NOT NULL);                    -- colocated
CREATE TABLE app_users (
    id bigint GENERATED ALWAYS AS IDENTITY,
    branch_code text NOT NULL, user_code text NOT NULL, password_hash text NOT NULL,
    status text NOT NULL DEFAULT 'ACTIVE', failed_attempts int NOT NULL DEFAULT 0,
    last_login_at timestamptz,
    PRIMARY KEY (id HASH)
) WITH (COLOCATION = false) SPLIT INTO 6 TABLETS;                                      -- its own tablets

INSERT INTO branches SELECT 'BR' || lpad(g::text, 4, '0'), 'Branch ' || g FROM generate_series(0, 499) g;
INSERT INTO app_users (branch_code, user_code, password_hash)
SELECT 'BR' || lpad((g % 500)::text, 4, '0'), 'U' || lpad(g::text, 8, '0'), md5(g::text)
FROM generate_series(1, 200000) g;                                                     -- keep it small for docker
ANALYZE app_users; ANALYZE branches;                                                   -- you must; nothing does it for you

-- 2. The scatter: no index on the natural key
EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';

-- 3. The fix, with explicit sharding and covering columns
CREATE UNIQUE INDEX app_users_login_idx ON app_users ((branch_code, user_code) HASH)
    INCLUDE (password_hash, status, failed_attempts);
-- wait for backfill: SELECT indisvalid FROM pg_index WHERE indexrelid = 'app_users_login_idx'::regclass;
EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';

-- 4. Hash PK means ORDER BY id sorts and ranges fan out
EXPLAIN (ANALYZE, DIST, COSTS OFF) SELECT id FROM app_users WHERE id BETWEEN 1000 AND 1100 ORDER BY id;

-- 5. Sequences are cached per node: ids jump
SELECT nextval(pg_get_serial_sequence('app_users', 'id')) FROM generate_series(1, 3);

-- 6. Where are the tablets? (yb_servers lists nodes; the master UI at :7000/tablet-servers shows leaders)
SELECT host, port, zone, region FROM yb_servers();
SELECT * FROM yb_table_properties('app_users'::regclass);                               -- num_tablets, is_colocated

-- 7. Snapshot isolation is the default: watch the error class
BEGIN;  SELECT current_setting('transaction_isolation');  ROLLBACK;                    -- repeatable read
-- in two sessions: both UPDATE the same row -> the second gets 40001 (or waits, depending on conflict mode)

-- 8. Follower reads for a read-only report
BEGIN TRANSACTION READ ONLY;
SET LOCAL yb_read_from_followers = true;
SET LOCAL yb_follower_read_staleness_ms = 30000;
SELECT count(*) FROM app_users;
COMMIT;
```

Everything from modules 07 and 08 (set-based SQL, PL/pgSQL procedures, `ON CONFLICT`) runs
unchanged on YSQL; only the cost model under it differs.

## 4. Under the hood (senior-level)

**The pggate boundary.** In the YSQL backend, the heap AM functions (`heap_insert`,
`heap_getnext`...) and index AM calls are replaced by `YBC*` functions that build DocDB requests
(`PgsqlReadRequest`, `PgsqlWriteRequest` protobufs), batch them, and send them to the right
tablet leader using the tablet location cache. Predicate pushdown: the executor sends WHERE
clauses the storage layer can evaluate (`Storage Filter`) so rows are filtered at the tablet, and
projections so only needed columns cross the network. Reads page through results with a
`ysql_prefetch_limit` (1024 rows per RPC); a Seq Scan on 2 M rows is ~2000 RPCs, which is the
`Storage Table Read Requests` number.

**Raft log as WAL, and why fsync still matters.** A tablet leader appends the write to its Raft
log (`fsync`ed, `durable_wal_write = true` by default; `false` is the risky speed knob) and
replicates it. Commit requires a majority of *durable* appends, so the system tolerates one
crashed node *and* a power loss on another only if WAL writes are durable. RocksDB's WAL is off
because the Raft log already covers the memtable's contents: on restart, the tablet replays its
Raft log from the last flushed index into the memtable, the same redo idea as module 04 one layer up.

**Intents, locks and conflict detection.** IntentsDB stores, per transaction, the provisional
values plus **lock intents** on rows and prefixes (strong/weak, read/write), encoded as keys.
A new write checks IntentsDB for conflicting intents and RegularDB for a committed value newer
than its read time. Older versions aborted the newcomer on conflict ("fail on conflict");
newer versions have **wait queues** so a transaction waits for the holder, like PostgreSQL's row
locks, with deadlock detection across tablets. The transaction status tablet records
pending/committed/aborted with the commit hybrid time; the **apply** step rewrites intents into
RegularDB with that timestamp and deletes them. A reader hitting an intent whose status is
unknown makes an RPC to the status tablet; that is why in-flight hot rows cost readers extra
RPCs.

**Hybrid logical clocks (HLC).** Each node keeps `(physical_us, logical)`; messages carry the
sender's HLC and receivers advance to max. A snapshot read at HLC `T` must be sure no write
with timestamp <= T can still arrive: writes with physical time within `max_clock_skew` of `T`
are in the uncertainty window, and if such a write is found the read is retried at a later time.
Tight clocks (chrony, or a cloud provider's clock service) shrink the window; the default 500 ms
is loose enough for ordinary NTP. CockroachDB and Spanner solve the same problem differently
(Spanner with TrueTime hardware).

**Leader leases and read consistency.** A leader may only serve a read if it holds a lease
whose duration is shorter than the election timeout, so a partitioned old leader cannot serve
stale data after a new leader was elected. Leases are renewed with heartbeats. This is how a
"read from leader" is linearisable without a Raft round of its own.

**Load balancing and tablet moves.** YB-Master monitors replica counts per node and moves
tablet replicas (remote bootstrap: copy RocksDB SSTables plus Raft log tail) and leaders to even
load, respecting placement policy. Immutable SSTables make this a file copy; this is the concrete
payoff of the LSM choice from module 10.

**Compaction as GC.** Full compactions drop versions older than the history retention window
and tombstones, and repack per-column entries into packed rows. Scheduled full compactions
(`scheduled_full_compaction_frequency_hours`) are the cluster's "VACUUM"; without them, update-
and delete-heavy tables read more entries per row over time.

## 5. In production

- **Latency budget.** Every statement is >= 1 network RPC; a 20-statement request is 20+ ms
  before any real work even in one zone. The login path in module 06 is the model: fewer
  statements, covering indexes, procedures. On a cluster this is not an optimisation, it is the
  design.
- **Run ANALYZE.** A table loaded by your migration engine and never analysed makes the planner
  believe it has 1000 rows and nest-loop it against everything. Add `ANALYZE` to the end of every
  load job and after large changes. Check `pg_class.reltuples`.
- **Hotspots are visible in the master UI** (per-tablet read/write ops). A range-sharded
  `created_at` key or a hash on `status` shows one tablet carrying the load. Fix the key design;
  there is no runtime trick.
- **Retry 40001 everywhere.** Snapshot isolation by default means concurrent updates of the same
  row fail one side. The C server needs the retry loop from module 05 around every transaction,
  with jittered backoff.
- **Connection pooling matters more**: each YSQL connection is a PostgreSQL backend plus its
  catalog cache; the first statement on a fresh connection pays `Catalog Read Requests` RPCs.
  Keep long-lived pooled connections; YugabyteDB ships a built-in connection manager in newer
  versions.
- **Rolling upgrades and node loss** are routine: RF = 3 across zones, and the application must
  tolerate a few seconds of errors for tablets whose leader was on the lost node. Spread leaders
  (the balancer does) so no single node's failure disturbs every table at once.
- **Backups**: distributed snapshots (`yb_backup`, or the platform's scheduled backups) capture
  all tablets at one hybrid time; `pg_dump` works but is a logical, slow, non-consistent-across-
  tablets-unless-in-one-transaction export. Point-in-time recovery uses the history retention
  window per database.
- **Honest limits you should mention**: no triggers on partitioned tables in older versions,
  some `ALTER TABLE` forms rewrite or are unsupported, `pg_stat_statements` exists but per-node,
  logical replication/CDC via its own connector (Debezium-based), extensions limited to those
  that do not touch storage, and version-to-version differences are larger than PostgreSQL's.
  Check the docs for the version you run.
- **How you phrase the resume bullet**: "The login path did sequential scans on hash-sharded
  tables, fanning out to every tablet across the cluster; I added a covering composite index
  sharded on the natural key so each login became a single-tablet index-only read, and collapsed
  the five statements into one PL/pgSQL call, cutting round trips from five to one."

## 6. Try it

1. **(easy)** Bring up the docker cluster, load `code/m11_yugabyte_demo.sql` steps 1 to 3, and
   compare the two `EXPLAIN (ANALYZE, DIST)` outputs. Done when you can read Storage Read Requests
   and explain the drop.
2. **(easy)** Run step 4 and show the Sort node and the fan-out. Recreate `app_users` with
   `PRIMARY KEY (id ASC)` and show the range query now touches one tablet. Done when you can state
   the trade-off (hotspot on inserts).
3. **(medium)** Change the docker compose to 3 yugabyted nodes (the file has the commented
   block), set RF = 3, load the table, then `docker stop` one node while running a loop of
   point reads. Done when you can report the error window length and confirm no committed data was
   lost after the node returns.
4. **(medium)** Create a table with 1000 rows and three secondary indexes. Time 10,000
   single-row inserts; drop two indexes; time again. Done when you can explain the difference with
   distributed transactions and intents.
5. **(hard)** Design the tablets and indexes for the full loan schema on a 3-node cluster:
   which tables colocated, PK sharding for each big table, which indexes hash which columns, and
   which are covering. Write the DDL and justify every line in a comment. Done when a reviewer
   could not find a login-style scatter in any of the module 06 queries.
6. **(hard)** Reproduce a 40001 under snapshot isolation with two sessions, then implement the
   retry loop in a small libpq program against port 5433 and show the invariant holds under 1000
   concurrent attempts from two processes. Done when the counts reconcile and retries are logged.

## 7. Interview questions

**Q: Describe YugabyteDB's architecture.**
A: A PostgreSQL-derived query layer (YSQL) on every node, speaking the PostgreSQL protocol, over
DocDB, a distributed document store in which every table and index is split into tablets, each
tablet is a Raft group replicated to three nodes, and each replica is a RocksDB instance. A small
YB-Master Raft group keeps the tablet map and balances load. Writes commit when a majority of a
tablet's replicas have logged them; reads go to the tablet leader.

**Q: What changes for the application compared with PostgreSQL?**
A: Round trips dominate because every statement is a network RPC; the primary key is hash
sharded by default so range scans and ORDER BY on it fan out; secondary indexes turn writes into
distributed transactions and non-covering lookups into two hops; the default isolation is
snapshot, so clients must retry 40001; sequences are cached per node; ANALYZE is manual; VACUUM
is unnecessary; small tables should be colocated to avoid tablet explosion.

**Q: How does YugabyteDB implement MVCC and transactions without xmin/xmax or VACUUM?**
A: Each value is stored in RocksDB under a key that ends with a hybrid timestamp, so versions
sit adjacent and a read at time T seeks to the newest version at or before T; compaction removes
versions older than the retention window. Multi-tablet transactions write provisional intents to
each tablet and commit atomically by recording a commit timestamp on a transaction status tablet;
readers that meet an intent consult the status tablet, and intents are applied asynchronously.

**Q: How did you design indexes differently on the cluster?**
A: I chose the sharding of each index explicitly, hashing on the natural key columns the lookup
always supplies so an exact match goes to one tablet, and included the columns the query reads so
the plan is an index-only scan with one storage read request instead of two across nodes. I kept
the number of secondary indexes minimal because each one makes every write a distributed
transaction, and avoided hashing on low-cardinality columns to prevent hotspots.

**Q: What does `Storage Read Requests` in a YugabyteDB EXPLAIN tell you?**
A: The number of RPCs the coordinator made to tablet servers for that node. A point lookup should
be 1; a sequential scan is one per page of prefetched rows per tablet; an index scan that is not
covering adds table read requests. It is the cluster's equivalent of buffer reads and the first
number to minimise.

**Q: What happens when a node dies?**
A: Tablets it led lose their leader; followers detect missed heartbeats and elect a new leader
within a few seconds; committed data is intact because every commit was on a majority. Tablets
it followed continue immediately with two of three replicas. The master later re-replicates
under-replicated tablets to healthy nodes if the node stays down.

**Q: Why hash sharding by default, and when is it wrong?**
A: Hashing spreads inserts and point reads evenly across tablets and nodes, which is what most
OLTP tables need and what avoids hotspots on increasing keys. It is wrong when the access pattern
is range scans or ordered reads on the key, such as time-series or code lookups, where range
sharding keeps the scan on one or two tablets; the cost of range sharding is hotspots on
sequential inserts, mitigated by pre-splitting or a hash prefix.

## 8. Recap

- YSQL = PostgreSQL query layer; DocDB = tablets on Raft on RocksDB; YB-Master = tablet map and balancer.
- Rows are documents keyed by PK with per-column sub-keys and hybrid timestamps; MVCC lives in the key; compaction is GC.
- Hash sharding by default (16-bit hash space); range sharding for ordered access, with hotspot risk.
- RF = 3 Raft groups per tablet; majority commit; leader reads with leases; follower reads optional and stale.
- Single-tablet writes are fast path; multi-tablet operations are distributed transactions with intents and a status tablet.
- Default isolation is snapshot (REPEATABLE READ); retry 40001; SERIALIZABLE is lock-based here.
- Every statement is at least one RPC: batch, cover, use procedures; run ANALYZE yourself.
- Secondary indexes are tables: choose their sharding explicitly, cover hot lookups, index less.
- Colocate small tables; split big ones; watch the master UI for hotspots and leader distribution.
- EXPLAIN (ANALYZE, DIST): Storage Read Requests, Rows Scanned, Catalog Read Requests are the numbers that matter.
