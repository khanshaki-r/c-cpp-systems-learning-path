# 2. Storage engines: pages and heap files

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

A database never reads or writes "a row". It reads and writes **pages**: fixed-size blocks,
8 KB in PostgreSQL, that are the unit of I/O between disk and memory. A table is a file (or a
few files) made of these pages laid end to end. Rows are packed into the pages; an index entry
points at a row by saying "page 1234, slot 7".

The analogy is a filing cabinet with fixed-size drawers. You never pull one sheet out of the
cabinet; you pull a whole drawer onto your desk, find the sheet, maybe change it, and later
push the drawer back. Your desk is the buffer pool: it holds a limited number of drawers, and
when it is full you have to put one back (eviction) before pulling out another. If a drawer on
your desk has been written on (dirty), it must be put back carefully (written to disk) and not
just dropped.

Everything in later modules depends on this picture. An index is fast because it reads 3 or 4
pages instead of 100,000. VACUUM exists because deleted rows still occupy page space. WAL
exists because a crash in the middle of pushing a drawer back leaves a half-written drawer.

## 2. The real thing

### 2.1 Files on disk

```
$PGDATA/
  base/<database oid>/<relfilenode>          main fork, data pages, 1 GB segments: 16432, 16432.1, 16432.2 ...
  base/<database oid>/<relfilenode>_fsm      free space map fork
  base/<database oid>/<relfilenode>_vm       visibility map fork
  base/<database oid>/<relfilenode>_init     unlogged table init fork
  pg_wal/                                    WAL segments, 16 MB each by default
  pg_xact/                                   commit status of every transaction (2 bits per xid)
  global/                                    shared catalogs (pg_database, pg_authid ...)
```

`SELECT pg_relation_filepath('loan_applications')` gives the path. A table over 1 GB is split
into numbered segment files so that file-size limits and backup tools never see huge files.
Each index is its own relation with its own files.

### 2.2 The 8 KB heap page (slotted page)

```
 offset 0
 +--------------------------------------------------------------------------+
 | PageHeaderData (24 bytes)                                                |
 |  pd_lsn (8)       LSN of the last WAL record that touched this page      |
 |  pd_checksum (2)  page checksum, if enabled at initdb                    |
 |  pd_flags (2)     PD_HAS_FREE_LINES, PD_PAGE_FULL, PD_ALL_VISIBLE        |
 |  pd_lower (2)     end of line pointer array                              |
 |  pd_upper (2)     start of tuple data                                    |
 |  pd_special (2)   start of special space (8192 for heap: none)           |
 |  pd_pagesize_version (2), pd_prune_xid (4)                               |
 +--------------------------------------------------------------------------+
 | ItemIdData[1] ItemIdData[2] ItemIdData[3] ...   4 bytes each             |
 |  lp_off:15  lp_flags:2 (UNUSED/NORMAL/REDIRECT/DEAD)  lp_len:15          |
 +------------------------------ pd_lower ----------------------------------+
 |                                                                          |
 |                      free space (grows down and up)                      |
 |                                                                          |
 +------------------------------ pd_upper ----------------------------------+
 | tuple 3 | tuple 2 | tuple 1 |   tuples are added from the END backwards  |
 +------------------------------ pd_special --------------------------------+
 | special space: empty for heap pages; btree pages keep sibling pointers   |
 +--------------------------------------------------------------------------+ offset 8192
```

Why slotted? A tuple's address (its **TID**, "tuple identifier" = block number + line pointer
index, e.g. `(1234,7)`) must stay stable so indexes can point at it, but tuples have variable
length and get moved when the page is compacted. The line pointer array is the indirection:
indexes point at slot 7, slot 7 points at wherever the bytes currently are. Compaction (page
pruning) rewrites `lp_off` values without touching any index.

Line pointer flags matter for module 03 and 05: `LP_DEAD` marks a slot whose tuple is gone and
can be reused after VACUUM; `LP_REDIRECT` is how HOT update chains are shortened.

### 2.3 The heap tuple

