# 4. Write-ahead logging and crash recovery

**Time:** ~3 hours   **Prerequisites:** 02

## 1. In plain words

A database must survive the power going out in the middle of a write. Writing the changed page
directly is not safe: a page write is not atomic, and a transaction usually touches several
pages, so a crash can leave half of them changed. The solution every serious database uses is
to first append a description of the change to a log file, force that log to disk, and only
then (lazily, whenever convenient) write the pages. After a crash, the log is replayed from the
last known-good point and the pages are repaired. This is **write-ahead logging** (WAL).

The analogy is the bound ledger in a bank branch: before any clerk changes a customer's folder,
the change is written in the ledger in pen. The folders may be updated later in a batch. If the
building floods, the folders are reconstructed from the ledger. The ledger is append-only, so
writing to it is fast (the pen never jumps around the page), and it is the one thing that is
never allowed to be lost.

WAL buys three things at once: durability (commit means "it is in the ledger"), fast writes
(one sequential append instead of many random page writes per commit), and, as a bonus,
replication (ship the ledger to another building and replay it there).

## 2. The real thing

### 2.1 The WAL stream and LSNs

WAL is one logical byte stream, append-only, cut into **segment files** of 16 MB in
`$PGDATA/pg_wal/` named like `000000010000000000000042` (timeline, high 32 bits, segment number).
A position in the stream is a **Log Sequence Number (LSN)**: a 64-bit byte offset, printed as two
hex halves, e.g. `0/1A2B3C40`. `pg_current_wal_lsn()` shows where the stream currently ends;
`pg_lsn` is a SQL type you can subtract to get bytes.

Every **WAL record** has a header (total length, transaction id, LSN of the previous record,
resource manager id, CRC) plus zero or more **block references** (which relation and block this
record modifies, optionally carrying a full copy of the page) and record-specific data. Resource
managers are the code modules that know how to replay their records: Heap, Heap2, Btree, Hash,
Gin, Gist, Transaction, XLOG (checkpoints), Storage (file create/truncate), and so on.

```
 a committed UPDATE of one indexed column, as WAL records:
   Heap   UPDATE      blkref 0: rel 1663/16384/16432 blk 1234 (old tuple's page)
                      blkref 1: ... blk 5678 (new tuple's page, if different)
   Btree  INSERT_LEAF blkref 0: rel .../16440 blk 77 (index leaf)
   Transaction COMMIT  xid 48213, timestamp
```

### 2.2 The write path

```
 backend                                    WAL writer / any committer
 ---------                                  ------------------------
 modify page in shared_buffers (exclusive content lock)
 XLogInsert(record)  -> copies record into wal_buffers (shared memory ring)
 stamp page->pd_lsn = record LSN            
 mark buffer dirty, release lock
 ...
 COMMIT: XLogInsert(COMMIT record)
         XLogFlush(commit LSN):  write() wal_buffers up to that LSN to the segment file,
                                 then fdatasync()                       <- the durable moment
         wait for sync replicas if synchronous_standby_names is set
         update pg_xact: xid = committed
         return "COMMIT" to client
```

The dirty data page has not been written. It will be written by the checkpointer, the background
writer, or a backend evicting it, and only after `XLogFlush(pd_lsn)` guarantees the WAL that
describes it is already durable. That single invariant (log before page) is what "write-ahead"
means.

Settings that shape this path:

| Setting | Default | Meaning |
|---|---|---|
| `wal_level` | `replica` | `minimal` (no replication), `replica` (streaming), `logical` (logical decoding too) |
| `synchronous_commit` | `on` | `off`: return before fdatasync (may lose the last few hundred ms of commits on crash, never corrupt); `local`; `remote_write`; `remote_apply` |
| `wal_sync_method` | `fdatasync` (Linux) | how WAL is forced to disk; `open_datasync` uses O_DSYNC writes |
| `wal_buffers` | 1/32 of shared_buffers, max 16 MB | in-memory ring; full ring forces early writes |
| `wal_writer_delay` | 200 ms | how often the WAL writer flushes in the background |
| `commit_delay` / `commit_siblings` | 0 / 5 | explicit group-commit tuning (rarely needed) |
| `full_page_writes` | `on` | see 2.4 |
| `wal_compression` | `off` | compress full-page images (pglz, lz4, zstd) |

