# 14. Performance tuning and operations

**Time:** ~3 hours   **Prerequisites:** 02, 04, 05, 06

## 1. In plain words

Modules 01 to 13 explained what the database does with one statement. This module is about
keeping a real server healthy while thousands of statements arrive per second, for years: how
many connections it can take, how much memory each part gets, how you find the query that is
hurting everybody, how dead rows get cleaned up, how you get the data back after a disk dies, and
how a second server takes over when the first one goes away.

The analogy is running a bank branch rather than serving one customer. You decide how many
counters are open (connections and pooling), how big the filing cabinets on the desks are
(memory settings), you watch the queue and ask who is taking so long (monitoring), you send the
cleaners in at the right time (VACUUM), you photocopy the ledgers every night and keep the
receipts in between (backups and WAL archiving), and you keep a second branch across the road
that copies every entry and can open its doors if this one floods (replication and failover).

For an interview this is where "I have used PostgreSQL" becomes "I have run PostgreSQL". The
questions are concrete: what does pgbouncer's transaction mode break, how do you size
`work_mem`, why did VACUUM not remove anything, what is your RPO, and what does Oracle call
the thing PostgreSQL calls a schema.

## 2. The real thing

### 2.1 Connections and pooling

PostgreSQL forks **one backend process per connection** (module 01). Each backend costs memory
(typically a few MB of private memory, more once it has touched many catalog entries and cached
plans), a slot in the shared `ProcArray` that every snapshot scans, and lock-table entries.
Hundreds of mostly idle connections are therefore not free: snapshot creation gets slower as
`max_connections` grows, and a burst where all of them become active at once oversubscribes the
CPUs and makes everything slower, not faster.

The fix is a **connection pooler**: many client connections share a small number of server
connections. **pgbouncer** is the standard one: a single-threaded, event-driven proxy (the same
design as Redis, module 13) that speaks the PostgreSQL protocol on both sides.

| Pool mode | A server connection is assigned to a client... | What breaks | Use |
|---|---|---|---|
| `session` | for the whole client session | nothing; but no multiplexing gain for long sessions | legacy apps, tools like psql |
| `transaction` | for one transaction, returned at COMMIT/ROLLBACK | any **session state**: `SET` (non-LOCAL), session advisory locks, `LISTEN`, temp tables kept across transactions, `WITH HOLD` cursors, protocol-level prepared statements unless pgbouncer 1.21+ with `max_prepared_statements` set | the default for OLTP application servers |
| `statement` | for one statement; multi-statement transactions refused | everything transactional | rare; autocommit-only workloads |

Transaction mode is what gives the large multiplexing gain: 2000 application threads that each
spend most of their time outside a transaction can share 40 server connections. It also explains
why `idle in transaction` is so harmful behind a pooler: that client holds a server connection
(and a snapshot that blocks VACUUM) while doing nothing.

Sizing. The often-quoted starting point from the PostgreSQL wiki is roughly
`connections = (2 x CPU cores) + effective spindles` of **active** connections; it is a rule of
thumb to start measuring from, not a law. The real answer is the number at which throughput
stops rising when you load-test with `pgbench`. For a 4-core server that is typically in the
tens, not the hundreds. Set `max_connections` a little above the pool size plus admin
headroom (`superuser_reserved_connections`, replication, monitoring).

Your RPC application server already has a connection pool inside the C process. pgbouncer still
helps when there are many such processes (each with its own pool), when they run on several
machines, or when a deployment restarts them all at once and they reconnect in a storm.

### 2.2 Memory and WAL settings

The settings that matter, with starting points for a dedicated 16 GB RAM, 4-core, SSD OLTP
server. These are conventional starting values used in the community (pgtune-style), **not**
measured optima: change one at a time and measure with the queries in section 3.

| Setting | What it controls | Starting point | Notes |
|---|---|---|---|
| `shared_buffers` | PostgreSQL's own page cache (module 02) | 4GB (~25% of RAM) | the OS page cache holds much of the rest; more is not always better (double buffering, checkpoint volume) |
| `effective_cache_size` | the planner's *assumption* of total cache (shared + OS) | 12GB (~75%) | allocates nothing; makes index scans look cheaper |
| `work_mem` | per sort/hash **node** per backend before spilling to temp files | 16MB to 64MB | multiply by connections x nodes per query; raise per role or per session for reports |
| `maintenance_work_mem` | VACUUM, CREATE INDEX, ALTER TABLE ADD FK | 1GB | big index builds and vacuums go faster |
| `autovacuum_work_mem` | dead-tuple memory per autovacuum worker | -1 (uses maintenance_work_mem) | in 16, at most 1GB is used for the dead-TID array |
| `wal_buffers` | WAL in shared memory before write | -1 (auto, 1/32 of shared_buffers, max 16MB) | rarely touched |
| `max_wal_size` | WAL between checkpoints before one is forced | 4GB to 16GB | too small: `checkpoints_req` dominates, many FPIs (module 04) |
| `checkpoint_timeout` | time between checkpoints | 15min | longer: fewer FPIs, longer crash recovery |
| `checkpoint_completion_target` | spread checkpoint writes over this fraction | 0.9 (default since 14) | smooths I/O |
| `synchronous_commit` | wait for WAL flush (and standbys) at commit | on | `off` per session for bulk loads: lose at most ~3 x wal_writer_delay of commits on crash, never corruption |
| `wal_compression` | compress full-page images | lz4 (or zstd) | cuts WAL volume; costs some CPU |
| `random_page_cost` | planner's cost of a random page read | 1.1 on SSD (default 4) | the default models spinning disks |
| `effective_io_concurrency` | prefetch depth for bitmap heap scans | 200 on SSD | |
| `huge_pages` | 2 MB pages for shared memory | try | fewer TLB misses with big shared_buffers (needs OS config) |
| `statement_timeout`, `lock_timeout`, `idle_in_transaction_session_timeout` | guard rails | per role: e.g. 30s, 5s, 60s | set on the application role, not globally (it would kill maintenance jobs) |

