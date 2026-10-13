# 5. Transactions, ACID and MVCC

**Time:** ~3 hours   **Prerequisites:** 02, 04

## 1. In plain words

A transaction is a group of changes that either all happen or none happen, and that other users
either see completely or not at all. The hard part is not the "all or nothing" (WAL and the
commit record give you that). The hard part is **isolation**: hundreds of sessions reading and
writing the same rows at once, each one expecting the database to behave as if it were alone.

Old databases did this with locks: a reader locks a row so a writer must wait, and a writer
locks a row so readers must wait. That serialises everything. PostgreSQL (and Oracle, and
YugabyteDB, and modern SQL Server with snapshot isolation) instead keep **multiple versions** of
each row. A writer creates a new version; readers keep seeing the old version until the writer
commits; nobody waits for anyone just to read. This is **multi-version concurrency control
(MVCC)**. The price is that old versions pile up and must be cleaned (VACUUM), and that "which
version do I see?" becomes a precise question with a precise answer you must be able to state.

The analogy: a shared document with tracked changes. Each editor sees the document as it was
when they opened it (their snapshot), plus their own edits. Others' pending edits are invisible
until accepted (commit). A periodic cleanup (VACUUM) removes the history nobody can scroll back to
anymore.

## 2. The real thing

### 2.1 ACID, precisely

- **Atomicity**: all of a transaction's changes commit together or none do. Mechanism: a single
  COMMIT record in WAL flips all of them from invisible to visible at once; abort leaves no
  trace to undo because new versions were never marked committed.
- **Consistency**: a transaction takes the database from one state satisfying all declared
  constraints (PK, FK, UNIQUE, CHECK, NOT NULL, triggers) to another. Mechanism: constraints are
  checked at statement end or, for `DEFERRABLE` constraints, at commit. Consistency is the
  property the *application* mostly owns; the database only checks what you declared.
- **Isolation**: concurrent transactions do not see each other's intermediate states, to the
  degree the isolation level promises. Mechanism: MVCC snapshots plus row and predicate locks.
- **Durability**: a committed transaction survives crashes. Mechanism: WAL fdatasync before
  returning COMMIT (module 04); with synchronous replication, also a copy on another machine.

### 2.2 The anomalies and the isolation levels

Anomaly definitions, each in one line:

| Anomaly | What you observe |
|---|---|
| **Dirty read** | you read a row version another transaction has written but not committed |
| **Non-repeatable read** | you read a row twice in one transaction and get different values because someone committed in between |
| **Phantom read** | you run the same WHERE twice and the second run returns rows that were not there (or lacks rows that were) |
| **Lost update** | two transactions read the same row, both compute a new value, both write; one write silently overwrites the other |
| **Write skew** | two transactions each read a set of rows, each writes a *different* row based on what it read, and the combination violates an invariant neither saw broken (the classic: two doctors both go off call because each saw "at least one other doctor is on call") |

PostgreSQL's levels and what each prevents:

| Level | Dirty | Non-repeatable | Phantom | Lost update | Write skew | Snapshot taken |
|---|---|---|---|---|---|---|
| READ UNCOMMITTED | prevented (behaves as READ COMMITTED) | no | no | no | no | per statement |
| **READ COMMITTED** (default) | prevented | **allowed** | allowed | allowed (unless you lock) | allowed | per statement |
| **REPEATABLE READ** | prevented | prevented | **prevented** (stronger than the SQL standard requires) | **detected**: concurrent update raises `serialization_failure` | allowed | at first statement |
| **SERIALIZABLE** | prevented | prevented | prevented | detected | **detected** via SSI, raises `serialization_failure` | at first statement + predicate tracking |

Important details for interviews:

- PostgreSQL never does dirty reads. There is no way to see uncommitted data from another session.
- READ COMMITTED takes a **new snapshot for every statement**. Within one statement you see a
  consistent state; between statements you see newer commits. Two `SELECT`s in one transaction
  can disagree.
