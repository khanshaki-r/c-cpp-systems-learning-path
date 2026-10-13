# 1. What a database actually does

**Time:** ~2 hours   **Prerequisites:** none

## 1. In plain words

A database management system (DBMS) is a program that takes a question written in a language
(SQL, or a `get(key)` call) and turns it into reads and writes of bytes on disk, while promising
three things the file system never promised you: your data is still correct after a crash,
many users can work at the same time without corrupting each other, and you can ask for data by
its meaning rather than by its byte offset.

Think of a bank branch. You walk up to a counter (the client protocol), say what you want in
plain language (the parser), the clerk decides how to do it fastest (the planner), does it (the
executor), going to the right filing cabinet (access methods), using the folders already on his
desk if he has them (buffer manager), and before closing a drawer he writes a line in a bound
ledger that cannot be torn out (the write-ahead log). If the building loses power, the ledger
is replayed and the cabinets are repaired. Every serious database, from SQLite to Oracle, has
these pieces. They differ in how the pieces are split and tuned.

The second idea of this module: a database can be a **library** linked into your program
(Berkeley DB, SQLite, RocksDB) or a **server** process you talk to over a socket (PostgreSQL,
Oracle, Redis). Same layers, very different operational consequences.

## 2. The real thing

### 2.1 The layers

```
 client  ---- TCP / Unix socket, frontend/backend protocol v3 ----> backend process
                                                                        |
      +-----------------------------------------------------------------+
      |  1. Protocol handler     parses messages: Query, Parse, Bind, Execute, Sync
      |  2. Parser (gram.y)      SQL text -> raw parse tree  (syntax only)
      |  3. Analyzer             raw tree -> Query tree: resolves names, types, catalogs
      |  4. Rewriter             applies rules and views (view -> its SELECT)
      |  5. Planner/optimizer    Query -> Plan tree: scans, joins, sorts, costs
      |  6. Executor             walks the Plan tree, pulls tuples node to node
      |  7. Access methods       heap AM (tables), index AMs: btree, hash, gin, gist, brin
      |  8. Buffer manager       shared_buffers: 8 KB pages in RAM, pin/unpin, eviction
      |  9. Storage manager      files on disk: base/<dboid>/<relfilenode>, 1 GB segments
      | 10. WAL                  every page change logged first; fsync on commit
      | 11. Lock manager         heavyweight locks (tables), row locks (in tuple headers),
      |                          lightweight locks (in-memory structures), spinlocks
      | 12. Transaction manager  xid assignment, commit log (pg_xact), snapshots
      +-----------------------------------------------------------------
```

One-line definitions:

- **Protocol**: the byte format of messages between client and server. PostgreSQL uses its own
  "frontend/backend protocol version 3"; libpq, JDBC and psycopg all speak it.
- **Parse tree**: the SQL statement as a tree of nodes (SelectStmt, ColumnRef, ...) with no
  knowledge of whether tables exist.
- **Query tree**: the parse tree after name resolution; every column is now (table oid, attnum, type).
- **Plan tree**: a tree of executable nodes (SeqScan, IndexScan, HashJoin, Sort, Aggregate...).
- **Access method (AM)**: the code that knows the layout of one kind of on-disk structure.
- **Buffer manager**: the cache of disk pages in shared memory; every page read or written goes through it.
- **WAL (write-ahead log)**: an append-only record of changes, written and flushed before the
  changed data page is allowed to reach disk.

### 2.2 Relational vs key-value

A **key-value store** gives you `put(key, value)`, `get(key)`, `delete(key)` and usually an
ordered cursor (`seek(key)`, `next()`). The value is opaque bytes. All interpretation is in your
program. Berkeley DB, LevelDB, RocksDB, Redis (with richer value types) are key-value stores.

A **relational database** stores typed rows in tables, enforces constraints, and lets you ask
declarative questions (`WHERE`, `JOIN`, `GROUP BY`) that the engine decides how to execute.
PostgreSQL, Oracle, SQL Server, YugabyteDB (YSQL) are relational.

The relational engine is a key-value store plus four things: a schema (types and constraints),
a query language, a planner, and secondary indexes maintained for you. YugabyteDB makes this
literal: its SQL layer is PostgreSQL's parser, planner and executor, and its storage layer is a
key-value store (DocDB on RocksDB). You spent two years moving data from the first kind to the
second kind; the rest of this course explains both ends.