Settings live in `postgresql.conf`, can be overridden by `ALTER SYSTEM` (written to
`postgresql.auto.conf`), per database or role (`ALTER ROLE loans_app SET work_mem = '32MB'`),
per session (`SET`), or per transaction (`SET LOCAL`). `SELECT name, setting, source FROM
pg_settings` shows which one won. Some need a restart (`shared_buffers`, `max_connections`,
`shared_preload_libraries`, `wal_level`); most need only `SELECT pg_reload_conf()`;
`pg_settings.context` says which.

### 2.3 Finding the problem: the observability toolkit

**`pg_stat_statements`** (module 06) aggregates every statement by its normalised text
(constants replaced by `$1`): calls, total and mean time, rows, buffers hit and read, temp blocks,
WAL bytes. It answers "what is the server spending its time on" and is the first query in every
incident. It needs `shared_preload_libraries = 'pg_stat_statements'` and a restart.

**`pg_stat_activity`** is one row per backend *now*: state (`active`, `idle`, `idle in
transaction`), `wait_event_type`/`wait_event` (what it is blocked on: `Lock`, `LWLock`, `IO`,
`Client`), `xact_start`, `query_start`, `backend_xmin` (the snapshot that holds the VACUUM
horizon), and the current query text. Sample it every few seconds and you have a poor man's
active-session history (Oracle's ASH, section 2.8).

**`pg_locks`** is the lock table: one row per held or awaited lock, with `granted`. Join it to
`pg_stat_activity`, or use `pg_blocking_pids(pid)` to get the blockers of a waiting backend
directly. Row locks appear as waits on a `transactionid`, because a row lock lives in the
tuple's `xmax` (module 05).

**The slow query log** writes to the server log (on Ubuntu,
`/var/log/postgresql/postgresql-16-main.log`):

```
log_min_duration_statement = 250ms     # every statement slower than this, with its duration
log_lock_waits = on                    # any lock wait longer than deadlock_timeout (1s)
log_temp_files = 0                     # every temp file (sorts and hashes spilling)
log_checkpoints = on                   # default on since 15: duration, buffers written, why
log_autovacuum_min_duration = 1s       # autovacuum runs that took longer than this
auto_explain.log_min_duration = 500ms  # the PLAN of slow statements (module 06)
log_line_prefix = '%m [%p] %q%u@%d %a ' # time, pid, user, database, application_name
```

`pgBadger` turns that log into an HTML report. Set `application_name` in every client's
connection string (your C server: `application_name=rpcsrv-node3`) so you can tell callers apart.

**Cumulative statistics views** for trends: `pg_stat_user_tables` (seq vs index scans, dead
tuples, last vacuum), `pg_stat_user_indexes` (unused indexes), `pg_stat_database` (commits,
rollbacks, deadlocks, temp bytes), `pg_stat_bgwriter` (checkpoints), `pg_stat_wal`,
`pg_stat_replication`, `pg_stat_progress_vacuum` / `_create_index` / `_basebackup`.
Export them to Prometheus with `postgres_exporter` and graph rates, not raw counters.

### 2.4 VACUUM tuning

Module 05 explained why VACUUM exists. Operations is about making it keep up.

**When autovacuum starts on a table:**
`n_dead_tup > autovacuum_vacuum_threshold (50) + autovacuum_vacuum_scale_factor (0.2) x reltuples`.
For `app_users` with 2 M rows that is 400,050 dead tuples; for a 100 M row table, 20 M. Hot
tables need per-table settings:

```sql
ALTER TABLE loans.app_users SET (autovacuum_vacuum_scale_factor = 0.01,     -- 1%: ~20k dead rows
                                 autovacuum_vacuum_threshold = 1000,
                                 autovacuum_analyze_scale_factor = 0.02);
-- Insert-only tables (audit, archive) never get dead tuples; since 13, inserts also trigger
-- vacuum (autovacuum_vacuum_insert_scale_factor) so their visibility map and freezing keep up.
ALTER TABLE loans.application_audit SET (autovacuum_vacuum_insert_scale_factor = 0.05);
```

**How fast it runs:** autovacuum is throttled by a cost budget. Each page hit, miss and dirtied
costs points (`vacuum_cost_page_hit` 1, `_miss` 2 in 16, `_dirty` 20); after
`autovacuum_vacuum_cost_limit` points (default 200, shared by all workers) it sleeps
`autovacuum_vacuum_cost_delay` (2 ms). On modern SSDs the default budget is often too low for a
busy system: raising the limit to 1000 to 2000 is the usual first step. `autovacuum_max_workers`
(3) is how many tables are vacuumed at once; more workers share the same cost limit, so raise the
limit together with the workers.

**What stops VACUUM from removing rows** (the "horizon"): the oldest snapshot anywhere.
Culprits, in the order you check them: a long or `idle in transaction` session
(`pg_stat_activity.backend_xmin`), a replication slot that is behind (`pg_replication_slots`),
a hot standby with `hot_standby_feedback = on` running a long query, an orphaned prepared
transaction (`pg_prepared_xacts`). `VACUUM (VERBOSE)` prints "N dead row versions cannot be
removed yet, oldest xmin: X".

**Freezing and wraparound.** Transaction ids are 32-bit; tuples older than
`vacuum_freeze_min_age` get frozen so their xid can be reused. Any table whose `relfrozenxid` age
passes `autovacuum_freeze_max_age` (200 M) gets an aggressive anti-wraparound vacuum that cannot
be cancelled by lock conflicts. If ages still approach about 2 billion, the server refuses new
xids to protect data. Monitor `age(datfrozenxid)` (section 3) and alert far below that.

**Bloat that VACUUM cannot fix.** VACUUM makes space reusable inside the file; it does not shrink
it (except trailing empty pages). After a mass delete, `VACUUM FULL` rewrites the table under an
`ACCESS EXCLUSIVE` lock (an outage); `pg_repack` does the same online with a trigger-based copy.
Indexes: `REINDEX INDEX CONCURRENTLY` (module 03).

### 2.5 Backups

Two kinds, with different purposes:

| | Logical: `pg_dump` / `pg_restore` | Physical: `pg_basebackup` + WAL archive |
|---|---|---|
| What | SQL-level dump of one database: DDL + data | a copy of the whole cluster's data directory files |
| Consistency | one REPEATABLE READ snapshot (module 05) | fuzzy copy made consistent by replaying WAL from the backup's start LSN (module 04) |
| Point-in-time recovery | no: only the moment of the dump | yes, to any moment covered by archived WAL |
| Portability | across major versions and architectures; selective (`-t`, `-n`) | same major version and platform only |
| Speed at scale | slow to restore (rebuilds every index) | restore = copy files + replay |
| Use | upgrades, copying a schema, small databases, extra safety net | the real backup of a production cluster |

```bash
# Logical: custom (-Fc) or directory (-Fd) format allows parallel and selective restore
pg_dump -d loans -Fd -j 4 -f /backup/loans_$(date +%F)
pg_restore -d loans_restore -j 4 /backup/loans_2026-10-12
pg_dumpall --globals-only > /backup/globals.sql     # roles and tablespaces are not in pg_dump

# Physical base backup, streaming the WAL needed to make it consistent
pg_basebackup -h localhost -U replicator -D /backup/base_$(date +%F) -Ft -z -X stream -P
```

**PITR** needs continuous **WAL archiving**: `archive_mode = on` and an `archive_command` (or
`archive_library`) that copies every completed 16 MB WAL segment somewhere safe. Restore = base
backup + `restore_command` + `recovery_target_time` (module 04 exercise 5). In production use a
tool that does archiving, retention, parallelism, compression, encryption and verification:
**pgBackRest** or **Barman** (`WAL-G` in cloud setups). Two numbers define the design: **RPO**
(how much data you may lose: the archive lag, or zero with synchronous replication) and **RTO**
(how long recovery may take: base backup size / restore speed + WAL replay time). A backup that
has never been restored is a hope, not a backup: schedule restore tests.

### 2.6 Replication

**Streaming (physical) replication.** A standby connects to the primary with the replication
protocol, receives the WAL byte stream as it is written (`walsender` on the primary,
`walreceiver` on the standby), and replays it continuously with the same redo code as crash
recovery. The standby is a byte-identical copy of the whole cluster and serves read-only queries
(**hot standby**). Key settings and facts:

- `wal_level = replica` (default), `max_wal_senders`, a role with `REPLICATION`, a
  `pg_hba.conf` line for `replication`.
- **Asynchronous** by default: the primary commits without waiting; a crash of the primary can
  lose the last transactions not yet received (RPO = replication lag, usually milliseconds to
  seconds). **Synchronous**: `synchronous_standby_names = 'FIRST 1 (s1, s2)'` or `'ANY 1 (...)'`
  and `synchronous_commit = on` (flush on standby) / `remote_apply` (visible on standby) /
  `remote_write`. Commit latency now includes a network round trip; if every synchronous standby
  is down, commits **wait**.
- **Replication slots** make the primary keep WAL until the standby has it, so a disconnected
  standby can catch up instead of needing a rebuild; the price is that a dead standby makes WAL
  pile up until the disk is full. `max_slot_wal_keep_size` caps it.
- **Conflicts on the standby**: replaying a VACUUM that removes rows a standby query still needs
  cancels that query (`canceling statement due to conflict with recovery`) after
  `max_standby_streaming_delay`. `hot_standby_feedback = on` stops the cancellations by holding
  back VACUUM on the *primary* (bloat). Pick which pain you prefer, per standby.
- Lag: `pg_stat_replication` on the primary (`write_lag`, `flush_lag`, `replay_lag`), and
  `pg_last_xact_replay_timestamp()` on the standby.

**Logical replication.** The primary decodes WAL into row changes (INSERT/UPDATE/DELETE per
table) with the `pgoutput` plugin and sends them to subscribers, which apply them as ordinary SQL
writes. `CREATE PUBLICATION` on the source, `CREATE SUBSCRIPTION` on the target.

| | Physical streaming | Logical |
|---|---|---|
| Unit | WAL bytes, whole cluster | row changes, chosen tables |
| Target | read-only identical copy | writable database with its own indexes and extra tables |
| Versions | same major version | across major versions (upgrades with near-zero downtime) |
| Not replicated | nothing | DDL, sequence values (sync them by hand at cutover), large objects; tables need a primary key or REPLICA IDENTITY for UPDATE/DELETE |
| Use | HA, read scaling, backups from standby | upgrades, CDC, consolidating or splitting databases, feeding analytics |

Logical decoding is also how change data capture tools (Debezium) stream PostgreSQL changes to
Kafka. That is the "CDC" row of module 12's cutover table, when the *source* is PostgreSQL.

### 2.7 Failover and high availability

Replication gives you a copy; **failover** is the decision and mechanics of making the copy the
primary. PostgreSQL itself only provides the mechanics (`pg_promote()`, `pg_ctl promote`,
timelines, `pg_rewind`); the decision needs an external manager.

- **Detection and decision**: **Patroni** is the common choice. Each node runs an agent that
  holds a leader key with a TTL in a distributed configuration store (etcd, Consul, ZooKeeper).
  The primary's agent renews the key; if it stops (node dead, network partition), a standby's
  agent acquires the key, promotes its PostgreSQL, and the others re-point to it. The consensus
  store is what prevents two primaries: only the key holder may be primary, and a primary that
  cannot renew the key demotes itself.
- **Split brain** is the failure to prevent: two nodes accepting writes. Defences are the
  leader lock above, **fencing** (the old primary is stopped or isolated before promotion, the
  "STONITH" idea), and synchronous replication so no acknowledged commit exists only on the old
  primary.
- **Timelines**: every promotion starts a new timeline id; WAL from the old timeline after the
  fork point is not valid on the new one. The old primary rejoins as a standby after
  **`pg_rewind`** rewinds its divergent pages (needs `wal_log_hints = on` or data checksums).
- **Routing clients to the primary**: a virtual IP, HAProxy health-checking Patroni's REST API,
  or libpq multi-host connection strings:
  `host=pg1,pg2,pg3 target_session_attrs=read-write` (libpq tries each host until it finds a
  writable one). Your C server's reconnect logic must treat a failover as "connection lost, retry
  the transaction" (the same retry loop as 40001).
- **What async failover costs**: with asynchronous replication, the promoted standby may be
  missing the last few acknowledged commits. Either accept a small RPO (and say it out loud), or
  pay commit latency for synchronous replication to at least one standby.

Contrast with YugabyteDB (module 11): every tablet is a Raft group; a leader's failure triggers
an election among the tablet's replicas within seconds, automatically, and a write is
acknowledged only after a majority has it, so RPO is zero for a single-node loss with no
external manager. PostgreSQL HA is assembled from parts; distributed SQL has it built in, and
pays for it with consensus latency on every write.

### 2.8 Oracle, SQL Server and PostgreSQL: an honest comparison

Your resume lists Oracle and SQL Server. In an interview, the safe and senior position is
precise knowledge of the differences that change application behaviour, and honesty about the
depth of your hands-on use. The table is what you should be able to say without notes.

**Terminology and architecture**

| Concept | Oracle | SQL Server | PostgreSQL |
|---|---|---|---|
| Server process model | one instance = shared memory (SGA) + background processes; dedicated or shared server processes | one process, many threads (SQLOS scheduler) | postmaster + one process per connection |
| "Database" | the whole set of files; one database per instance traditionally, PDBs inside a CDB since 12c | many databases per instance | many databases per cluster; no cross-database queries without extensions |
| "Schema" | = a user's namespace (user and schema are the same object) | namespace inside a database (`dbo`) | namespace inside a database, independent of users |
| Old row versions for readers | **undo** segments; current row in the block, old versions rebuilt from undo (ORA-01555 "snapshot too old") | locks by default; with RCSI/SNAPSHOT, row versions in the **tempdb version store** (or in the user database with Accelerated Database Recovery, 2019+) | all versions in the **heap** itself; VACUUM removes dead ones (module 05) |
| Write-ahead log | **redo log** (online + archived) | **transaction log** (.ldf) | **WAL** (pg_wal) |
| Table organisation | heap by default; index-organised tables (IOT) optional | usually a **clustered index** (the table is the B-tree on its key) | heap always; `CLUSTER` reorders once and is not maintained |
| Auto ids | sequences; identity columns since 12c | `IDENTITY`; sequences since 2012 | `GENERATED ... AS IDENTITY` over sequences |
| Empty string | `''` **is NULL** | `''` is not NULL | `''` is not NULL |
| Optimiser hints | yes, `/*+ INDEX(t idx) */` widely used | yes, table hints and `OPTION (...)`; Query Store plan forcing | none in core (extension `pg_hint_plan`) |
| Plans for parameters | bind peeking, adaptive cursor sharing | plan cache with **parameter sniffing** (a classic SQL Server incident) | custom vs generic plans after 5 executions (module 06) |
| Plan and wait analysis | `EXPLAIN PLAN`, `DBMS_XPLAN`, AWR / ASH reports | actual execution plan, DMVs (`sys.dm_exec_query_stats`), Query Store | `EXPLAIN (ANALYZE, BUFFERS)`, `pg_stat_statements`, `pg_stat_activity` |
| HA | Data Guard (standby), RAC (shared-disk active-active) | Always On availability groups, failover cluster instances | streaming replication + Patroni |
| Backup | RMAN | full / differential / log backups | pg_basebackup + WAL archive (pgBackRest) |

**Isolation and concurrency**

| | Oracle | SQL Server | PostgreSQL |
|---|---|---|---|
| Default level | READ COMMITTED, with statement-level read consistency from undo | READ COMMITTED **with shared locks** (on-premises default; RCSI is a database option, on by default in Azure SQL Database) | READ COMMITTED with a new MVCC snapshot per statement |
| Do readers block writers? | no | **yes** under the locking default; no with RCSI | no |
| Levels offered | READ COMMITTED, SERIALIZABLE (and READ ONLY) | READ UNCOMMITTED, READ COMMITTED, REPEATABLE READ, SNAPSHOT, SERIALIZABLE | READ UNCOMMITTED (behaves as RC), READ COMMITTED, REPEATABLE READ, SERIALIZABLE |
| What "SERIALIZABLE" really is | **snapshot isolation**: write skew is possible; conflicts raise ORA-08177 | true serializable through **key-range locks** (blocking, deadlock-prone) | **SSI**: snapshot isolation plus dependency tracking; aborts with 40001 instead of blocking (module 05) |
| Snapshot isolation by name | (that is its SERIALIZABLE) | `SNAPSHOT` (needs ALLOW_SNAPSHOT_ISOLATION) | `REPEATABLE READ` |
| Deadlock victim error | ORA-00060 | 1205 | 40P01 |
| Lock escalation | none (row locks live in the block) | row/page locks escalate to table locks past thresholds | none (row locks live in the tuple's xmax) |

YugabyteDB YSQL (module 11) supports SERIALIZABLE, REPEATABLE READ (snapshot) and READ COMMITTED,
and its default behaves as snapshot isolation unless read-committed support is enabled by a flag;
check the version you run before you promise anything.

**Procedural languages: PL/SQL, T-SQL, PL/pgSQL**

| | PL/SQL (Oracle) | T-SQL (SQL Server) | PL/pgSQL (PostgreSQL) |
|---|---|---|---|
| Structure | blocks; **packages** (spec + body, package state per session) | batches and procedures; no packages | functions and procedures; schemas as namespaces, no packages |
| Error handling | `EXCEPTION WHEN ... THEN`; `RAISE_APPLICATION_ERROR(-20001, ...)` | `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH`, `THROW`, `@@ERROR`; `SET XACT_ABORT ON` to abort the transaction on any error | `EXCEPTION WHEN ...` blocks are subtransactions (cost, module 08); `RAISE ... USING ERRCODE` |
| Transaction control | COMMIT in procedures; **autonomous transactions** (`PRAGMA AUTONOMOUS_TRANSACTION`) for logging that survives rollback | COMMIT/ROLLBACK in procedures, nested `BEGIN TRAN` only counts (`@@TRANCOUNT`) | COMMIT/ROLLBACK only in procedures called at top level; no autonomous transactions (dblink or a second connection) |
| Bulk work | `BULK COLLECT`, `FORALL` to cut context switches between PL/SQL and SQL engines | set-based statements, table variables, `#temp` tables | set-based SQL; `RETURN QUERY`; no bulk-bind syntax needed |
| Temp tables | global temporary tables (definition permanent, data per session) | `#local` and `##global` temp tables in tempdb | session temp tables created at runtime (catalog churn if overused) |
| Output to caller | OUT params, ref cursors, pipelined table functions | result sets returned directly from procedures, OUT params | `RETURNS TABLE` / `SETOF` functions (materialised), OUT params, refcursors |
| Upsert | `MERGE` | `MERGE` (with known race caveats) or UPDATE-then-INSERT with locking hints | `INSERT ... ON CONFLICT`; `MERGE` since 15 |

The honest one-liner for an interview: "My deep, daily work is PostgreSQL and PL/pgSQL, and the
YugabyteDB migration. I have worked with Oracle and SQL Server, and the differences I watch for
when moving code between them are the default isolation and what SERIALIZABLE really means,
undo versus heap versioning versus the tempdb version store, empty string as NULL in Oracle,
parameter sniffing in SQL Server, and how error handling and transactions behave in PL/SQL,
T-SQL and PL/pgSQL." Then answer at the depth you actually have.

## 3. Code

`code/m14_monitoring_queries.sql` contains the fourteen queries of an incident runbook:
settings, top statements, sessions, long transactions, blocking chains, locks, cache hit ratio,
dead tuples, autovacuum thresholds and progress, wraparound age, unused indexes, checkpoints,
WAL, replication and slots, and sizes. Run it as a whole once, then keep it open during
incidents. The three you will use most:

```sql
-- What costs the server most (pg_stat_statements)
SELECT left(regexp_replace(query, '\s+', ' ', 'g'), 70) AS query, calls,
       round(total_exec_time::numeric, 1) AS total_ms, round(mean_exec_time::numeric, 3) AS mean_ms,
       shared_blks_hit + shared_blks_read AS blocks, rows / greatest(calls, 1) AS rows_per_call,
       temp_blks_written
FROM pg_stat_statements
ORDER BY total_exec_time DESC LIMIT 15;

-- Who blocks whom: follow blocking_pid until you reach a session that is not blocked itself
SELECT blocked.pid AS blocked_pid, now() - blocked.query_start AS waiting_for,
       blocking.pid AS blocking_pid, blocking.state AS blocking_state,
       now() - blocking.xact_start AS blocking_xact_age, left(blocking.query, 50) AS blocking_query
FROM pg_stat_activity blocked
JOIN LATERAL unnest(pg_blocking_pids(blocked.pid)) AS b(pid) ON true
JOIN pg_stat_activity blocking ON blocking.pid = b.pid;

-- What holds back VACUUM: the oldest snapshots
SELECT pid, state, now() - xact_start AS xact_age, backend_xmin, left(query, 60)
FROM pg_stat_activity WHERE backend_xmin IS NOT NULL ORDER BY age(backend_xmin) DESC LIMIT 5;
```

**pgbouncer in front of the WSL PostgreSQL** (`sudo apt install pgbouncer`):

```ini
; /etc/pgbouncer/pgbouncer.ini
[databases]
loans = host=127.0.0.1 port=5432 dbname=loans

[pgbouncer]
listen_addr = 127.0.0.1
listen_port = 6432
auth_type = scram-sha-256
auth_file = /etc/pgbouncer/userlist.txt
pool_mode = transaction
default_pool_size = 20          ; server connections per (database, user) pair
max_client_conn = 2000          ; client connections pgbouncer accepts
max_prepared_statements = 100   ; 1.21+: protocol-level prepared statements work in transaction mode
server_reset_query =            ; empty in transaction mode (DISCARD ALL is for session mode)
```

```bash
# load test: 200 clients through pgbouncer versus directly (pgbench ships with postgresql-contrib)
pgbench -i -s 20 loans_bench                         # in a separate database
pgbench -h 127.0.0.1 -p 6432 -c 200 -j 4 -T 60 -S loans_bench   # through the pooler
pgbench -h 127.0.0.1 -p 5432 -c 200 -j 4 -T 60 -S loans_bench   # direct (raise max_connections first)
psql -h 127.0.0.1 -p 6432 -U $USER pgbouncer -c "SHOW POOLS;"    # cl_active, cl_waiting, sv_active, sv_idle
```

**A streaming replica on the same WSL machine** (port 5435; 5433 and 5434 are used by docker):

```bash
sudo -u postgres psql -c "CREATE ROLE replicator WITH REPLICATION LOGIN PASSWORD 'repl'"
echo "host replication replicator 127.0.0.1/32 scram-sha-256" | sudo tee -a /etc/postgresql/16/main/pg_hba.conf
sudo service postgresql reload
sudo pg_createcluster 16 replica --port 5435          # creates config and an empty data directory
sudo rm -rf /var/lib/postgresql/16/replica/*
sudo -u postgres env PGPASSWORD=repl pg_basebackup -h 127.0.0.1 -p 5432 -U replicator \
     -D /var/lib/postgresql/16/replica -R -X stream -C -S replica1 -P
#  -R writes standby.signal and primary_conninfo; -C -S creates and uses replication slot replica1
sudo pg_ctlcluster 16 replica start
psql -p 5432 -c "SELECT application_name, state, sync_state, replay_lag FROM pg_stat_replication"
psql -p 5435 loans -c "SELECT pg_is_in_recovery(), count(*) FROM loans.branches"   # true, 500
# failover drill
sudo pg_ctlcluster 16 main stop
psql -p 5435 -c "SELECT pg_promote()"
psql -p 5435 -c "SELECT timeline_id FROM pg_control_checkpoint()"                  # 2: a new timeline
```

**Logical replication of the master tables into a reporting database** (needs
`wal_level = logical` and a restart):

```sql
-- in database loans (publisher)
CREATE PUBLICATION pub_master FOR TABLE loans.branches, loans.loan_products, loans.workflow_states;
-- A subscription to a database in the SAME cluster must use a pre-created slot,
-- otherwise CREATE SUBSCRIPTION waits forever for its own transaction:
SELECT pg_create_logical_replication_slot('sub_master', 'pgoutput');
```

```bash
createdb loans_reporting
psql -d loans_reporting -c "CREATE SCHEMA loans"
pg_dump -d loans -s -t 'loans.branches' -t 'loans.loan_products' -t 'loans.workflow_states' \
    | psql -d loans_reporting                                   # DDL is not replicated: copy it first
# Two errors are expected: the cache-version triggers on branches and loan_products refer to a
# function that is not copied. The reporting side does not need them (apply workers do not fire
# ordinary triggers anyway).
psql -d loans_reporting -c "CREATE SUBSCRIPTION sub_master CONNECTION 'dbname=loans'
    PUBLICATION pub_master WITH (create_slot = false, slot_name = 'sub_master')"
psql -d loans -c "UPDATE loans.branches SET name = name || ' *' WHERE code = 'BR0007'"
psql -d loans_reporting -c "SELECT name FROM loans.branches WHERE code = 'BR0007'"   # changed
```

## 4. Under the hood (senior-level)

**Why many connections slow down every query.** Taking a snapshot (`GetSnapshotData`) means
reading every backend's advertised xid from the shared `ProcArray`. PostgreSQL 14 made this much
cheaper by keeping the xids in a dense array, but the cost still scales with the number of
connected backends, and each backend also holds catalog and plan caches in private memory.
Context switching between more runnable processes than cores adds latency without adding
throughput. A pooler turns "2000 connections, 40 busy" into "40 connections, 40 busy", which is
the shape the engine is good at.

**pgbouncer's mechanics.** One thread, `libevent`, a state machine per client and per server
connection. In transaction mode it watches the `ReadyForQuery` message's transaction status byte
(`I` idle, `T` in transaction, `E` failed): when it returns to `I`, the server connection goes
back to the pool. That is exactly why session state leaks between clients in this mode: the
server backend persists, so a `SET search_path` by client A is seen by client B's next
transaction on the same backend. Use `SET LOCAL`, or set things per role.

**shared_buffers versus the OS page cache.** A page read by PostgreSQL usually exists twice:
in `shared_buffers` and in the kernel's page cache (PostgreSQL uses buffered I/O, not
`O_DIRECT`, in 16). The clock-sweep replacement in shared buffers protects frequently used
pages; the kernel's LRU catches the rest. A huge `shared_buffers` reduces the useful OS cache,
increases the dirty data each checkpoint must write, and makes the buffer mapping table bigger.
That is why the conventional starting point is a quarter of RAM rather than most of it.

**What a checkpoint costs, precisely.** A checkpoint writes every dirty buffer, then fsyncs every
touched file, then writes a checkpoint record. After it, the *first* modification of each page
writes a full-page image to WAL (module 04). So frequent checkpoints cost twice: the write
storm, and a WAL volume spike from FPIs. `checkpoint_completion_target = 0.9` spreads the writes;
a larger `max_wal_size` reduces the frequency; `wal_compression` shrinks the FPIs.

**How streaming replication is wired.** The standby's `walreceiver` connects with
`replication=true`, sends `START_REPLICATION SLOT replica1 PHYSICAL 0/3000000`, and the primary
starts a `walsender` that reads WAL from `pg_wal` (or memory) and streams `XLogData` messages.
The standby writes them to its own `pg_wal`, flushes, and reports positions back
(`write`/`flush`/`apply` LSNs), which is where `pg_stat_replication`'s lag columns come from and
what synchronous commit waits on. The `startup` process replays the WAL with the same redo
functions used after a crash. Logical replication runs the same `walsender` but passes WAL
through **logical decoding**, which reassembles each transaction's changes (spilling large ones
to disk) and emits them at commit in commit order.