- READ COMMITTED's UPDATE behaviour: if the row you are updating was changed and committed by
  someone else after your snapshot, PostgreSQL waits for them, then **re-evaluates your WHERE
  clause on the new version** (the "EvalPlanQual" mechanism) and updates that version. This is
  why `UPDATE accounts SET balance = balance - 100 WHERE id = 7` is safe at READ COMMITTED (the
  subtraction is applied to the latest version), but `SELECT balance; ... UPDATE SET balance =
  <computed in application>` is a lost update unless you `SELECT ... FOR UPDATE`.
- REPEATABLE READ gives you one snapshot for the whole transaction. If you try to update a row
  someone else updated after your snapshot, you get `ERROR: could not serialize access due to
  concurrent update` (SQLSTATE 40001). Your application must retry.
- SERIALIZABLE in PostgreSQL is **Serializable Snapshot Isolation (SSI)**: snapshot isolation
  plus tracking of read/write dependencies to detect the patterns that could produce a
  non-serialisable outcome. It never blocks readers; it aborts one transaction instead. Any
  transaction at this level must be retryable.
- Oracle's and YugabyteDB's "SERIALIZABLE" and SQL Server's "SNAPSHOT" are discussed in module 14
  because they do not all mean the same thing.

### 2.3 MVCC in PostgreSQL: xmin, xmax, snapshots

Every tuple version carries (module 02): `t_xmin` (the transaction that created it), `t_xmax`
(the transaction that deleted or replaced it, or 0), and hint bits in `t_infomask`. Transaction
ids (**xids**) are 32-bit integers handed out sequentially from a shared counter when a
transaction first writes (read-only transactions get no xid). Their commit status is in
`pg_xact`.

A **snapshot** is three things:

- `xmin`: the lowest xid still running when the snapshot was taken. Every xid below it is
  finished (committed or aborted) and can be judged from `pg_xact` alone.
- `xmax`: one past the highest xid that had been assigned. Every xid at or above it started after
  the snapshot and is invisible, whatever it later does.
- `xip[]`: the list of xids between xmin and xmax that were in progress at snapshot time.

`SELECT pg_current_snapshot()` prints it as `xmin:xmax:xip1,xip2`.

An xid X is **visible to the snapshot** if: X committed, and X < xmax, and X not in xip. (Or X
is my own xid and the change was made by an earlier command than the current one; the command
id `t_cid` handles that.)

**Tuple visibility rule** (`HeapTupleSatisfiesMVCC`, simplified but faithful):

```
visible(tuple, snapshot):
    if not visible_xid(tuple.xmin):          # inserter not committed-and-in-past from my view
        return false                           # (uncommitted, aborted, or started after snapshot)
    if tuple.xmax == 0 or xmax is lock-only:  # never deleted (xmax used only for row locks)
        return true
    if visible_xid(tuple.xmax):               # deleter committed and in my past
        return false                           # it is gone for me
    return true                               # deleter is in progress, aborted, or in my future: still there
```

Worked example. Snapshot `100:105:102`. Tuples:

| xmin | xmax | Visible? | Why |
|---|---|---|---|
| 95 | 0 | yes | inserter committed and < xmin; never deleted |
| 95 | 103 (committed) | no | deleter 103 is not in xip and < xmax: its delete is in my past |
| 95 | 102 (committed later) | yes | 102 was in progress at snapshot time (in xip): its delete is not in my view |
| 104 (committed) | 0 | yes | 104 < xmax and not in xip, committed: visible |
| 106 (committed) | 0 | no | 106 >= xmax: started after my snapshot |
| 101 (aborted) | 0 | no | inserter aborted |

This is the single most asked internals question. Practice it until you can do the table in
your head.

Two xids are reserved: 1 (bootstrap) and 2 (`FrozenTransactionId`, "visible to everyone, always").

### 2.4 VACUUM and bloat

An UPDATE writes a new version and sets `xmax` on the old one; a DELETE sets `xmax`. The old
version is **dead** once no snapshot can see it: when its `xmax` is committed and below every
active snapshot's `xmin` (the **oldest xmin horizon**). VACUUM:

1. Scans heap pages not marked all-visible in the VM; collects dead tuple TIDs.
2. Removes the matching entries from every index on the table (one pass per index, or in
   parallel workers for big tables).