### 2.3 Checkpoints

A **checkpoint** is the point from which recovery starts. Doing one means: write every dirty
buffer in `shared_buffers` to its data file, `fsync` all those files, then write a CHECKPOINT
record to WAL and store its **redo pointer** (the LSN at which the checkpoint *began*) in
`$PGDATA/global/pg_control`. Everything before the redo pointer is now reflected in the data
files, so WAL segments before it can be recycled (renamed and reused as future segments).

Checkpoints are triggered by time (`checkpoint_timeout`, default 5 min) or WAL volume
(`max_wal_size`, default 1 GB, a soft limit). They are spread out over
`checkpoint_completion_target` (default 0.9) of the interval so the I/O is a steady trickle
instead of a burst. `log_checkpoints = on` (default since 15) logs each one with buffer counts
and sync times; read them.

Trade-off: frequent checkpoints mean shorter recovery and less WAL kept, but more page writes
and more full-page images (next section). Most production systems raise `max_wal_size` to
several GB and `checkpoint_timeout` to 15 to 30 minutes.

### 2.4 Full-page writes and torn pages

An 8 KB page is written as two or more 4 KB OS pages (or sixteen 512-byte sectors). A crash in
the middle leaves a **torn page**: half old, half new. Replaying a delta WAL record ("set byte
40 of tuple 7 to 'A'") onto a torn page produces garbage. Therefore: the **first time a page is
modified after a checkpoint**, the WAL record carries a **full-page image (FPI)** of the whole
page instead of a delta. Recovery restores the image unconditionally, then applies later deltas
on top. Subsequent modifications until the next checkpoint log only deltas, because recovery
will have restored the page from the FPI first.

Consequences: WAL volume right after a checkpoint spikes (every touched page costs 8 KB); a
system that updates many distinct pages generates WAL dominated by FPIs. `pg_stat_wal.wal_fpi`
counts them. Longer checkpoint intervals reduce FPIs. `wal_compression = lz4` shrinks them
cheaply. `full_page_writes = off` is only safe on storage that guarantees atomic 8 KB writes
(ZFS with matching record size is the usual case).

### 2.5 Group commit and why WAL makes writes sequential

Each `fdatasync` costs a device round trip: around 0.1 ms on a datacentre NVMe, 1 to 2 ms on a
SATA SSD, 5 to 10 ms on a spinning disk (typical figures). If every commit paid its own fsync,
a spinning disk could not exceed about 100 commits per second. **Group commit** fixes this
naturally: `XLogFlush(lsn)` flushes everything in `wal_buffers` up to the latest written LSN,
so when ten backends commit within the same millisecond, the first one's fsync covers them all,
and the others see that the flushed LSN already exceeds theirs and return immediately. Commit
throughput becomes (fsyncs per second) x (commits arriving per fsync), which scales with
concurrency. `commit_delay` can add a small sleep to gather more siblings; it rarely helps on SSDs.

The write pattern on disk is therefore: one sequential append stream (WAL) that is fsynced
often, plus data pages written in large sorted batches at checkpoint time. Random writes are
deferred and batched. That is the whole performance argument for WAL and it is why every
transactional store since ARIES does it this way. LSM trees (module 10) push the same idea
further by making *all* writes sequential.

### 2.6 Crash recovery (redo)

On startup, `StartupXLOG()` reads `pg_control`. If the cluster was not shut down cleanly, it
takes the redo pointer of the last checkpoint and replays WAL from there to the end:

1. Read the next record; verify its CRC. A bad CRC or missing record means end of WAL.
2. For each block reference: if it carries an FPI, overwrite the page with it. Otherwise read
   the page and apply the delta **only if** `page->pd_lsn < record LSN`. This makes redo
   **idempotent**: a page that was already written with this change before the crash is not
   changed twice.
3. Track transaction status: COMMIT records mark xids committed in `pg_xact`; xids with no
   COMMIT record by end of WAL are simply left as in-progress, which MVCC treats as aborted.

Note what is missing: there is no **undo** phase. PostgreSQL never needs to roll back changes of
uncommitted transactions on disk, because MVCC leaves the old row version in place and the new
version is invisible unless its xid is committed (module 05). Abort is just "do not record a
commit". Oracle, SQL Server and Berkeley DB keep undo information (rollback segments, the log's
before-images) and have an undo phase; PostgreSQL's cost for this simplicity is VACUUM.