| | Key-value (Berkeley DB) | Relational (PostgreSQL) |
|---|---|---|
| Unit of storage | key bytes -> value bytes | typed row in a table |
| Who knows the record layout | your C structs | the catalog (`pg_attribute`) |
| Secondary access paths | you build them (secondary DBs) | `CREATE INDEX` |
| Query | `cursor->get(DB_SET_RANGE)` loops | declarative SQL, planner decides |
| Constraints | your code | `PRIMARY KEY`, `FOREIGN KEY`, `CHECK`, `UNIQUE` |
| Concurrency | page or record locks, you choose | MVCC plus row locks, automatic |
| Deployment | library in your process | server process, network protocol |

### 2.3 Library vs server

Berkeley DB is a C library. `db_create()`, `db->open()` happen inside your process. The buffer
pool (the "mpool") is a shared-memory region mapped into every process that opens the same
environment. There is no SQL, no network, no separate server process to start. Crash recovery is
your responsibility: you call `DB_ENV->open()` with `DB_RECOVER`. Consequences:

- Lowest possible latency: a `get` is a function call and a few memory reads. No syscall unless
  the page is not in the mpool.
- No separate failure domain: if your process corrupts memory, it can corrupt the database's
  in-memory pages. If your process dies holding locks, another process must run recovery.
- One machine only. Replication exists (Berkeley DB HA) but is rarely used.
- Schema is in your head and your C headers. Nobody can run an ad-hoc report.

PostgreSQL is a server. The `postmaster` process listens on a socket, forks one **backend
process per connection**, and all backends share one `shared_buffers` region. Consequences:

- Every statement crosses a socket: a round trip costs tens of microseconds on localhost,
  hundreds across a network, milliseconds across a region. This dominates the design of
  application code (batching, fewer round trips, stored procedures).
- The server survives client crashes. Locks held by a dead connection are released when the
  backend notices the socket closed.
- Many languages and tools can read the same data. Schema is in the catalog.
- Process-per-connection means connections are expensive (several MB each, fork cost); hence
  connection poolers (module 14).

### 2.4 A request's path from SQL text to disk pages

Take this statement, sent from a C program using libpq:

```sql
SELECT customer_id, status FROM loan_applications WHERE application_no = 'LA-2024-000123';
```

1. **Client**: libpq builds a `Query` message (simple protocol) or `Parse`/`Bind`/`Execute`
   (extended protocol, used by `PQexecParams` and prepared statements), writes it to the socket.
2. **Backend reads the message** in `PostgresMain()`, calls `exec_simple_query()`.
3. **Parser**: `raw_parser()` runs the flex lexer and bison grammar, returns a `SelectStmt`.
   Syntax errors stop here ("syntax error at or near").
4. **Analyzer**: `parse_analyze()` looks up `loan_applications` in `pg_class` (via the relcache,
   a per-backend cache of catalog rows), resolves `application_no` to attribute number 2 of type
   `text`, checks the literal can be coerced to `text`. "column does not exist" stops here.
5. **Rewriter**: no views or rules here, so the Query passes through unchanged.
6. **Planner**: `pg_plan_query()` looks at `pg_statistic` for `application_no`. If there is a
   unique index, it estimates 1 row and chooses an **Index Scan**. If not, it estimates the
   selectivity from `n_distinct` and may still pick a **Seq Scan** of the whole heap. Output: a
   `PlannedStmt` with a plan tree.
7. **Executor**: `ExecutorStart()` builds executor state, `ExecutorRun()` calls the top node's
   `ExecProcNode()`, which pulls tuples from its child (the Volcano / iterator model). The Index
   Scan node calls the btree AM: `btgettuple()`.
8. **Access method**: btree descends from the root page to a leaf page, comparing the key. Each
   page it needs is requested from the buffer manager with `ReadBuffer()`. The leaf holds an
   index tuple containing the heap **TID** (block number, item offset) of the row.
9. **Buffer manager**: `ReadBuffer()` hashes (relfilenode, fork, block) into the shared buffer
   lookup table. If the page is in `shared_buffers`, it is pinned and returned. If not, a victim
   buffer is chosen (clock-sweep), written out if dirty, and the page is read with `pread()` from
   the file `base/<db>/<relfilenode>`. The OS page cache may serve it without touching the disk.
10. **Heap AM**: `heap_fetch()` reads the tuple at that TID, checks **visibility** using the
    tuple's `xmin`/`xmax` against the transaction's snapshot (module 05). Visible: the tuple is
    projected to (customer_id, status) and sent back as a `DataRow` message. Not visible: the
    index scan asks for the next matching entry.