**Backups without stopping writes.** `pg_basebackup` calls `pg_backup_start`, which forces a
checkpoint and records its redo LSN, then copies files while they change. The copy is
inconsistent (torn pages are possible), and that is fine: recovery replays WAL from the recorded
redo LSN, and full-page writes overwrite any torn page with an intact image (module 04). The
backup is usable only together with the WAL from its start to its end, which is why `-X stream`
or an archive is mandatory.

**Autovacuum's scheduling.** The launcher wakes every `autovacuum_naptime` (1 min) per database
and starts workers up to `autovacuum_max_workers`; each worker picks tables over their thresholds
from the statistics system. The cost budget is shared ("balanced") across active workers, so
three workers each get a third of `autovacuum_vacuum_cost_limit` unless a table has its own.
Anti-wraparound vacuums ignore the lock-conflict cancellation that ordinary autovacuum yields to,
which is how an `ALTER TABLE` can suddenly queue behind a vacuum you cannot cancel.

## 5. In production

- **The connection storm.** All application nodes restart after a deploy and each opens its
  full pool at once: `max_connections` reached, `FATAL: sorry, too many clients already`, and
  the health checks fail too. Fixes: pgbouncer in front, staggered restarts, pools that grow
  lazily, and `superuser_reserved_connections` so you can still log in.