Recovery time is proportional to WAL since the last checkpoint, which is bounded by
`max_wal_size`. A 4 GB `max_wal_size` on fast storage replays in well under a minute.

### 2.7 Replication reuses WAL

**Streaming (physical) replication.** A standby runs continuous recovery: a `walreceiver`
process connects to the primary's `walsender`, which streams WAL records as they are written.
The standby applies them exactly like crash recovery, forever. With `hot_standby = on` it also
serves read-only queries. The standby is a byte-for-byte copy of the primary, same major
version required. `synchronous_standby_names` makes COMMIT wait until the standby has received
(`remote_write`), flushed (`on`), or applied (`remote_apply`) the commit record.

**Replication slots** make the primary keep WAL until a named consumer has confirmed it. A dead
consumer with a slot is the classic cause of `pg_wal` filling the disk; `max_slot_wal_keep_size`
caps the damage.

**Logical replication** decodes WAL into row-level changes (INSERT/UPDATE/DELETE with column
values) using `wal_level = logical` and a plugin (`pgoutput` for built-in publications, or
`wal2json`, Debezium). It can replicate a subset of tables, across major versions, and into
other systems. This is also the mechanism behind **change data capture (CDC)** cutovers in
module 12.

### 2.8 Point-in-time recovery

Take a **base backup** (`pg_basebackup`, a copy of the data directory taken while running, with
the WAL needed to make it consistent) and **archive every WAL segment** as it fills
(`archive_mode = on`, `archive_command` or `archive_library`). To recover to 14:32:10 yesterday:
restore the base backup, set `restore_command` and `recovery_target_time`, start. The server
replays archived WAL until that time and stops, creating a new **timeline** (history branches
so that WAL after the recovery point is not confused with the new future). This is how you
undo a `DELETE FROM customers` without a WHERE clause: not by rolling back, but by replaying
history up to one second before it.

### 2.9 Berkeley DB's log and transactional subsystem compared

Berkeley DB has the same architecture under different names, which is why your migration source
felt familiar:

| Concept | Berkeley DB | PostgreSQL |
|---|---|---|
| Log files | `log.0000000001` ... in the environment home | `pg_wal/0000000100000000000000xx` |
| Position | `DB_LSN {file, offset}` | 64-bit LSN `X/Y` |
| Logging style | **physical undo/redo**: records carry before- and after-images | **redo only**; no undo, MVCC provides rollback |
| Enable | `DB_INIT_LOG | DB_INIT_TXN` in `DB_ENV->open` | always on |
| Durability on commit | `txn->commit(0)` fsyncs; `DB_TXN_NOSYNC` skips; `DB_TXN_WRITE_NOSYNC` writes without fsync | `synchronous_commit` |
| Checkpoint | `DB_ENV->txn_checkpoint(kbyte, min, flags)`, called by you or a thread you create | checkpointer process, automatic |
| Recovery | you pass `DB_RECOVER` (normal) or `DB_RECOVER_FATAL` (catastrophic: replay all archived logs) at env open | automatic at startup |
| Log retention | `db_archive` / `DB_LOG_AUTO_REMOVE` after checkpoint | recycled after checkpoint unless slots/archive hold them |
| Replication | Berkeley DB HA (replication manager), log-shipping based | streaming physical, logical |

Two differences worth saying out loud. First, Berkeley DB's **undo** phase: after a crash it
replays forward to redo committed transactions, then rolls back uncommitted ones using the
before-images in the log. PostgreSQL skips that step entirely. Second, Berkeley DB leaves the
operational discipline to the application: if your server never called `txn_checkpoint`, the
log grew without bound and recovery took forever; if it opened without `DB_RECOVER` after a
crash, it got a corrupt environment. PostgreSQL does both for you. That is a real argument in
"why did you migrate", separate from the SQL argument.

## 3. Code

`code/m04_wal_inspect.sh` runs these from the shell on WSL; here are the pieces.