11. **Client**: libpq collects `DataRow`s into a `PGresult`; `CommandComplete` and `ReadyForQuery`
    end the exchange.

For an `UPDATE` the path is the same until step 10, then: a new tuple version is written to a
heap page (the old one gets `xmax` set), a WAL record describing the change is appended to the
WAL buffers, indexes are updated if an indexed column changed, and at `COMMIT` the WAL is
flushed with `fdatasync()`. The dirty heap page stays in `shared_buffers` and is written later by
the checkpointer or background writer. Module 04 covers why this order is safe.

### 2.5 Where the time goes

Typical costs on a 2020s server, for scale (orders of magnitude, from standard latency tables):

| Step | Typical cost |
|---|---|
| Socket round trip, localhost | 20 to 50 us |
| Socket round trip, same data centre | 100 to 500 us |
| Parse + analyze + plan a simple statement | 50 to 200 us |
| Buffer manager hit (page in shared_buffers) | under 1 us |
| Page read from OS page cache | a few us |
| Page read from NVMe SSD | 50 to 100 us |
| Page read from spinning disk | 5 to 10 ms |
| fsync of WAL to SSD | 0.5 to 2 ms (device dependent) |

Two lessons that will repeat through the course: avoid round trips (batch, use set-based SQL),
and avoid disk reads (indexes that fit in memory, good `shared_buffers`). A seq scan of a 10 GB
table is slow not because of CPU but because of 1.3 million page reads.

## 3. Code

### 3.1 Talking to the server at the protocol level

`code/m01_libpq_hello.c` connects with libpq, runs one parameterised query with the extended
protocol, and prints the plan the server chose. Build: `gcc m01_libpq_hello.c -o hello -lpq`.

```c
/* Extended protocol: Parse/Bind/Execute. The server plans once per call with the
 * real parameter value, so it can use the statistics for that value. */
const char *params[1] = { "LA-2024-000123" };
PGresult *res = PQexecParams(conn,
    "SELECT customer_id, status FROM loan_applications WHERE application_no = $1",
    1, NULL, params, NULL, NULL, 0 /* text results */);
if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    fprintf(stderr, "query failed: %s", PQerrorMessage(conn));   /* server-side error text */
}
```

### 3.2 Watch the layers from psql

```sql
-- the parser alone: a syntax error never touches the catalog
SELEC 1;
-- ERROR:  syntax error at or near "SELEC"

-- the analyzer: valid syntax, unknown relation
SELECT * FROM no_such_table;
-- ERROR:  relation "no_such_table" does not exist

-- the planner's choice and the executor's actual work
EXPLAIN (ANALYZE, BUFFERS)
SELECT customer_id, status FROM loan_applications WHERE application_no = 'LA-2024-000123';

-- the storage manager: where the file is
SELECT pg_relation_filepath('loan_applications');          -- base/16384/16432
SELECT pg_relation_size('loan_applications') / 8192 AS pages;

-- the buffer manager: how many of its pages are in shared_buffers right now
SELECT count(*) FROM pg_buffercache b
JOIN pg_class c ON b.relfilenode = pg_relation_filenode(c.oid)
WHERE c.relname = 'loan_applications';

-- the lock manager: what this session holds mid-transaction
BEGIN;
SELECT * FROM loan_applications WHERE id = 1 FOR UPDATE;
SELECT locktype, mode, granted FROM pg_locks WHERE pid = pg_backend_pid();
ROLLBACK;
```

### 3.3 Trace one statement through the C source (reading exercise)

If you clone `postgres` from git (branch `REL_16_STABLE`), the path above is literally:

```
src/backend/tcop/postgres.c        PostgresMain -> exec_simple_query
src/backend/parser/gram.y          grammar
src/backend/parser/analyze.c       transformStmt
src/backend/optimizer/plan/planner.c   standard_planner
src/backend/executor/execMain.c    ExecutorRun
src/backend/executor/nodeIndexscan.c   IndexNext
src/backend/access/nbtree/nbtsearch.c  _bt_search, _bt_first
src/backend/storage/buffer/bufmgr.c    ReadBuffer_common
src/backend/access/heap/heapam.c       heap_fetch, heap_update
src/backend/access/transam/xlog.c      XLogInsert, XLogFlush
```

You do not need to read it all. Open `bufmgr.c` and find `ReadBuffer_common`; read the comment
block. That one function is the boundary between "memory" and "disk" for the whole system.

## 4. Under the hood (senior-level)