3. Marks those heap line pointers `LP_UNUSED`, compacts pages, updates FSM and VM.
4. **Freezes** old tuples: any `xmin` older than `vacuum_freeze_min_age` (50 M xids) that is
   committed gets the `HEAP_XMIN_FROZEN` bits, meaning "treat as infinitely old". This is what
   makes wraparound survivable (next section).
5. Updates `pg_class.reltuples`/`relpages` and, if run as `VACUUM ANALYZE`, the statistics.

VACUUM does not return space to the OS (except trailing empty pages) and does not block reads
or writes; it takes `SHARE UPDATE EXCLUSIVE`, which only conflicts with DDL and other vacuums.
`VACUUM FULL` rewrites the table into a new file (needs `ACCESS EXCLUSIVE`, blocks everything,
needs 2x disk); use `pg_repack` for an online rewrite.

**Autovacuum** launches a worker for a table when
`dead_tuples > autovacuum_vacuum_threshold (50) + autovacuum_vacuum_scale_factor (0.2) x reltuples`.
For a 100 M row table that is 20 M dead tuples before it starts: far too lax for hot tables. Set
per table: `ALTER TABLE app_users SET (autovacuum_vacuum_scale_factor = 0.01)`. Autovacuum is
throttled by `autovacuum_vacuum_cost_delay` (2 ms) and `autovacuum_vacuum_cost_limit` (200);
raising the limit is the usual first tuning step on busy systems (module 14).

**What holds the horizon back** (why "VACUUM ran but removed nothing"): any long-running
transaction (even an idle `BEGIN` left open by a connection pool, `idle in transaction`), a
hot standby with `hot_standby_feedback`, a logical replication slot that is behind, a prepared
(2PC) transaction nobody finished. `SELECT backend_xmin, xact_start, state, query FROM
pg_stat_activity ORDER BY backend_xmin` finds the culprit; `idle_in_transaction_session_timeout`
prevents the most common one.

### 2.5 Transaction id wraparound

Xids are 32-bit and compared **circularly**: for any xid, the 2 billion xids before it are "in
the past" and the 2 billion after are "in the future". If a tuple's `xmin` is left unfrozen for
more than about 2 billion transactions, it suddenly appears to be in the future and becomes
invisible: data loss without data loss. Freezing prevents this by marking old tuples as frozen.
Safeguards:

- `autovacuum_freeze_max_age` (200 M): a table whose oldest unfrozen xid
  (`pg_class.relfrozenxid`) is older than this gets an **anti-wraparound autovacuum** even if
  autovacuum is disabled for it. These are aggressive vacuums that scan all non-frozen pages.
- `age(relfrozenxid)` tells you how far a table is; `age(datfrozenxid)` the database.
- At 40 M xids before the limit the server logs warnings; at 3 M it **refuses new commands**
  until a vacuum is run. Monitoring `age(datfrozenxid)` is non-negotiable in production.

Since PostgreSQL 9.4 freezing sets flag bits rather than overwriting `xmin` with 2, so forensic
`xmin` values survive. PostgreSQL 16 does not have 64-bit xids (an often-discussed change).
YugabyteDB does not have this problem at all: DocDB uses hybrid timestamps, not 32-bit xids.

### 2.6 Serializable Snapshot Isolation (SSI)

Snapshot isolation alone allows write skew because neither transaction writes what the other
read. SSI (Cahill, Rohm, Fekete 2008; in PostgreSQL since 9.1) tracks **rw-conflicts**: T1 read
something that T2 later wrote (T1 -> T2, "T1 read the old version"). A result from the paper: a
non-serialisable execution requires a transaction with both an incoming and an outgoing
rw-conflict edge (a "dangerous structure" of two consecutive edges), where the one at the end
committed first. PostgreSQL records reads as **SIRead predicate locks** on tuples, pages or
whole relations (they never block; they are bookkeeping) in a shared memory table sized by
`max_pred_locks_per_transaction`, detects the dangerous structure at commit or on write, and
aborts one participant with SQLSTATE 40001. Costs: memory and CPU for tracking, and false
positives (aborts of executions that would have been fine) when locks escalate to page or
relation level. Benefit: true serialisability with no read blocking, so a `SELECT` never waits.

