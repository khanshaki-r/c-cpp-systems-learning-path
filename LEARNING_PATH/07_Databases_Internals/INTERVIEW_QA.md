# Interview questions: 07_Databases_Internals

38 questions with model answers, ordered from basic to senior. Answer out loud, in your own
words, then compare. Each answer is what a strong candidate says in under a minute; the module
in brackets has the depth behind it. Where a question touches your own work (the Berkeley DB to
YugabyteDB migration, the PL/pgSQL layer, the login fix), use your real numbers.

---

## Part 1. Storage: pages, buffers, indexes

**1. What is a database page, and why does PostgreSQL use 8 KB pages?** (02)
A page is the fixed-size unit in which the engine reads, writes, caches and logs data; every table
and index file is an array of pages. An 8 KB heap page has a 24-byte header, an array of 4-byte
line pointers growing down from the top, tuples growing up from the bottom, and free space in
between. A fixed size makes caching, free-space tracking and WAL full-page images simple, and 8 KB
is a compromise between I/O efficiency and wasted space when only one row is needed.

**2. What is in a PostgreSQL tuple header and why does it matter?** (02, 05)
Each row version carries a 23-byte header: `t_xmin` (creating transaction), `t_xmax` (deleting or
locking transaction), `t_ctid` (pointer to the newer version), infomask hint bits and the null
bitmap. That is how MVCC, row locks and update chains work without a separate lock table or undo
log. The cost is per-row overhead and old versions in the heap until VACUUM.

**3. What is the buffer pool and how does PostgreSQL choose which page to evict?** (02)
`shared_buffers` is a shared array of 8 KB buffers holding copies of disk pages, with a hash table
from (relation, fork, block) to buffer. Eviction uses a clock sweep: each buffer has a usage count
(0 to 5) bumped on access and decremented as the clock hand passes; a buffer at 0 and unpinned is
reused, after writing it if dirty. Large sequential scans and VACUUM use small ring buffers so
they cannot flush the whole cache.

**4. How does a B+tree index find a row, and how many page reads does it take?** (03)
From the metapage to the root, binary-search the separator keys to pick a child, repeat to a
leaf, binary-search the leaf for the key, take the heap TID, read the heap page. Fan-out is in the
hundreds per 8 KB page, so even hundreds of millions of rows need a height of 3 or 4: about 4 or 5
page reads per point lookup, with the upper levels nearly always cached.

**5. How do you choose the column order of a composite index?** (03)
Equality predicates first, then at most one range predicate, then the ORDER BY columns in the same
direction, then INCLUDE columns for output. The tree can only narrow the scan on a leftmost prefix
of its columns; columns after the first range predicate only filter. Among equality columns,
order by how the index will be shared with other queries rather than by selectivity alone.

**6. What is a covering index, and what does INCLUDE do?** (03, 06)
A covering index contains every column a query reads, so the executor answers from the index
alone with an Index Only Scan, provided the visibility map marks the heap pages all-visible
(otherwise it still checks the heap, shown as Heap Fetches). INCLUDE stores payload columns only
in leaf entries: they do not affect uniqueness or ordering and keep internal pages narrow. My
login fix was exactly this: a unique index on (branch_code, user_code) including the hash, status
and attempt counter.

**7. Why would the planner ignore an index you just created?** (03, 06)
Usually because it is right to: if the predicate matches more than a few percent of rows, random
heap reads cost more than one sequential pass. Other causes: the predicate does not match the
index (a function or cast on the column, wrong leftmost prefix, a partial index not implied by the
WHERE), stale or misleading statistics, or a generic plan built for an average parameter value.
EXPLAIN with estimated versus actual rows tells you which.

**8. What is a HOT update?** (03)
A heap-only tuple update happens when no indexed column changes and the new version fits on the
same page: PostgreSQL chains old to new within the page and writes no index entries at all.
Index entries keep pointing at the old line pointer, which pruning later turns into a redirect.
Design consequences: do not index columns that change on every update, and leave free space with
fillfactor 80 to 90 on hot-update tables; check `n_tup_hot_upd / n_tup_upd`.

## Part 2. Durability and transactions

**9. What is the write-ahead log and why is it fast enough to wait for at every commit?** (04)
Before a data page change can reach disk, a WAL record describing it must be durable; commit waits
only for the WAL flush, not for the data pages. WAL is append-only and sequential, and one flush
covers every transaction that committed in the same moment (group commit), so a commit costs one
sequential fsync instead of random writes to many data files. Data pages are written lazily by the
background writer and checkpoints.

**10. What does a checkpoint do, and what are full-page writes?** (04)
A checkpoint writes all dirty buffers to disk and records a redo point, so crash recovery only has
to replay WAL from there. Because an 8 KB page write is not atomic on disk, the first change to
each page after a checkpoint logs the whole page image, so a torn page can be restored before
redo. Frequent checkpoints mean faster recovery but write storms and more full-page images; that
is the `max_wal_size` / `checkpoint_timeout` trade-off.