- **`work_mem` out of memory.** Someone set `work_mem = 1GB` globally to fix one report; under
  load the kernel's OOM killer kills a backend, the postmaster restarts all backends, everyone is
  disconnected. Set big values per role or per session only.
- **"The database is slow" at 3 AM, in order:** `pg_stat_activity` (what is active, what waits
  on what), blocking chains, `pg_stat_statements` deltas since the last snapshot, the slow query
  log and auto_explain plans, then OS metrics (`iostat -x`, `vmstat`, disk full?). Write the
  timeline as you go; the post-mortem needs it.
- **Idle in transaction behind a pooler.** One code path forgot to COMMIT before an RPC to
  another service. VACUUM stopped removing rows cluster-wide, tables bloated, queries slowed over
  days. `idle_in_transaction_session_timeout` on the application role turns this into an error in
  that one code path.
- **The replication slot that filled the disk.** A decommissioned standby or a stopped CDC
  connector left its slot; WAL accumulated until the primary's disk filled and the primary
  stopped. Alert on `pg_replication_slots` retained WAL; set `max_slot_wal_keep_size`.
- **Backups that do not restore.** The archive command returned success while writing to a full
  disk; nobody tested a restore for months. Use pgBackRest's `check` and `verify`, and restore a
  backup into a scratch server on a schedule.