Rule for using it: every transaction at SERIALIZABLE must be wrapped in a retry loop on 40001.
Read-only transactions can declare `READ ONLY DEFERRABLE` to wait for a safe snapshot and never
be aborted.

### 2.7 Locks: rows, tables, advisory

**Row locks** live in the tuple header, not in a lock table: `t_xmax` is set to the locker's
xid with `HEAP_XMAX_LOCK_ONLY` in the infomask (and a **MultiXact** id if several transactions
hold shared locks on the same row). Waiting for a row lock means waiting for the holder's
*transaction* to end: the waiter takes a lock on the holder's xid in the lock manager
(`XactLockTableWait`), and that is what shows in `pg_locks` as `locktype = transactionid`.

| Clause | Mode | Conflicts with | Use |
|---|---|---|---|
| `SELECT ... FOR UPDATE` | strongest | all other row locks, UPDATE, DELETE | read-modify-write; prevents lost updates |
| `FOR NO KEY UPDATE` | what UPDATE takes when no key column changes | FOR UPDATE, FOR NO KEY UPDATE, FOR SHARE | |
| `FOR SHARE` | shared | FOR UPDATE, FOR NO KEY UPDATE, UPDATE, DELETE | "do not delete this parent while I insert a child" (explicit) |
| `FOR KEY SHARE` | weakest, taken by FK checks on the parent row | only FOR UPDATE and key-changing UPDATE/DELETE | lets inserts of children not block non-key updates of the parent |

`NOWAIT` errors instead of waiting; `SKIP LOCKED` skips locked rows, which is how you build a
job queue (`SELECT ... FOR UPDATE SKIP LOCKED LIMIT 10`).

**Table (heavyweight) locks** have eight modes from `ACCESS SHARE` (any SELECT) to `ACCESS
EXCLUSIVE` (DROP, TRUNCATE, most ALTER TABLE, VACUUM FULL, `LOCK TABLE`). The conflict matrix is
in the manual; the one to memorise: `ACCESS EXCLUSIVE` conflicts with everything including
SELECT, and a request for it **queues behind** running transactions and **blocks everything
behind it** while it waits. This is how a tiny `ALTER TABLE ADD COLUMN` behind a 10-minute report
takes down an application: every new SELECT queues behind the ALTER. Always set
`lock_timeout` before DDL.

**Advisory locks** are application-defined lock keys (`pg_advisory_lock(bigint)`,
`pg_try_advisory_lock`, `pg_advisory_xact_lock`) stored in the same lock manager. Use them to
serialise an application action ("only one importer for customer 42 at a time") without a row to
lock. Session-level ones survive COMMIT and must be released explicitly; prefer the `_xact_`
forms.

### 2.8 Deadlocks

A deadlock is a cycle in the waits-for graph: T1 holds row A and wants row B; T2 holds row B and
wants row A. PostgreSQL does not prevent deadlocks; it **detects** them. When a backend has
waited `deadlock_timeout` (1 s) for a lock, it runs the deadlock detector: it walks the lock
manager's wait queues building the waits-for graph and looks for a cycle. If one exists, the
detecting transaction is aborted with `ERROR: deadlock detected` (SQLSTATE 40P01) and the log
shows both processes and their statements. The other transaction proceeds.

Prevention is an application discipline: acquire locks in a consistent global order (update
accounts in ascending id order; `SELECT ... FOR UPDATE ORDER BY id`), keep transactions short,
avoid user think-time inside transactions, and treat 40P01 like 40001: retry. The two-session
scripts in `code/m05_deadlock_a.sql` and `_b.sql` produce one on demand.

Note: in Berkeley DB, deadlock detection was also your job (`DB_ENV->set_lk_detect` or a
`db_deadlock` daemon), and page-level locking made deadlocks far more common than PostgreSQL's
row-level MVCC. Both are honest points in the migration story.

## 3. Code

### 3.1 Two-session isolation demos

Open two `psql loans` windows, A and B, and run `code/m05_isolation_session_a.sql` and
`code/m05_isolation_session_b.sql` **step by step** in the numbered order written in the comments.
The core of each demo:

```sql
-- Demo 1: READ COMMITTED non-repeatable read
-- A1
BEGIN;
SELECT balance FROM accounts WHERE id = 1;            -- 1000
-- B1
UPDATE accounts SET balance = balance - 100 WHERE id = 1;   -- autocommit
-- A2
SELECT balance FROM accounts WHERE id = 1;            -- 900: new statement, new snapshot
COMMIT;

-- Demo 2: REPEATABLE READ keeps its snapshot, and detects a concurrent update
-- A1
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT balance FROM accounts WHERE id = 1;            -- 900
-- B1
UPDATE accounts SET balance = balance - 100 WHERE id = 1;   -- 800 committed
-- A2
SELECT balance FROM accounts WHERE id = 1;            -- still 900
UPDATE accounts SET balance = balance - 50 WHERE id = 1;
-- ERROR:  could not serialize access due to concurrent update
ROLLBACK;

-- Demo 3: lost update at READ COMMITTED, and the FOR UPDATE fix
-- A1
BEGIN;
SELECT balance FROM accounts WHERE id = 2;            -- 500, application computes 500 - 300 = 200
-- B1
BEGIN;
SELECT balance FROM accounts WHERE id = 2;            -- 500, application computes 500 - 400 = 100
UPDATE accounts SET balance = 100 WHERE id = 2; COMMIT;
-- A2
UPDATE accounts SET balance = 200 WHERE id = 2; COMMIT;   -- B's withdrawal is lost: 200
-- Fix: SELECT ... FOR UPDATE in A1 and B1; B blocks until A commits, then re-reads 200.

-- Demo 4: write skew, only SERIALIZABLE catches it
-- invariant: at least one doctor on call per shift
-- A1
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM on_call WHERE shift = 'night' AND on_duty;   -- 2
-- B1
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM on_call WHERE shift = 'night' AND on_duty;   -- 2
-- A2
UPDATE on_call SET on_duty = false WHERE doctor = 'alice' AND shift = 'night';
COMMIT;                                                            -- ok
-- B2
UPDATE on_call SET on_duty = false WHERE doctor = 'bob' AND shift = 'night';
COMMIT;
-- ERROR:  could not serialize access due to read/write dependencies among transactions
-- At REPEATABLE READ both commit and nobody is on call.
```

### 3.2 Seeing MVCC in the tuple headers

```sql
-- txid_current() forces an xid to be assigned so you can match it to xmin/xmax
BEGIN;
SELECT pg_current_xact_id();                          -- e.g. 48213
UPDATE accounts SET balance = balance + 1 WHERE id = 1;
SELECT lp, t_xmin, t_xmax, t_ctid,
       (t_infomask & 128) <> 0 AS xmax_lock_only
FROM heap_page_items(get_raw_page('accounts', 0));
-- old version: t_xmax = 48213, t_ctid -> new slot. new version: t_xmin = 48213.
SELECT pg_current_snapshot();                         -- xmin:xmax:xip
COMMIT;

-- Row lock = xmax with LOCK_ONLY, no new version
BEGIN;
SELECT * FROM accounts WHERE id = 1 FOR UPDATE;
SELECT lp, t_xmin, t_xmax, (t_infomask & 128) <> 0 AS xmax_lock_only
FROM heap_page_items(get_raw_page('accounts', 0)) WHERE lp = 1;
COMMIT;
```

### 3.3 Who is blocking whom

```sql
SELECT a.pid, a.state, a.wait_event_type, a.wait_event,
       pg_blocking_pids(a.pid) AS blocked_by,
       age(now(), a.xact_start) AS xact_age,
       left(a.query, 80) AS query
FROM pg_stat_activity a
WHERE a.datname = current_database() AND a.pid <> pg_backend_pid()
ORDER BY a.xact_start;
```

### 3.4 A retry loop (what every SERIALIZABLE or REPEATABLE READ client needs)