**11. Walk through crash recovery in PostgreSQL. Why is there no undo phase?** (04, 05)
On startup the server reads the last checkpoint record, then replays every WAL record from the
redo point, skipping records whose LSN is not newer than the page's LSN (that makes redo
idempotent). There is no undo because MVCC keeps old row versions in the heap: changes of a
transaction without a commit record are simply invisible, since its xid is not marked committed.
VACUUM later removes those versions.

**12. How does MVCC decide whether a row version is visible to you?** (05)
A snapshot records the next xid (xmax), the oldest running xid (xmin) and the list of transactions
in progress. A version is visible if its creating xid committed before the snapshot (below
xmax, not in the in-progress list, committed in `pg_xact`) and its deleting xid is zero, aborted,
or not yet visible to the snapshot. READ COMMITTED takes a new snapshot per statement; REPEATABLE
READ and SERIALIZABLE keep one for the transaction.

**13. Name the classic isolation anomalies and which PostgreSQL level prevents each.** (05)
Dirty read: never possible in PostgreSQL. Non-repeatable read and phantom: possible in READ
COMMITTED, prevented by REPEATABLE READ (PostgreSQL's snapshot is stronger than the standard
requires). Lost update: possible in READ COMMITTED with read-modify-write in the application,
detected in REPEATABLE READ (40001). Write skew: only SERIALIZABLE (SSI) prevents it, by aborting
one transaction. Any transaction at REPEATABLE READ or above must be retryable.

**14. Two clerks read a balance and write back a computed value. What goes wrong and how do you fix it?** (05)
A lost update: both read 500, one writes 200, the other 100, and one withdrawal disappears.
Fixes: let the database compute (`SET balance = balance - 300 WHERE balance >= 300`, which
re-checks on the newest version), lock the row with `SELECT ... FOR UPDATE`, or run at
REPEATABLE READ and retry on serialization failure. Optimistic version columns work too.

**15. What does VACUUM do, and why can it fail to clean up?** (05, 14)
VACUUM removes dead row versions no snapshot can see, removes their index entries, updates the
free space map and visibility map, and freezes old xids to prevent wraparound. It does not shrink
files. It cannot remove anything newer than the oldest snapshot in the cluster, so a long or idle
in-transaction session, a lagging replication slot, a standby with hot_standby_feedback, or a
forgotten prepared transaction stops cleanup everywhere and tables bloat.

**16. How does PostgreSQL detect deadlocks, and how should the application respond?** (05)
A backend that has waited `deadlock_timeout` for a lock runs a depth-first search over the
waits-for graph; if it finds a cycle it aborts itself with 40P01. The application rolls back and
retries the whole transaction. Prevention is ordering: lock rows in a global order (parent before
child, lower id first) in every code path.

## Part 3. Queries and the planner

**17. How do you read an EXPLAIN (ANALYZE, BUFFERS) plan?** (06)
Bottom-up and inside-out: leaves are scans, parents consume their rows. For each node compare
estimated `rows` with `actual rows` times `loops` (a big mismatch is the root of most bad plans),
look at time per node (inclusive of children), Buffers hit versus read, and lines like
`Rows Removed by Filter`, `Heap Fetches`, `Batches` or temp I/O that reveal wasted work. The fix
usually sits at the lowest node where the estimate went wrong.

**18. Explain the three join methods and when the planner picks each.** (06)
Nested loop: for each outer row probe the inner, ideal when the outer is small and the inner has
an index. Hash join: build a hash table on the smaller input and probe it with the other, good for
large unsorted equi-joins as long as the hash fits in work_mem (otherwise it spills into batches).
Merge join: walk two inputs sorted on the join key, good when both come sorted from indexes or are
large. A nested loop chosen because of an underestimate is the classic disaster.

**19. Why can a query be fast in psql and slow from the application?** (06, 08)
The application uses prepared statements, and after five executions PostgreSQL may switch to a
generic plan built for an average parameter, which is wrong for skewed values. Other causes: a
parameter bound with a different type, so the column gets cast and the index cannot be used;
different session settings; or the application holding a transaction open. Compare with
`EXPLAIN EXECUTE`, and use `plan_cache_mode = force_custom_plan` where skew matters.

**20. Tell me about the login latency problem you fixed.** (03, 06, 08)
Logins looked up users by branch code plus user code, but the table only had a surrogate primary
key, so every login was a sequential scan, and on the hash-sharded cluster that scan touched every
node. I confirmed it with EXPLAIN (Seq Scan, rows removed by filter, buffers equal to the table)
and pg_stat_statements. The fix was a unique composite index on the natural key with INCLUDE
columns, turning it into a single index-only point read, plus collapsing the five-statement login
path into one PL/pgSQL call. Give your real before and after numbers.