- **Failover that lost data.** Asynchronous replica promoted after the primary died with 2
  seconds of lag: those acknowledged payments do not exist on the new primary. Either accept a
  documented RPO, or run synchronous replication to a standby in another rack and accept the
  latency. For a banking ledger, the second.
- **Major version upgrades.** `pg_upgrade --link` takes minutes of downtime for any size (it
  reuses data files); logical replication to a new-version cluster then a switchover takes
  seconds of downtime but more preparation (DDL freeze, sequences, slots). Run `ANALYZE` after
  either, because planner statistics are not carried over by pg_upgrade in 16.
- **Mapping to your system**: the RPC server's DB pool size per node x number of nodes must stay
  under the pooler's `max_client_conn`; login procedures from module 08 run in transaction mode
  without session state; the slow query log is where the login regression (module 06) would have
  shown up first; on YugabyteDB, failover is Raft's job but your client's retry loop is still
  yours.

## 6. Try it

1. **(easy)** Enable `pg_stat_statements` (README setup), run `code/m06_slow_vs_fast_queries.sql`,
   then section 1 of `code/m14_monitoring_queries.sql`. Done when you can name the top three
   statements by total time and explain from `blocks` versus `rows_per_call` which one lacks an
   index.
2. **(easy)** Start two `psql` sessions; in one run `BEGIN; UPDATE loans.accounts SET balance =
   balance WHERE id = 1;` and leave it. In the other run sections 3, 4 and 5 of the monitoring
   file. Done when you can point at the `idle in transaction` session, its `backend_xmin`, and the
   lock rows, and end it with `pg_terminate_backend`.