```c
/* libpq pseudo-code: retry on 40001 (serialization_failure) and 40P01 (deadlock_detected) */
for (int attempt = 0; attempt < 5; ++attempt) {
    PQexec(conn, "BEGIN ISOLATION LEVEL SERIALIZABLE");
    PGresult *r = run_business_logic(conn);
    const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    if (sqlstate && (strcmp(sqlstate, "40001") == 0 || strcmp(sqlstate, "40P01") == 0)) {
        PQexec(conn, "ROLLBACK");
        backoff_ms(attempt);           /* exponential with jitter, so retries do not collide again */
        continue;
    }
    PQexec(conn, "COMMIT");
    break;
}
```

## 4. Under the hood (senior-level)

**Snapshot construction** (`GetSnapshotData`) scans the shared **ProcArray**, one entry per
backend holding its current xid and `xmin`. With hundreds of connections this scan is a
measurable per-statement cost, which is one reason PostgreSQL 14 reworked it into dense arrays
and why connection pooling helps even read-only workloads. The snapshot's `xmin` also becomes
the backend's advertised `xmin` in the ProcArray, and the minimum of those across all backends
is the horizon VACUUM must respect. An idle-in-transaction session with an old snapshot pins
that horizon for everyone.

**Hint bits and pg_xact.** Deciding "did xid 48213 commit?" means reading `pg_xact`, which is
an SLRU cache over files with 2 bits per xid. To avoid doing that for every tuple on every
read, the first reader who learns the answer sets `HEAP_XMIN_COMMITTED` or `HEAP_XMIN_INVALID`
(and the `XMAX` equivalents) in the tuple header. Subsequent visibility checks are then pure
bit tests. A freshly bulk-loaded table has no hint bits, so the first full scan after a load is
slow and dirties every page; `VACUUM` (or `COPY ... FREEZE` in the same transaction that created
the table) sets them up front.

**Command ids within a transaction.** `t_cid` records which statement in the transaction
created or deleted a version, so that statement 3 can see rows inserted by statement 2 but a
statement never sees its own partial output (`INSERT INTO t SELECT * FROM t` does not loop). The
combo-cid trick packs both an insert cid and a delete cid into the same field when one
transaction both inserts and deletes a tuple.

**Subtransactions.** `SAVEPOINT` and PL/pgSQL `BEGIN ... EXCEPTION` blocks create
subtransactions with their own xids, recorded in `pg_subtrans` as children of the parent. Each
backend caches up to 64 subtransaction xids in its ProcArray slot; beyond that
(`PGPROC_MAX_CACHED_SUBXIDS`) every snapshot must consult `pg_subtrans` on disk, a well-known
performance cliff ("subtransaction overflow"). A PL/pgSQL loop with an exception block per
iteration over 100,000 rows hits it (module 08).

**MultiXacts.** When two transactions both hold `FOR SHARE` or `FOR KEY SHARE` on a row (common:
two inserts of children referencing the same parent), `xmax` cannot hold both xids, so a
**MultiXactId** is allocated in `pg_multixact` and stored instead. MultiXacts have their own
wraparound and vacuum thresholds (`autovacuum_multixact_freeze_max_age`). Heavy FK traffic to a
hot parent row is the classic generator.

**How a deadlock is actually found.** Lock waits are queues in the shared lock table
(`LOCK`/`PROCLOCK` structures in partitions). The detector (`DeadLockCheck`) does a depth-first
search over "process waits for lock, lock is held by processes" edges, considering that waiters
ahead in a queue also block. If a cycle is found it may first try to **rearrange the wait queue**
to break a soft cycle (a case where reordering waiters avoids the deadlock) before aborting.
`deadlock_timeout` is the only cost knob: lower detects faster but runs the check more often.

**Why MVCC and not undo-based rollback (as in Oracle).** Oracle keeps one current row version in
the table and reconstructs older versions from undo segments when a reader needs them (ORA-01555
"snapshot too old" is the undo running out). PostgreSQL keeps all versions in the heap, so reads
never reconstruct anything, but the heap bloats and needs VACUUM. SQL Server's row-versioning
levels copy old versions into tempdb's version store. Each choice moves the cost: Oracle pays on
long readers, PostgreSQL pays on writers and vacuum, SQL Server pays in tempdb. Saying this
calmly is what "knows Oracle and SQL Server differences" means (module 14).

## 5. In production