**Process model.** PostgreSQL forks a backend per connection. Backends share memory through a
System V or mmap shared segment created by the postmaster: `shared_buffers`, the lock tables,
the WAL buffers, the commit log cache, the proc array (one entry per backend holding its current
xid and snapshot information). There are also background processes: checkpointer, background
writer, WAL writer, autovacuum launcher and workers, stats collector (replaced by shared memory
stats in 15+), archiver, logical replication workers. `ps aux | grep postgres` shows all of them.

**Why not threads?** Historical, plus isolation: a crash in one backend cannot scribble over
another backend's private memory; the postmaster notices the death and restarts everything to be
safe (because shared memory may be inconsistent). The cost is heavier connections and no shared
per-connection caches (each backend has its own relcache, plan cache, catalog cache). A
threaded model has been discussed for years; as of PostgreSQL 16 it is still processes.

**Memory contexts.** All backend allocations go through `palloc()` into hierarchical memory
contexts (`MemoryContext`). A query runs in a context that is reset when the query ends, so
there is no per-allocation `free`. This is the same arena idea as the per-batch arena you used
in your migration engine: free everything at once at a known boundary. `work_mem` is the
budget per sort or hash node before spilling to disk; it is a soft target, not a hard cap on the
backend.

**Catalog as tables.** The schema itself lives in tables: `pg_class` (relations), `pg_attribute`
(columns), `pg_index`, `pg_proc` (functions), `pg_type`, `pg_statistic`. `CREATE TABLE` is
mostly inserts into these. Each backend caches catalog rows (syscache, relcache) and the caches
are invalidated through shared invalidation messages when DDL commits. That is why a `DROP
TABLE` in one session takes an `AccessExclusiveLock`: it must wait for everyone's cached
knowledge to be safely discarded.

**The iterator (Volcano) model.** Every plan node implements `ExecProcNode()` returning one
tuple or NULL. A `Hash Join` node calls its inner child until exhausted (building the hash
table), then calls its outer child one tuple at a time and probes. The entire plan is pull
driven from the top. PostgreSQL 10+ adds JIT compilation of expressions with LLVM for large
queries, but the per-node structure remains. This matters when you read EXPLAIN (module 06):
"loops" and "actual rows" are counts of these per-node calls.

**Where Berkeley DB differs internally.** Berkeley DB has the same bottom layers (mpool = buffer
manager, log = WAL, lock = lock manager, txn = transaction manager) and the access methods
(BTREE, HASH, QUEUE, RECNO), but no parser, planner or executor. Your C code is the executor.
Opening a cursor and calling `DB_NEXT` in a loop is you writing an Index Scan by hand. That is
exactly what "B-tree cursor traversal" on your resume means, and why module 10 revisits it.

## 5. In production

- **"The database is slow" is almost never one layer.** The first question is which layer:
  network (round trips, connection setup), planning (bad plan, no index), execution (large
  sort, many loops), storage (page reads from disk), locks (waiting on another transaction).
  `pg_stat_activity.wait_event_type` tells you if a backend is waiting on `Lock`, `IO`, `LWLock`
  or `Client`. Start there.
- **Connection count.** Each PostgreSQL connection is a process with its own caches. 500 idle
  connections cost several GB and make every catalog invalidation more expensive. Your RPC
  application server should hold a small pool (tens, not hundreds) and multiplex. Module 14.
- **Round trips kill.** A login path that does 12 single-row SELECTs across a network does 12
  round trips. On a multi-node YugabyteDB cluster each may also hop to another node. Folding
  them into one PL/pgSQL function call or one set-based statement is often a 10x win before any
  index is touched. Module 07 and 08.
- **A library database is in your blast radius.** With Berkeley DB a segfault in your server
  could leave the environment needing recovery; a hung process could hold page locks forever
  (`db_deadlock`, `DB_ENV->lock_detect`). Moving to a server database trades latency for a
  clean failure boundary. Say this when asked "why did you migrate?".
- **3 AM**: "all connections are stuck". `SELECT pid, state, wait_event_type, wait_event,
  query FROM pg_stat_activity` will usually show them all waiting on `Lock` behind one session
  holding an `AccessExclusiveLock` from a DDL that is itself blocked behind a long transaction.
  `pg_blocking_pids(pid)` gives you the chain. Module 05 and 14.

## 6. Try it

1. **(easy)** Install PostgreSQL 16 on WSL per the README, create the `loans` database, load
   `code/m09_schema.sql` and `code/m09_seed.sql`. Done when `\dt` lists the tables and
   `SELECT count(*) FROM loan_applications` returns a number.