```
 HeapTupleHeaderData (23 bytes, padded to 24)
   t_xmin (4)       xid that inserted this version
   t_xmax (4)       xid that deleted/updated it, or 0 (also used for row locks)
   t_cid (4)        command id within the inserting transaction (or t_xvac)
   t_ctid (6)       TID of this tuple, or of the NEWER version if updated
   t_infomask2 (2)  number of attributes + HOT flags
   t_infomask (2)   HAS_NULL, HAS_VARWIDTH, XMIN_COMMITTED, XMIN_INVALID, XMAX_COMMITTED,
                    XMAX_INVALID, XMAX_IS_MULTI, UPDATED, HOT_UPDATED, HEAP_ONLY_TUPLE ...
   t_hoff (1)       offset of user data (header + null bitmap, MAXALIGNed to 8)
 null bitmap        1 bit per attribute, only if HAS_NULL
 user data          columns in declaration order, each aligned per its type ('c','s','i','d')
```

Every row carries 24 bytes of header plus alignment padding. A table with columns
`(id int8, flag bool, amount int8)` wastes 7 bytes after `flag` because `int8` needs 8-byte
alignment. Reordering to `(id int8, amount int8, flag bool)` saves 7 bytes per row. For a 100
million row table that is 700 MB. This is the "column tetris" trick, and it is the same
alignment rule you know from C structs.

Variable-length columns (`text`, `bytea`, `numeric`, `jsonb`) start with a **varlena** header:
1 byte for short values under 127 bytes, 4 bytes otherwise, with bits saying whether the value
is compressed or stored externally (TOAST).

### 2.4 TOAST: rows bigger than a page

A tuple must fit in one page. "The Oversized-Attribute Storage Technique" handles big values.
When a tuple would exceed about 2 KB (`TOAST_TUPLE_THRESHOLD`, 2040 bytes), the engine tries, in
order, for each toastable column from largest to smallest: compress it (pglz by default, lz4 if
`default_toast_compression = lz4`), then if still too big move it out of line into the table's
**TOAST table** (`pg_toast.pg_toast_<oid>`) in chunks of about 2000 bytes, leaving an 18-byte
pointer in the main tuple. Per-column storage strategies:

| Strategy | Compress | Out of line | Default for |
|---|---|---|---|
| PLAIN | no | no | fixed-width types |
| MAIN | yes | only as last resort | |
| EXTENDED | yes | yes | most varlena types (text, bytea, jsonb) |
| EXTERNAL | no | yes | set it for big values you substring often |

Consequences: `SELECT *` on a table with a 50 KB `jsonb` column fetches from the TOAST table on
every row (an index lookup into the toast index per chunk). `SELECT id, status` does not touch
TOAST at all. This is one concrete reason `SELECT *` is an anti-pattern (module 07).

### 2.5 Free space map and visibility map

**FSM** (`_fsm` fork): for each heap page, one byte recording roughly how much free space it
has (in 1/256ths of a page, so 32-byte granularity), organised as a binary tree over pages so
that "find me a page with at least 300 bytes free" is a tree descent, not a scan. `INSERT`
consults it; VACUUM updates it.

**VM** (`_vm` fork): two bits per heap page: *all-visible* (every tuple on the page is visible
to every current and future transaction, nothing to vacuum) and *all-frozen* (no xid on the
page can ever wrap around). Two users:

1. VACUUM skips all-visible pages: this is why vacuuming a mostly-static 100 GB table is cheap.
2. **Index-only scans** (module 03) can trust the index entry for a page marked all-visible and
   skip the heap fetch; for other pages they must visit the heap to check visibility. The
   "Heap Fetches" line in EXPLAIN counts those visits.

Any write to a page clears its VM bits.

### 2.6 How a sequential scan reads a table

`Seq Scan` calls `heap_getnextslot()`, which asks the buffer manager for block 0, 1, 2, ... in
order. On each page it walks the line pointers, skips `LP_UNUSED`/`LP_DEAD`, and checks each
tuple's visibility against the snapshot. Three details make this faster than naive:

- **Page-at-a-time visibility**: on first touch of a page the scan computes visibility for all
  tuples on it under one buffer lock, then releases the lock and returns tuples from a local array.