3. **(medium)** Install pgbouncer in transaction mode as in section 3 and run the two pgbench
   commands with `-c 200`. Done when you have both TPS numbers, the `SHOW POOLS` output, and can
   explain why `SET search_path` from one client leaks to another in this mode (show it).
4. **(medium)** Take a `pg_dump -Fd -j 4` of `loans`, restore it into `loans_restore` with
   `pg_restore -j 4`, and time both. Then create the streaming replica on port 5435, run
   `m09_seed.sql` on the primary while watching `replay_lag`, and promote the replica. Done when
   you have the restore time, a lag observation, and the new timeline id.
5. **(hard)** Make autovacuum fall behind: run a `pgbench` custom script that updates
   `failed_attempts` on random `app_users` rows at full speed for 5 minutes with default
   settings, recording `n_dead_tup` every 30 s. Then apply the per-table settings from section
   2.4 and raise `autovacuum_vacuum_cost_limit` to 2000, and repeat. Done when you have two
   curves of dead tuples over time and can explain the difference with the trigger formula and
   the cost budget.
6. **(hard)** Set up the logical replication of the master tables into `loans_reporting`, then
   use it to rehearse a "zero-downtime" cutover: add a publication for `loan_applications` too,
   let it catch up, stop writes on the source, check row counts and a parity sample with
   `fn_parity_hash_loan` on both sides, and write the five-line runbook you would use. Done when
   counts and hashes match and the runbook names the sequence-sync step.