```sql
-- 1. Where is the end of WAL, and which segment file is that?
SELECT pg_current_wal_lsn(), pg_walfile_name(pg_current_wal_lsn());

-- 2. How many WAL bytes does one transaction generate?
SELECT pg_current_wal_lsn() AS before \gset
UPDATE loan_applications SET status = 'UNDER_REVIEW' WHERE id = 1;
SELECT pg_current_wal_lsn() - :'before'::pg_lsn AS wal_bytes;
-- run it twice: the first run after a checkpoint includes a full-page image (~8 KB),
-- the second only a delta (~100 bytes)

-- 3. Force a checkpoint and watch FPI count rise on the next write
CHECKPOINT;
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_wal;
UPDATE loan_applications SET status = 'SUBMITTED' WHERE id = 1;
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_wal;

-- 4. The page carries the LSN of its last change
SELECT lsn FROM page_header(get_raw_page('loan_applications', 0));

-- 5. Checkpoint state and settings
SELECT checkpoint_lsn, redo_lsn, timeline_id FROM pg_control_checkpoint();
SHOW checkpoint_timeout;  SHOW max_wal_size;  SHOW checkpoint_completion_target;

-- 6. Checkpoint activity since stats reset (16: pg_stat_bgwriter; 17 splits out pg_stat_checkpointer)
SELECT checkpoints_timed, checkpoints_req, buffers_checkpoint, buffers_backend
FROM pg_stat_bgwriter;
```

```bash
# 7. Decode the WAL records of your last transaction (run as the postgres OS user, or with read access)
SEG=$(psql -Atc "select pg_walfile_name(pg_current_wal_lsn())" loans)
sudo /usr/lib/postgresql/16/bin/pg_waldump -p /var/lib/postgresql/16/main/pg_wal "$SEG" | tail -20
# expect lines like:
# rmgr: Heap  len (rec/tot): 54/8266, tx: 812, lsn: 0/01A2B3C8, prev 0/01A2B390,
#   desc: UPDATE off 3 xmax 812 flags 0x00 ; new off 9 xmax 0, blkref #0: rel 1663/16384/16432 blk 0 FPW
# rmgr: Transaction len 34/34 ... desc: COMMIT 2026-10-03 ...
# "FPW" marks the full-page image

# 8. Crash test (safe on a dev machine): kill -9 the postmaster mid-transaction and watch redo
psql loans -c "BEGIN; UPDATE loan_applications SET status='X' WHERE id < 100; SELECT pg_sleep(30); COMMIT;" &
sleep 2
sudo kill -9 $(head -1 /var/lib/postgresql/16/main/postmaster.pid)
sudo service postgresql start
sudo tail -30 /var/log/postgresql/postgresql-16-main.log
# expect: "database system was interrupted", "redo starts at 0/...", "redo done at 0/...",
# and the uncommitted update is simply not there: no undo pass needed.
```

## 4. Under the hood (senior-level)

**Inserting into wal_buffers without a global lock.** `XLogInsert` first reserves space in the
WAL stream under a short spinlock (`insertpos_lck`): it bumps a shared "current insert
position" by the record length and gets back its start LSN. It then copies the record into the
ring buffer *without* holding that lock, protected by one of `NUM_XLOGINSERT_LOCKS` (8)
insertion locks that let `XLogFlush` know when all copies up to a position have finished. This
is why many backends can generate WAL concurrently; the serial point is a few dozen instructions.

**The WAL writer and the flush horizon.** Three shared LSNs matter: *insert* (reserved),
*write* (handed to `write()`), *flush* (fdatasynced). `XLogFlush(X)` checks if flush >= X; if
not, it takes `WALWriteLock`, writes up to the current write-able position, fsyncs, and advances
flush. Everyone waiting for an LSN below the new flush proceeds. That is the whole group-commit
mechanism; there is no separate "group commit" code path.

**Hint bits and WAL.** When a backend reads a tuple and determines its inserting transaction
committed, it sets `HEAP_XMIN_COMMITTED` in the tuple header to spare future readers a `pg_xact`
lookup. This dirties the page but writes no WAL record (the information is reconstructible). With
data checksums on, however, a torn page with a changed hint bit would fail its checksum, so the
engine logs an FPI the first time a page is hint-dirtied after a checkpoint (`XLOG_FPI_FOR_HINT`).
Checksums therefore slightly increase WAL volume. This explains "why did a SELECT generate WAL?"