- **The default is READ COMMITTED**, and most application code was written assuming it. Changing
  the default to REPEATABLE READ without adding retry loops turns rare races into user-visible
  errors. Change it per transaction where it matters.
- **Read-modify-write in application code** is the lost-update generator. Either do the
  arithmetic in SQL (`SET balance = balance - $1`), or lock the row first (`FOR UPDATE`), or use
  an optimistic version column (`UPDATE ... WHERE id = $1 AND version = $2`, check rows affected).
  Your PL/pgSQL workflow layer should use one of these for every state transition.
- **`idle in transaction`** is the silent killer: it holds row locks and pins the VACUUM
  horizon. Set `idle_in_transaction_session_timeout = '5min'` (or lower) and fix the client that
  leaves transactions open (usually a pool returning connections mid-transaction after an
  exception).
- **Table bloat you can measure**: `pgstattuple`, or `n_dead_tup` in `pg_stat_user_tables`
  versus `n_live_tup`. If `last_autovacuum` is old on a hot table, check for a horizon holder
  before touching autovacuum settings.
- **Wraparound alarm**: alert at `age(datfrozenxid) > 1 billion`. If it reaches the shutdown
  threshold, you are running `VACUUM FREEZE` on the biggest tables in single-user mode while the
  bank is down. Nobody wants to explain that.
- **Deadlocks in batch jobs**: two batch processes updating overlapping sets in different
  orders. Sort the ids, or take one advisory lock per logical batch. Log analysis: every
  `deadlock detected` message in the server log lists both statements; the fix is almost always
  ordering.
- **DDL under load**: `SET lock_timeout = '3s'` then `ALTER TABLE ...`; retry on timeout. Use
  `CREATE INDEX CONCURRENTLY`, `ADD COLUMN` without a volatile default (fast since 11), and
  `ADD CONSTRAINT ... NOT VALID` followed by `VALIDATE CONSTRAINT` to avoid long exclusive locks.
- **YugabyteDB** implements MVCC with hybrid timestamps in DocDB rather than xmin/xmax in heap
  tuples, has no VACUUM, defaults to SNAPSHOT (equivalent to REPEATABLE READ) and offers
  SERIALIZABLE and READ COMMITTED (the last with an explicit flag in older versions). Module 11.

## 6. Try it

1. **(easy)** Run demos 1 and 2 from the two-session scripts. Done when you have seen the
   non-repeatable read and the `could not serialize` error with your own eyes.
2. **(easy)** With `pageinspect`, update a row inside an open transaction and dump the two
   versions' `t_xmin`/`t_xmax`/`t_ctid`. Done when you can identify which is old, which is new,
   and what `pg_current_snapshot()` shows from a second session.
3. **(medium)** Build the visibility table from section 2.3 for snapshot `200:210:203,207` and
   the tuples (xmin 150, xmax 0), (150, 203 committed), (150, 205 committed), (207 committed, 0),
   (209 aborted, 0), (211 committed, 0). Then verify two of them experimentally by arranging the
   transactions. Done when all six are right and two are verified.
4. **(medium)** Reproduce a deadlock with `code/m05_deadlock_a.sql`/`_b.sql`. Read the server
   log entry. Then fix it by ordering and show it no longer deadlocks. Done when you can quote
   the log and explain the fix.
5. **(hard)** Create a table with 1 M rows, open a `BEGIN; SELECT 1;` in a second session at
   REPEATABLE READ and leave it, then update all rows twice in the first session and run
   `VACUUM VERBOSE`. Observe "dead row versions cannot be removed yet, oldest xmin: N". Commit
   the second session, vacuum again. Done when you can explain `oldest xmin` and its effect on
   table size.
6. **(hard)** Write the retry loop from 3.4 in C with libpq and test it under the write-skew
   demo running in a loop from two processes at SERIALIZABLE. Done when the invariant is never
   violated and you can report how many retries happened.

## 7. Interview questions

**Q: Explain MVCC in PostgreSQL.**
A: Every row version carries the xid that created it (xmin) and the xid that deleted or
replaced it (xmax). A transaction takes a snapshot (lowest running xid, next unassigned xid,
list of in-progress xids) and sees a version if its xmin is committed and in the snapshot's past
and its xmax is absent, aborted, in progress, or in the snapshot's future. Writers create new
versions instead of overwriting, so readers never block writers and vice versa. Dead versions
are reclaimed by VACUUM.