## 7. Interview questions

**Q: Why do you need pgbouncer, and what breaks in transaction mode?**
A: PostgreSQL runs a process per connection, so hundreds of mostly idle connections waste memory,
slow snapshot creation and oversubscribe CPUs under bursts. pgbouncer multiplexes many client
connections onto a small server pool. In transaction mode a server connection is handed back at
every commit, so any session state breaks: session SETs, session advisory locks, LISTEN, temp
tables across transactions, WITH HOLD cursors, and prepared statements unless pgbouncer 1.21+
with max_prepared_statements.

**Q: How do you set shared_buffers and work_mem?**
A: shared_buffers is PostgreSQL's page cache; a quarter of RAM is the conventional start because
the OS page cache holds the rest and very large values increase checkpoint work. work_mem is per
sort or hash node per backend, so the worst case is connections times nodes times work_mem; keep
the global value modest, raise it per role or session for reports, and watch temp_blks_written in
pg_stat_statements and log_temp_files to see spills.

**Q: VACUUM runs but dead tuples are not removed. Why?**
A: Something holds back the xmin horizon: a long or idle-in-transaction session, a lagging
replication slot, a standby with hot_standby_feedback running a long query, or an orphaned
prepared transaction. VACUUM VERBOSE reports "dead row versions cannot be removed yet, oldest
xmin"; pg_stat_activity.backend_xmin, pg_replication_slots and pg_prepared_xacts find the
culprit.