- **Ring buffer**: a scan of a table larger than a quarter of `shared_buffers` uses a small
  private ring of buffers (256 KB) so it does not evict the entire working set of other
  queries. Yes: a big seq scan deliberately does *not* cache the table.
- **Synchronised scans**: a second seq scan of the same big table starts where the first one
  currently is, so both share the pages being read.
- The OS does read-ahead on the sequential file reads, which is why `seq_page_cost` (1.0) is
  set four times cheaper than `random_page_cost` (4.0) in the planner's cost model.

### 2.7 Row store vs column store

PostgreSQL stores a row's columns together on one page (**row store**, N-ary storage model).
Good when you read whole rows by key (OLTP: your login path, a loan application screen). A
**column store** stores each column in its own file or block run (Parquet, ClickHouse,
Citus columnar, Oracle In-Memory, SQL Server columnstore indexes). Good when you aggregate one
or two columns across millions of rows: you read only those columns, and runs of similar values
compress 10x or better. Bad for point lookups and single-row updates, which must touch every
column's structure. PostgreSQL core has no column store; extensions and separate analytic
databases fill that role. Answer "row store, optimised for OLTP" when asked how PostgreSQL stores data.

### 2.8 The buffer pool

`shared_buffers` is an array of 8 KB slots (default 128 MB; typical production 25% of RAM) plus
a parallel array of `BufferDesc` headers (tag = relation+fork+block, state bits: dirty, valid,
pin count, usage count, I/O in progress) and a shared hash table from tag to slot.

**Pin**: a backend that is reading a page increments the pin count; the page cannot be evicted
while pinned. **Content lock**: a lightweight lock on the page, shared for reads, exclusive
for modifications. Pins are held for the duration of use; content locks for microseconds.

**Replacement: clock sweep.** A hand moves around the slot array. Each slot has a usage count
(0 to 5, incremented on each pin). When looking for a victim, the hand decrements the count of
each slot it passes and takes the first slot with count 0 and pin count 0. This approximates LRU
without the lock contention of a true LRU list. Dirty victims are written out (`pwrite`) before
reuse; the **background writer** tries to keep some clean buffers ahead of the hand so backends
rarely have to write.

Two caches, not one: PostgreSQL's `shared_buffers` sits on top of the OS page cache. A "read"
that misses `shared_buffers` often hits the OS cache and costs microseconds, not a disk seek.
EXPLAIN's `shared hit` versus `read` cannot distinguish OS cache from disk; `track_io_timing`
can (module 06).

### 2.9 fsync and durability

A `write()` syscall puts bytes in the OS page cache; the disk has them only after `fsync()` /
`fdatasync()`, and even then the drive's own write cache must be flushed (the kernel sends a
cache flush command; cheap consumer SSDs sometimes lie). PostgreSQL's rules:

- WAL is `fdatasync`ed (default `wal_sync_method` on Linux) before `COMMIT` returns, when
  `synchronous_commit = on`.
- Data pages are written whenever evicted or at **checkpoint**, and all files touched since the
  last checkpoint are `fsync`ed at the end of the checkpoint. Between checkpoints data pages on
  disk are stale; WAL is the source of truth (module 04).
- If `fsync()` ever fails, PostgreSQL 12+ **PANICs** and restarts, replaying WAL. This was the
  outcome of the 2018 "fsyncgate" discussion: Linux may drop dirty pages after reporting an
  fsync error, so retrying is unsafe; crash recovery from WAL is the only correct response.
- `fsync = off` is a benchmark-only setting; a power loss can corrupt the cluster.

## 3. Code

`code/m02_page_inspect.sql` walks through all of this with the `pageinspect` extension.