2. **(easy)** Run the five psql snippets from section 3.2. Done when you can say which layer
   produced each error or result.
3. **(medium)** Run `ps -o pid,ppid,cmd -C postgres` (or `ps aux | grep postgres`). Identify
   the postmaster, checkpointer, background writer, WAL writer, autovacuum launcher and your own
   backend. Open a second `psql` and watch a new backend appear. Done when you can name each.
4. **(medium)** Build and run `code/m01_libpq_hello.c`. Change it to run the same query 1000
   times in a loop and time it with `clock_gettime`. Then wrap the loop in one transaction
   (`BEGIN`/`COMMIT`). Done when you can explain the time difference (hint: nothing, it is
   read-only; now change it to an UPDATE and compare: per-statement commit vs one commit).
5. **(hard)** Draw, on paper, the path of an `UPDATE loan_applications SET status = 'APPROVED'
   WHERE id = 42` through all 12 layers, including which pages are touched (heap page, every
   index on the table whose key changed, WAL), and when each byte reaches durable storage. Done
   when your drawing matches what modules 02 to 05 say (come back and check).

## 7. Interview questions

**Q: Walk me through what happens when I send a SELECT to PostgreSQL.**
A: The client sends a protocol message over the socket. The backend parses it into a parse
tree, the analyzer resolves names and types against the catalog, the rewriter expands views,
the planner uses statistics to produce a plan tree, and the executor pulls tuples through that
tree. Scan nodes call access methods (heap or index), which fetch pages through the buffer
manager; a miss goes to the storage manager and `pread()`. Each tuple is checked for visibility
against the transaction's snapshot before being returned.

**Q: What is the difference between a key-value store and a relational database?**
A: A key-value store maps opaque key bytes to opaque value bytes and lets you look up or scan by
key; your program interprets everything. A relational database adds a typed schema with
constraints, a declarative query language, a planner that chooses access paths, and secondary
indexes maintained automatically. Internally a relational database is often a key-value store
plus those layers; YugabyteDB makes this explicit with PostgreSQL's query layer over RocksDB.

**Q: Why is Berkeley DB a library and PostgreSQL a server? What does that change?**
A: Berkeley DB links into your process; a `get` is a function call with no network, so latency
is minimal, but your process shares a failure domain with the data, only one machine can host
it, and there is no SQL for ad-hoc access. PostgreSQL runs as separate processes behind a
socket; every statement pays a round trip, but the server survives client crashes, enforces
schema centrally, and serves many clients and tools.

**Q: Why does PostgreSQL use a process per connection and what does it cost?**
A: Historical design plus fault isolation: a backend crash cannot corrupt another backend's
private memory. The cost is that each connection is a process with several MB of private caches
and fork startup, so thousands of connections are expensive and a connection pooler is
standard.

**Q: Where would you look first if "the database is slow"?**
A: `pg_stat_activity` for what sessions are doing and what they wait on (Lock, IO, LWLock,
Client), then `pg_stat_statements` for the statements consuming the most total time, then
`EXPLAIN (ANALYZE, BUFFERS)` on the top offender. That tells me whether the problem is network
round trips, a bad plan, disk reads, or lock contention, which are fixed in different ways.

**Q: What is the buffer manager and why does every layer go through it?**
A: It is the shared cache of 8 KB disk pages in `shared_buffers`. Every access method asks it
for a page by (relation, fork, block number); it pins the page if present or evicts a victim and
reads it from disk if not. Centralising this gives one consistent view of each page, allows
dirty pages to be written lazily by the checkpointer, and lets WAL rules be enforced in one place.

## 8. Recap

- A DBMS is layers: protocol, parser, analyzer, rewriter, planner, executor, access methods,
  buffer manager, storage, WAL, locks, transactions.
- The planner decides *how*; the executor pulls tuples one node at a time (Volcano model).
- All page access goes through the buffer manager; a miss is a `pread()` on a 1 GB segment file.
- Writes go to WAL first, fsync on commit; data pages are written later by the checkpointer.
- Key-value store: opaque bytes, you are the executor. Relational: schema, SQL, planner, indexes.
- Library database (Berkeley DB): function-call latency, shared failure domain, one machine.
- Server database (PostgreSQL): round trips, clean isolation, many clients, process per connection.
- Round trips and disk page reads are the two costs that dominate application performance.
- Catalog is tables; backends cache it; DDL invalidates caches and needs strong locks.
- Memory contexts are arenas: allocate freely, free all at once at a boundary.