**Q: pg_dump or pg_basebackup?**
A: pg_dump is a logical, consistent snapshot of one database, portable across versions and
selective, but it only restores to the moment of the dump and rebuilds every index on restore.
pg_basebackup copies the cluster's files and, with continuous WAL archiving, allows
point-in-time recovery to any moment; it is the production backup, ideally through pgBackRest
or Barman, with scheduled restore tests to prove RPO and RTO.

**Q: Physical versus logical replication?**
A: Physical streaming ships WAL bytes and produces a read-only, byte-identical copy of the whole
cluster on the same major version: HA and read scaling. Logical replication decodes WAL into row
changes per table and applies them as SQL on a writable target, across versions: upgrades, CDC,
partial copies. Logical does not carry DDL or sequence values, and needs a replica identity
(normally the primary key) on each table that receives updates or deletes.

**Q: How does failover work for PostgreSQL, and what can go wrong?**
A: PostgreSQL provides promotion and timelines; a manager such as Patroni decides, using a leader
lock in etcd or Consul so only one node may be primary, promotes a standby and re-points the
others, while clients find the new primary through a proxy or libpq target_session_attrs. The
risks are split brain, which the consensus lock and fencing prevent, and data loss with
asynchronous replication, which synchronous replication to at least one standby removes at the
cost of commit latency. The old primary rejoins after pg_rewind.

**Q: What does "SERIALIZABLE" mean in Oracle, SQL Server and PostgreSQL?**
A: In Oracle it is snapshot isolation, so write skew is still possible and conflicts raise
ORA-08177. In SQL Server it is lock-based serializability with key-range locks, which blocks and
deadlocks more. In PostgreSQL it is Serializable Snapshot Isolation: snapshot reads plus
read/write dependency tracking that aborts one transaction with 40001 instead of blocking. SQL
Server's SNAPSHOT level and PostgreSQL's REPEATABLE READ are the snapshot isolation equivalents.

**Q: Name three behavioural differences that bite when moving code from Oracle or SQL Server to PostgreSQL.**
A: Oracle treats the empty string as NULL, PostgreSQL does not, so `WHERE col = ''` and NOT NULL
constraints change meaning. SQL Server's default READ COMMITTED takes shared locks so readers
block writers, while PostgreSQL never blocks readers, which changes code that relied on blocking
for correctness. Error handling differs: a PL/pgSQL EXCEPTION block is a subtransaction with a
cost, there are no autonomous transactions for logging, and T-SQL's TRY/CATCH with XACT_ABORT
semantics do not map one to one.

## 8. Recap

- Process per connection: pool with pgbouncer; transaction mode multiplexes but forbids session state.
- shared_buffers ~25% of RAM to start, effective_cache_size is only a planner hint, work_mem is per node per backend.
- max_wal_size and checkpoint_timeout trade checkpoint frequency (FPIs, write storms) against recovery time.
- First tools in an incident: pg_stat_activity, pg_blocking_pids, pg_stat_statements, slow log with auto_explain.
- Autovacuum: per-table scale factors for hot tables, a bigger cost limit, and hunt whatever holds the xmin horizon.
- Watch xid age; anti-wraparound vacuums cannot be cancelled and the server stops near 2 billion.
- pg_dump for logical copies and upgrades; pg_basebackup plus WAL archiving for PITR; test restores.
- Streaming replication = WAL bytes, read-only copy; logical = row changes, writable target, cross-version.
- Failover needs a manager (Patroni + etcd), fencing against split brain, and an honest RPO.
- Oracle: undo, '' is NULL, SERIALIZABLE is SI. SQL Server: locking RC by default, tempdb versions, key-range SERIALIZABLE. PostgreSQL: heap MVCC, SSI.