**pg_xact and the commit record.** Transaction status lives in `pg_xact` (formerly `pg_clog`),
2 bits per xid: in progress, committed, aborted, sub-committed. The COMMIT WAL record is what
makes the commit durable; `pg_xact` pages are themselves written via the buffer manager style
SLRU cache and protected by the same WAL-before-page rule. Visibility checks (module 05) consult
`pg_xact` for xids not covered by hint bits.

**Replay and the standby's view.** The standby applies WAL in LSN order in a single startup
process (parallel replay is still not in 16). Hot-standby queries can conflict with replay: a
VACUUM record removing tuples the standby query still needs causes `max_standby_streaming_delay`
to pause replay, or cancels the query ("canceling statement due to conflict with recovery").
`hot_standby_feedback = on` tells the primary which xids the standby still needs, at the cost
of bloat on the primary.

**What `synchronous_commit = off` really risks.** The WAL writer flushes at least every
`wal_writer_delay` (200 ms) and whenever a page of WAL fills. With `off`, a crash loses at most
the commits of about the last 3 x 200 ms, and clients were told "COMMIT" for those. The data
files are never inconsistent, because the WAL-before-page rule still holds; recovery simply ends
earlier. For audit logs or idempotent bulk loads it is an acceptable 5x to 10x throughput trade;
for a funds transfer it is not.

**Timelines.** After PITR or promotion, the server increments the timeline id (the first 8 hex
digits of segment names). A `.history` file records the branch point. This prevents a recovered
server from reading WAL from a future it did not have, and lets a demoted old primary be
re-attached with `pg_rewind`.

## 5. In production

- **Disk for WAL.** Historically a separate spindle for `pg_wal` (symlink it) because its
  sequential, fsync-heavy pattern should not compete with random data reads. On NVMe this matters
  less, but a slow fsync (shared network storage, a VM with a write cache disabled) shows up as
  `WALSync` / `WALWrite` wait events and caps commit rate at a few hundred per second.
- **Checkpoint tuning symptom**: `checkpoints_req` much higher than `checkpoints_timed` means
  `max_wal_size` is too small; the system is checkpointing on WAL volume, generating excess FPIs
  and I/O bursts. Raise `max_wal_size`, keep `checkpoint_completion_target` at 0.9.
- **`pg_wal` fills the disk** (3 AM classic). Causes in order of likelihood: a replication slot
  whose consumer is dead (`SELECT slot_name, active, pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)) FROM pg_replication_slots`),
  `archive_command` failing (`pg_stat_archiver.failed_count`), a huge transaction. Fix the cause.
  Never `rm` in `pg_wal`: that is the ledger.
- **Testing recovery is part of having backups.** A base backup you have never restored is not
  a backup. Schedule a monthly restore to a scratch host with `recovery_target_time` and compare
  row counts.
- **For the migration story**: the Berkeley DB environment needed explicit checkpointing and
  `DB_RECOVER` discipline in your server's startup code; PostgreSQL/YugabyteDB handle it. And
  YugabyteDB (module 11) replaces PostgreSQL's WAL with a per-tablet Raft log, so durability is
  "a majority of replicas have the entry", not "fdatasync returned on one disk".
- **Synchronous replication latency**: `synchronous_commit = on` with a sync standby adds one
  network round trip plus the standby's fsync to every commit. Across availability zones that is
  1 to 2 ms; across regions 50 ms or more. Decide per transaction with `SET LOCAL
  synchronous_commit = off` for the non-critical ones.

## 6. Try it

1. **(easy)** Run snippet 2 twice: once right after `CHECKPOINT`, once immediately again.
   Done when you can explain the ~8 KB versus ~100 bytes difference.
2. **(easy)** Run `pg_waldump` on the current segment and identify a Heap UPDATE record, its
   Btree companion (update an indexed column first), and the COMMIT record. Done when you can read
   `blkref` and `FPW`.
3. **(medium)** Perform the kill -9 test in snippet 8. Read the log lines. Done when you can
   name the redo start LSN, the redo end LSN, and confirm the uncommitted UPDATE is absent with no
   undo.