**Q: What does the default isolation level allow, and what does it prevent?**
A: READ COMMITTED prevents dirty reads only. Each statement gets a fresh snapshot, so repeated
reads can differ and phantoms can appear, and a read-then-write in application code can lose an
update. UPDATE on a concurrently changed row waits and re-evaluates the WHERE on the new version,
so arithmetic in SQL is safe.

**Q: What is the difference between REPEATABLE READ and SERIALIZABLE in PostgreSQL?**
A: Both use one snapshot for the whole transaction, so neither has non-repeatable or phantom
reads, and both abort on a write to a row changed concurrently. Only SERIALIZABLE detects write
skew, where two transactions read overlapping data and write disjoint rows; it does so with
SSI, tracking read/write dependencies and aborting a participant when a dangerous structure
appears. Both require clients to retry on SQLSTATE 40001.

**Q: What is write skew? Give an example.**
A: Two transactions each read a condition, both find it satisfied, and each writes a different
row such that the combination violates the condition. Two doctors each check "at least two on
call" and each removes themselves; under snapshot isolation both commit and nobody is on call.
Only SERIALIZABLE (SSI) or an explicit lock on the rows read prevents it.

**Q: Why is VACUUM necessary and what happens if it falls behind?**
A: Updates and deletes leave old versions in place; VACUUM removes those no transaction can
see, cleans their index entries, updates the free space and visibility maps, and freezes old
xids. If it falls behind, tables and indexes bloat, scans read dead space, index-only scans do
heap fetches, and eventually the xid wraparound protection forces an emergency vacuum or
refuses writes.

**Q: What is transaction id wraparound?**
A: Xids are 32-bit and compared circularly, so a tuple left unfrozen for about 2 billion
transactions would appear to be from the future and vanish. VACUUM freezes old tuples (marks
them visible to all); autovacuum forces a freeze when a table's relfrozenxid exceeds
autovacuum_freeze_max_age, and the server stops accepting commands when a database is 3 M xids
from the limit. Monitor age(datfrozenxid).

**Q: How does PostgreSQL handle deadlocks?**
A: It detects rather than prevents. After a backend waits deadlock_timeout for a lock, it
searches the waits-for graph in the shared lock table for a cycle and, if found, aborts its own
transaction with SQLSTATE 40P01, logging both statements. Applications prevent them by locking
rows in a consistent order and keeping transactions short, and retry when they happen.

**Q: How do row locks work internally?**
A: A row lock is recorded in the tuple header: xmax holds the locker's xid with a lock-only flag
(or a MultiXact id when several share it). A waiter blocks on the holder's transaction id in the
lock manager until that transaction ends. FOR UPDATE, FOR NO KEY UPDATE, FOR SHARE and FOR KEY
SHARE are the four strengths; FK checks use FOR KEY SHARE so child inserts do not block non-key
parent updates.

## 8. Recap

- ACID: atomic via one commit record; consistent via constraints; isolated via MVCC + locks; durable via WAL fsync.
- Anomalies: dirty read, non-repeatable read, phantom, lost update, write skew.
- READ COMMITTED (default): snapshot per statement; prevents only dirty reads; UPDATE re-evaluates on the new version.
- REPEATABLE READ: one snapshot; no phantoms in PostgreSQL; concurrent update raises 40001.
- SERIALIZABLE: SSI, detects write skew with predicate locks, aborts instead of blocking; always retry.
- Snapshot = xmin, xmax, xip. Visible if xmin committed-in-past and xmax not committed-in-past.
- VACUUM removes dead versions below the oldest xmin horizon, cleans indexes, sets FSM/VM, freezes.
- Long transactions and idle-in-transaction sessions hold the horizon and cause bloat.
- Wraparound: 32-bit circular xids; freeze; monitor age(datfrozenxid).
- Row locks live in xmax; waits are on the holder's xid; deadlocks are detected after deadlock_timeout and one side is aborted.