```sql
CREATE EXTENSION IF NOT EXISTS pageinspect;

-- 1. Where is the table and how big in pages?
SELECT pg_relation_filepath('loan_applications'),
       pg_relation_size('loan_applications') / 8192 AS main_pages,
       pg_relation_size('loan_applications', 'fsm') / 8192 AS fsm_pages,
       pg_relation_size('loan_applications', 'vm')  / 8192 AS vm_pages;

-- 2. The header of page 0. pd_lower/pd_upper tell you how full it is.
SELECT * FROM page_header(get_raw_page('loan_applications', 0));

-- 3. Every line pointer and tuple header on page 0.
--    lp_off/lp_len: where the bytes are. t_xmin/t_xmax: MVCC (module 05).
--    t_ctid pointing elsewhere means "this version was updated, newer one is there".
SELECT lp, lp_off, lp_len, lp_flags, t_xmin, t_xmax, t_ctid, t_hoff,
       (t_infomask & 1) <> 0 AS has_null,
       (t_infomask & 16384) <> 0 AS hot_updated,
       (t_infomask2 & 32768) <> 0 AS heap_only
FROM heap_page_items(get_raw_page('loan_applications', 0));

-- 4. Watch an UPDATE create a new version on the same page (HOT) or elsewhere.
UPDATE loan_applications SET status = 'UNDER_REVIEW' WHERE id = 1;
SELECT lp, t_xmin, t_xmax, t_ctid FROM heap_page_items(get_raw_page('loan_applications', 0))
WHERE lp <= 3;

-- 5. TOAST: which table holds the big values and how many chunks
SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'documents';
SELECT chunk_id, count(*) AS chunks, sum(length(chunk_data)) AS bytes
FROM pg_toast.pg_toast_<oid>   -- substitute the oid printed above
GROUP BY chunk_id LIMIT 5;

-- 6. Alignment padding: compare the row size of two column orders
CREATE TABLE pad_bad  (a int8, b bool, c int8, d bool, e int8);
CREATE TABLE pad_good (a int8, c int8, e int8, b bool, d bool);
INSERT INTO pad_bad  SELECT g, true, g, false, g FROM generate_series(1, 100000) g;
INSERT INTO pad_good SELECT g, g, g, true, false FROM generate_series(1, 100000) g;
SELECT pg_relation_size('pad_bad') AS bad, pg_relation_size('pad_good') AS good;
-- bad is noticeably larger: 7 bytes of padding after each bool before the next int8

-- 7. The buffer pool: what fraction of the table is cached, and usage counts
CREATE EXTENSION IF NOT EXISTS pg_buffercache;
SELECT c.relname, count(*) AS buffers, sum(CASE WHEN isdirty THEN 1 ELSE 0 END) AS dirty,
       round(avg(usagecount), 2) AS avg_usage
FROM pg_buffercache b
JOIN pg_class c ON b.relfilenode = pg_relation_filenode(c.oid)
WHERE c.relname IN ('loan_applications', 'customers')
GROUP BY c.relname;

-- 8. Free space map: how much room each page claims to have
SELECT blkno, avail FROM pg_freespace('loan_applications') ORDER BY blkno LIMIT 10;

-- 9. Visibility map bits
SELECT blkno, all_visible, all_frozen FROM pg_visibility('loan_applications') LIMIT 10;
```

Expected observation for step 4: the old tuple's `t_xmax` becomes your transaction id and its
`t_ctid` points at the new slot; the new tuple has `t_xmin` equal to that same id. The old
version stays until VACUUM. That is MVCC made visible (module 05).

## 4. Under the hood (senior-level)

**Why 8 KB.** It is a compile-time constant (`BLCKSZ`). It must be a multiple of the OS page
(4 KB) and of the disk sector (512 B or 4 KB). 8 KB means one page write is two OS pages, which
is *not* atomic on most storage: a crash can leave a page half old, half new (a **torn page**).
WAL full-page writes exist for this (module 04). Larger pages give better sequential read
efficiency and more fan-out in B-trees; smaller pages waste less on point updates. 8 KB is a
30-year-old compromise that still works.

**`pd_lsn` is the hinge between storage and WAL.** When a page is modified, the LSN of the WAL
record describing the change is stamped into `pd_lsn`. The buffer manager will not write a
dirty page to disk until WAL up to `pd_lsn` has been flushed (`XLogFlush(pd_lsn)` in
`FlushBuffer()`). That one rule *is* write-ahead logging. Hint-bit updates (setting
`XMIN_COMMITTED` after checking `pg_xact`) are the exception: they are not WAL-logged unless
checksums are on, because losing them only costs a re-check.