4. **(medium)** Measure commit throughput: a loop of 2000 single-row inserts each in its own
   transaction, with `synchronous_commit = on` then `off`. Then wrap all 2000 in one transaction.
   Done when you have three timings and can explain each in terms of fsync count.
5. **(hard)** Set up PITR on your WSL instance: `archive_mode = on`, `archive_command = 'cp %p
   /var/lib/postgresql/wal_archive/%f'`, take `pg_basebackup -D /tmp/base -Fp -Xs`, make some
   changes with timestamps, delete a table, then restore to a second data directory on port
   5433 with `recovery_target_time` set to just before the delete. Done when the table is back in
   the restored instance and the new timeline id is visible in `pg_control_checkpoint()`.

## 7. Interview questions

**Q: What is write-ahead logging and what problem does it solve?**
A: Every change is described in an append-only log record that is forced to disk before the
modified data page is allowed to reach disk. After a crash, the log is replayed from the last
checkpoint to repair the data files. It solves durability under non-atomic multi-page writes,
and it turns random page writes into one sequential log stream plus deferred batched page writes.

**Q: What happens between COMMIT and the data being durable?**
A: The backend writes a COMMIT record into the WAL buffers, then calls XLogFlush, which writes
the buffers to the current WAL segment and fdatasyncs it. When that returns, the commit is
durable and the client is told COMMIT. The modified heap and index pages are still only in shared
buffers; they reach disk later at a checkpoint or on eviction.

**Q: What is a checkpoint and why not checkpoint constantly?**
A: A checkpoint writes all dirty buffers to disk, fsyncs, and records a redo pointer so recovery
can start from there and older WAL can be recycled. Checkpointing often shortens recovery and
saves WAL space but costs page writes and, more importantly, more full-page images, because the
first change to each page after a checkpoint must log the whole page.

**Q: What are full-page writes for?**
A: Protection against torn pages. An 8 KB page write is not atomic, so a crash can leave it half
updated; applying a delta onto it would corrupt it. The first modification of a page after each
checkpoint logs the entire page, so recovery can restore a known-good image before applying
deltas.

**Q: How does PostgreSQL roll back an uncommitted transaction after a crash?**
A: It does not have to. There is no undo log. MVCC keeps the old row versions and the new ones
are invisible unless their transaction is marked committed in pg_xact. Recovery only redoes; any
transaction with no COMMIT record at the end of WAL is treated as aborted. VACUUM later removes
its dead tuples.

**Q: How does group commit work?**
A: XLogFlush flushes WAL up to the latest written position, not just the caller's LSN. When
several backends commit concurrently, the first fsync covers all of them and the rest find their
LSN already flushed. Commit throughput therefore scales with concurrency rather than being capped
at one fsync per commit.

**Q: How does replication use WAL?**
A: Physical streaming replication ships WAL records to a standby that applies them continuously
as if in perpetual crash recovery, producing a byte-identical copy. Logical replication decodes
the same WAL into row changes so a subset of tables can be replicated across versions or into
other systems. Replication slots make the primary retain WAL until the consumer has it.

**Q: How did Berkeley DB's logging differ from PostgreSQL's?**
A: Same write-ahead principle and LSNs, but Berkeley DB logs before- and after-images and runs an
undo pass after redo; PostgreSQL logs redo only and relies on MVCC for rollback. Berkeley DB also
leaves checkpointing and recovery invocation to the application, which is an operational burden a
server database removes.

## 8. Recap

- WAL is an append-only stream of change records; LSN is a byte position in it.
- Rule: a dirty page is never written before the WAL describing it is flushed (`pd_lsn` check).
- COMMIT = write COMMIT record + fdatasync WAL. Data pages are written later.
- Checkpoint = flush all dirty pages, fsync, record redo pointer; recovery starts there.
- First change to a page after a checkpoint logs a full-page image to beat torn pages.
- Group commit falls out of flushing to the latest LSN; throughput scales with concurrency.
- Redo is idempotent via LSN comparison; there is no undo phase thanks to MVCC.
- Streaming replication is perpetual redo on another machine; logical replication decodes WAL to rows.
- PITR = base backup + archived WAL + recovery target; creates a new timeline.
- Berkeley DB: same ideas, undo/redo images, application-driven checkpoint and recovery.