**21. When does logic belong in a stored procedure rather than the application?** (08)
When a rule must hold for every writer, when the work is set-based over many rows, when several
statements form one atomic unit that would otherwise cost several round trips, or when the
application role must not see raw data. Keep CPU-heavy work, external calls and fast-changing
team-owned logic in the application. In my system, validation, authorisation and workflow
transitions with audit went into PL/pgSQL; password hashing stayed in C.

**22. What does an EXCEPTION block cost in PL/pgSQL?** (08)
Each entry creates a subtransaction (a savepoint), and if it writes it gets its own xid. In a loop
that is one subtransaction per iteration, and past 64 live subtransactions per backend, snapshot
checks spill to the on-disk `pg_subtrans`, which can slow the whole server. Wrap the loop instead
of each iteration, and handle expected cases with `ON CONFLICT` or checks instead of exceptions.

## Part 4. Key-value stores and distributed SQL

**23. How does Berkeley DB differ from PostgreSQL?** (10)
Berkeley DB is a library linked into your process: no server, no SQL, no schema, keys and values
are opaque bytes, and the application owns the record format, deadlock detection, checkpointing,
log archiving and recovery invocation. Its B-tree uses page-level two-phase locking by default, so
readers and writers block each other and deadlocks are routine unless you enable MVCC snapshots.
PostgreSQL is a server with a catalog, a planner, MVCC that never blocks readers, and all those
operational duties built in.

**24. Show me the Berkeley DB C calls for a transactional insert and a range scan.** (10)
`db_env_create`, `env->open` with `DB_INIT_MPOOL | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN |
DB_RECOVER`, `db_create`, `dbp->open` with `DB_BTREE`. Insert: `env->txn_begin`, fill two DBTs,
`dbp->put(dbp, txn, &key, &val, DB_NOOVERWRITE)`, `txn->commit`. Scan: `dbp->cursor`, then
`cur->get(cur, &key, &val, DB_SET_RANGE)` to position at the first key >= start and `DB_NEXT` until
the prefix changes, closing the cursor before commit. Every unit of work sits in a retry loop for
`DB_LOCK_DEADLOCK`.

**25. LSM tree versus B-tree: what are the trade-offs?** (10)
A B-tree updates pages in place: one path for reads, predictable latency, but random writes and a
page rewrite per change. An LSM tree appends to a WAL and an in-memory memtable, flushes sorted
immutable files and merges them in the background, so writes are sequential and cheap but reads
may check several levels (bloom filters help) and compaction adds write amplification and latency
spikes. B-trees suit read-heavy OLTP; LSMs suit write-heavy workloads on SSDs and are the storage
layer of most distributed databases.

**26. How does YugabyteDB store a PostgreSQL table?** (11)
The YSQL layer is PostgreSQL's query layer; storage is DocDB. Each table is split into tablets by
hash of the primary key (or by range); each tablet is a Raft group of replicas, each replica a
RocksDB-based LSM store. A row becomes DocDB key-value pairs keyed by the encoded primary key plus
column ids and a hybrid timestamp. Secondary indexes are separate distributed tables with their
own tablets, so a write touching several indexes is a distributed transaction.

**27. What does Raft give a YugabyteDB tablet?** (11)
One leader per tablet accepts writes and replicates log entries to followers; a write is committed
once a majority has persisted it, so with replication factor 3 the tablet survives one node loss
with no data loss. If the leader fails, followers elect a new one within seconds, automatically.
Leaders also hold leases so they can serve consistent reads without a round of consensus.

**28. Why did your login query hurt more on YugabyteDB than it would on single-node PostgreSQL?** (11)
With hash-sharded tables, a predicate that is not on the shard key cannot be routed to one tablet,
so the sequential scan is sent to every tablet on every node and each scans its share: network
round trips plus work on all nodes, getting worse as the cluster grows. A hash-sharded unique
index on the natural key with INCLUDE columns makes the lookup one RPC to one tablet leader,
visible in `EXPLAIN (ANALYZE, DIST)` as one storage read request.

**29. Hash or range sharding for a primary key?** (11)
Hash spreads writes evenly and makes point lookups one tablet, but range scans and ORDER BY on the
key touch every tablet and need a sort. Range keeps neighbouring keys together, so ranges are
cheap, but monotonically increasing keys (timestamps, sequences) send all inserts to the last
tablet, a hotspot. Choose by access pattern: hash for point access, range for scans, and
composite keys like `(customer_id HASH, created_at ASC)` to get both.

## Part 5. Migration