**Page pruning (opportunistic).** When a backend reads a page whose `pd_prune_xid` is older
than the oldest running transaction, and the page is nearly full, `heap_page_prune()` removes
dead tuple versions and compacts the page *without* VACUUM, in the reader's process. This is why
a hot table can have many updates per second without exploding: single-page vacuum, done by whoever
is holding the page anyway. It cannot remove the line pointers themselves (indexes may still
point at them), so it marks them `LP_DEAD`; only VACUUM, which also cleans the indexes, can set
them back to `LP_UNUSED`.

**Buffer header spinlocks and partitions.** The buffer mapping hash table is split into 128
partitions, each with its own lightweight lock, so that lookups for different pages rarely
contend. Each buffer's state (pin count, usage count, flags) is packed into one 32-bit
atomic and updated with compare-and-swap; the spinlock is taken only for complex transitions.
This is the standard pattern from 04_Concurrency applied to a cache: partition the lock, use
atomics for the hot path.

**What `read` really costs.** On a buffer miss the backend calls `smgrread()` -> `mdread()` ->
`pread()` on the segment file. If the OS page cache has it, this is a memory copy (a few us).
If not, a real device read. With `effective_io_concurrency` > 0, Bitmap Heap Scans issue
`posix_fadvise(WILLNEED)` for upcoming pages so the kernel prefetches. PostgreSQL 16 and
earlier do not use asynchronous I/O (io_uring work landed later), so a single backend's reads
are synchronous: one outstanding read at a time. That is why a big index range scan on cold
data is latency-bound, and why keeping the working set in memory matters so much.

**Checksums.** Off by default before 18 (`initdb --data-checksums` turns them on); when on,
every page carries a CRC-style checksum verified on read, which detects storage-level
corruption (bit rot, bad firmware) rather than silently returning garbage. Turn them on in
production unless CPU is genuinely the constraint.

**Berkeley DB pages.** Berkeley DB uses the same slotted-page design (its B-tree leaf pages have
an index array growing down and items growing up), a configurable page size (512 B to 64 KB,
default often 4 KB), and the same "mpool" buffer manager with pin counts and LRU-ish eviction,
also mapped as shared memory across processes. When your migration engine opened a cursor and
walked `DB_NEXT`, the library was doing exactly these page fetches via its mpool. The record you
got back was an opaque `DBT` holding your packed struct; PostgreSQL's equivalent is the tuple
with a header the engine understands. That is the whole difference between "key-value" and
"relational" at the page level.

## 5. In production

- **Bloat.** Every UPDATE writes a new tuple; every DELETE leaves the old one. If VACUUM cannot
  keep up (long transactions block it, or autovacuum is throttled), tables and indexes grow with
  dead space, scans read more pages for the same rows, and the buffer pool holds garbage.
  `pgstattuple('loan_applications')` reports `dead_tuple_percent` and `free_percent`. Module 05
  and 14 cover tuning.
- **Column order.** Put fixed-width 8-byte columns first, then 4-byte, then 2-byte, then
  booleans, then variable-length. Free space, zero risk, do it at design time. (You cannot
  reorder an existing table without rewriting it.)
- **Wide JSON blobs** in the main table punish every `SELECT *`. Keep documents in a separate
  table joined on demand, or at least never select them in list queries.
- **shared_buffers vs OS cache.** Setting `shared_buffers` to 80% of RAM starves the OS cache
  and makes double-buffering worse. 25% is the common starting point; measure buffer hit ratio
  (`pg_stat_database.blks_hit / (blks_hit + blks_read)`) and adjust.
- **"Disk full" at 3 AM.** Usually `pg_wal` growing because archiving failed or a replication
  slot is holding WAL, or a runaway table rewrite. Check `pg_replication_slots`,
  `pg_stat_archiver`, and `du -sh $PGDATA/*`. Never delete WAL files by hand.
- **Checksum failures** in the log mean a hardware or filesystem problem. Restore the page from a
  replica or backup; do not `zero_damaged_pages` unless you have accepted data loss.

## 6. Try it

1. **(easy)** Run `code/m02_page_inspect.sql` steps 1 to 3 against `loan_applications`.
   Done when you can point at `pd_lower`, `pd_upper` and say how many bytes are free on page 0.