**30. Walk me through your Berkeley DB to YugabyteDB migration.** (12)
Inventory every database, key and record layout, version and code path that writes, producing a
mapping document per record type that doubles as the parity specification. A decoder reading packed
records by explicit offsets with per-field byte order and version dispatch, quarantining what it
cannot decode. A streaming engine: cursor from the last checkpoint, batches decoded into a per-batch
arena, idempotent upserts committed with a checkpoint row in the same transaction, retried on
40001. Then per-record parity hashes, dual write with a replay queue, shadow reads, a flag flip of
reads, a soak, and retirement of the old store, with rollback as a flag until an announced point
of no return.

**31. How did you make the migration engine restartable and idempotent?** (12)
Each batch's rows and a checkpoint row (run id, last source key, record count, batch hash) commit
in the same target transaction, so after a crash either both exist or neither does. On restart the
engine reads the checkpoint, repositions the cursor with `DB_SET_RANGE` on the last key and skips
it. Every write is `INSERT ... ON CONFLICT` on the natural key, so redoing a batch is harmless,
which also made `synchronous_commit = off` safe for the load session.

**32. How do you prove the migrated data is correct?** (12)
Row counts are not enough. Define a canonical serialisation per entity (fixed field order,
trimmed, UTF-8, money in integer cents, dates as YYYYMMDD, a NULL sentinel), hash it from the
decoded source in the engine and recompute it from the target rows with a SQL function, and
compare per record. Add counts per partition including rejects, the business aggregates the
finance team already reconciles, sampled field-by-field deep diffs, and a continuous diff during
dual write. Every mismatch is a finding with a status; cutover needs zero open findings.

**33. Why never cast a byte buffer to a struct when decoding legacy records?** (12)
It is undefined behaviour when the buffer is not aligned for the struct, which Berkeley DB does not
guarantee, and it silently assumes the current compiler's layout, padding and byte order match the
original writer's. Read each field with `memcpy` or byte shifts at explicit offsets with an explicit
endianness: compilers turn that into the same single load on x86, without the UB. Hash the decoded
fields, not raw bytes, because padding bytes may contain garbage.

**34. How do you cut over with zero downtime, and how do you roll back?** (12)
Dual write behind a runtime flag (old store first while it is the source of truth, new store
second, failures to a replay queue), bulk-load history from a snapshot, reconcile changes made
during the load, run full parity, then shadow reads, then flip reads to the new store with a flag,
then swap the dual-write order so the old store stays current. Rollback is flipping the flag back,
rehearsed in staging, with triggers for it decided in advance. The point of no return is when the
new store holds data the old one cannot represent; announce it.

## Part 6. Caching, Redis and operations

**35. Explain cache-aside and how you invalidate it correctly.** (13)
Read the cache; on a miss read the database and fill the cache with a TTL. On write, update the
database, commit, then delete the key; deleting rather than setting avoids two writers leaving the
older value in the cache. The remaining race (a reader fills the cache with a value it read before
the write) is bounded by TTL, versioned keys or leases. Protect hot keys from stampedes with
single-flight loading, TTL jitter and serving stale while refreshing.

**36. Why is Redis fast, and what are its limits as a source of truth?** (13)
Commands run on one thread over in-memory data structures, so each is a few memory accesses with no
locking, and an epoll event loop multiplexes thousands of connections; I/O threads only parallelise
socket reads and writes. Persistence is an RDB snapshot or an append-only file fsynced every second
by default, and replication is asynchronous, so acknowledged writes can be lost on a crash or
failover. That is fine for caches, sessions and rate limits, and wrong for balances; a Redis lock
also needs a fencing token checked by the database to be safe.

**37. What does pgbouncer's transaction mode break, and why use it anyway?** (14)
A server connection is returned to the pool at every commit, so session state does not survive:
session SETs, session advisory locks, LISTEN, temp tables across transactions, WITH HOLD cursors,
and protocol prepared statements unless pgbouncer 1.21+ is configured for them. It is worth it
because PostgreSQL runs a process per connection, and multiplexing thousands of mostly idle
application connections onto a few dozen busy server connections is what the engine handles best.

**38. Compare default isolation and "SERIALIZABLE" across Oracle, SQL Server and PostgreSQL.** (05, 14)
All three default to READ COMMITTED, but Oracle reads a consistent version from undo, SQL Server
takes shared locks so readers block writers unless read committed snapshot is enabled, and
PostgreSQL takes a fresh MVCC snapshot per statement. Oracle's SERIALIZABLE is really snapshot
isolation and allows write skew; SQL Server's is lock-based with key-range locks; PostgreSQL's is
Serializable Snapshot Isolation, which aborts one transaction with 40001 instead of blocking. When
moving code between them, also watch Oracle's empty string as NULL and SQL Server's parameter
sniffing.