2. **(easy)** Run step 6 (alignment). Done when you have the two sizes and can explain the
   difference in bytes per row from alignment rules.
3. **(medium)** Insert 200,000 rows into a scratch table with a 3 KB `text` column. Compare
   `pg_relation_size` of the main table and of its TOAST table. Change the column to
   `STORAGE EXTERNAL`, rewrite the table (`VACUUM FULL`) and compare again. Done when you can
   explain where each byte lives.
4. **(medium)** Run a `SELECT count(*)` on a table larger than `shared_buffers / 4` with
   `EXPLAIN (ANALYZE, BUFFERS)` twice. Note that `shared read` stays high on the second run.
   Done when you can explain the ring buffer behaviour.
5. **(hard)** With `pageinspect`, update one row 5 times in separate transactions without
   VACUUM. Dump `heap_page_items` after each. Then run `VACUUM loan_applications` and dump
   again. Done when you can describe the chain of `t_ctid` pointers, the `LP_DEAD`/`LP_REDIRECT`
   line pointers, and what VACUUM removed.

## 7. Interview questions

**Q: Describe the layout of a PostgreSQL heap page.**
A: An 8 KB block with a 24-byte header (LSN, checksum, flags, `pd_lower`, `pd_upper`,
`pd_special`), then an array of 4-byte line pointers growing downward from the header, free
space in the middle, and tuples growing upward from the end. Indexes point at (block, line
pointer), so tuples can be moved within the page during compaction without touching indexes.

**Q: What is a TID and why is it indirect?**
A: A tuple identifier is (block number, line pointer index). It is what index entries store. The
line pointer holds the actual byte offset, so compaction can move the tuple and only update the
line pointer, keeping all index entries valid.

**Q: What is TOAST?**
A: The mechanism for column values too big for one page. When a tuple exceeds about 2 KB, large
variable-length columns are compressed and, if still too big, stored out of line in a TOAST
table in roughly 2 KB chunks, with a pointer left in the main tuple. Reading a toasted column
means extra index lookups into the TOAST table, so avoid selecting large columns you do not need.

**Q: What does the visibility map do?**
A: Two bits per heap page: all-visible and all-frozen. VACUUM skips all-visible pages, which
makes vacuuming large static tables cheap, and index-only scans can skip the heap fetch for
pages marked all-visible. Any modification clears the bits.

**Q: How does the buffer pool decide what to evict?**
A: Clock sweep: a hand walks the buffer array, decrementing each buffer's usage count (0 to 5),
and takes the first unpinned buffer with count 0. Dirty victims are written out first, after
making sure WAL up to the page's LSN is flushed. The background writer keeps clean buffers
available so backends rarely wait on that write.

**Q: Is data on disk after COMMIT returns?**
A: The WAL record is, because COMMIT fdatasyncs WAL with `synchronous_commit = on`. The data
pages are not; they stay dirty in shared buffers and reach disk on eviction or at the next
checkpoint. Crash recovery replays WAL from the last checkpoint to rebuild them.

**Q: Why does column order in CREATE TABLE affect table size?**
A: Each type has an alignment requirement (int8 needs 8 bytes, int4 needs 4, bool 1) and the
engine inserts padding exactly as a C compiler does for structs. Declaring 8-byte columns first,
then 4, then 2, then 1-byte, then variable-length minimises padding.

## 8. Recap

- Tables are files of 8 KB slotted pages; rows never move between pages without a new TID.
- Page = 24-byte header, line pointer array growing down, tuples growing up, optional special space.
- Tuple = 24-byte header (xmin, xmax, ctid, infomask) + null bitmap + aligned columns.
- Alignment padding is real; order columns by descending alignment.
- TOAST compresses and moves big values out of line; `SELECT *` pays for them.
- FSM finds pages with room; VM lets VACUUM and index-only scans skip pages.
- Seq scan reads pages in order, uses a ring buffer for big tables, relies on OS read-ahead.
- Row store (PostgreSQL) for OLTP; column store for analytics.
- Buffer pool: pin, content lock, clock-sweep eviction, background writer, double caching with the OS.
- `pd_lsn` ties pages to WAL: no dirty page hits disk before its WAL does. fsync failure = PANIC.
