# minidb specification

**Status:** the contract for the optional second capstone. `01_build_plan.md` builds it in eight
steps and gives the key classes in full. If the plan and this document disagree, this document
wins and the plan has a bug.

**Prerequisites:** tcpkv tagged v1.0.0 (README of this course, section 1). 07_Databases_Internals
modules `02_storage_engines_pages_and_heap_files.md`, `03_btree_indexes_in_depth.md` and
`04_wal_and_crash_recovery.md` read at least once; the build plan points back to them.

## 1. In plain words

minidb is a tiny storage engine: a library that stores key-value pairs in one file on disk,
finds them quickly through a B+tree, and never loses a write it has acknowledged, even if the
process is killed in the middle of writing.

Think of a bank ledger kept in a bound book (the data file) plus a carbon-copy receipt pad
(the write-ahead log). Before the clerk changes a page in the book, the full new version of
that page goes on the receipt pad, and the pad is put in the safe. If the clerk faints halfway
through copying into the book, the next clerk reads the receipts in the safe and redoes the
copying. Receipts that were never put in the safe are thrown away. Every so often the book is
brought fully up to date and the old receipts are shredded (a checkpoint).

You already know the outside of this from your Berkeley DB to YugabyteDB migration: what a
storage engine promises. minidb is the inside: how it keeps the promise. Every interview
question about pages, B-trees, WAL, LSNs, checkpoints and recovery can then be answered from
code you wrote.

## 2. Goals and non-goals

Goals:

* **Durable:** when `put` or `del` returns, the change survives a process crash, and survives a
  power failure as long as the disk honours `fdatasync`.
* **Atomic per operation:** after any crash, each operation is either fully applied or not at
  all, including operations that split B+tree pages across several pages.
* **Correct and checkable:** a structural checker validates the whole tree; a crash test kills
  the process mid-write 25 times per run and verifies the result every time.
* **Small:** about 1,900 lines of library code (headers included) plus about 600 lines of
  tests in the reference version. Every line
  explainable.

Non-goals (and what to say when asked "why not"):

* **Concurrency beyond one mutex.** One operation at a time. Latch crabbing and MVCC are the
  next steps; see section 11.
* **Multi-operation transactions.** Each `put`/`del` is its own transaction. The commit
  protocol already supports many pages per transaction, so a `WriteBatch` is a small extension.
* **Space reclamation.** Deletes never merge pages or free them. The file never shrinks.
* **Performance.** Every write is one or more full-page log records and an `fdatasync`. That is
  correct and slow; group commit and delta records are the known fixes (section 11).
* **SQL, secondary indexes, compression, encryption, replication.**

## 3. API

### 3.1 C++

```cpp
namespace minidb {

struct Options {
    std::size_t cache_pages = 256;                  // page cache size in 4 KiB frames (1 MiB)
    std::uint64_t checkpoint_wal_bytes = 8u << 20;  // checkpoint when the log exceeds 8 MiB
};

class Db {
public:
    explicit Db(const std::string& path, Options options = {});  // opens or creates, runs recovery
    ~Db();                                                         // checkpoints

    void put(std::string_view key, std::string_view value);       // insert or overwrite; durable on return
    std::optional<std::string> get(std::string_view key);
    bool del(std::string_view key);                                // true if the key existed; durable on return
    std::vector<std::pair<std::string, std::string>>
        scan(std::string_view start, std::string_view end = {}, std::size_t limit = SIZE_MAX);
    void checkpoint();
    std::size_t check();                                           // full structural check, returns key count
};

}  // namespace minidb
```

| operation | semantics |
|-----------|-----------|
| `put(k, v)` | insert or overwrite. Durable when it returns. Throws `std::invalid_argument` for an empty key, a key over 128 bytes or a value over 880 bytes. |
| `get(k)` | the value, or `std::nullopt` |
| `del(k)` | removes the key; returns whether it existed. Durable when it returns. |
| `scan(start, end, limit)` | keys in `[start, end)` in ascending byte order, at most `limit`; an empty `end` means no upper bound |
| `checkpoint()` | writes all dirty pages, syncs the data file, empties the log |
| `check()` | walks the whole tree, verifies every invariant (section 4.4), returns the key count; throws `CorruptionError` on a violation |

Keys compare bytewise (`memcmp` order). Errors: `CorruptionError` (bad checksum, bad magic,
impossible structure), `DbError` (cache exhausted, previous commit failed),
`std::system_error` (I/O failure, with `errno`).

### 3.2 Command-line tool (`minidb_cli`)

```
minidb_cli <file> put <key> <value>
minidb_cli <file> get <key>
minidb_cli <file> del <key>
minidb_cli <file> scan [start] [end] [limit]
minidb_cli <file> check            # prints the key count, or the first violation
minidb_cli <file> stats            # page count, log size, last recovery statistics
```

### 3.3 Limits

| limit | value | reason |
|-------|-------|--------|
| page size | 4096 bytes | the usual OS page and filesystem block size; one page is one I/O |
| key | 1..128 bytes | with the value limit, a leaf cell is at most a quarter of a page, which guarantees a split always yields two halves that fit |
| value | 0..880 bytes | same; larger values would need overflow pages (section 11) |
| pages | 2^32 - 1 | `PageId` is 32 bits: 16 TiB of 4 KiB pages |

## 4. On-disk format

Two files: the **data file** `<path>` and the **log** `<path>-wal`. Both use host byte order
(little-endian on x86-64 and ARM64 Linux), written and read with `memcpy`, never pointer casts.

### 4.1 Data file

Page N occupies bytes `[N * 4096, (N + 1) * 4096)`. Every page starts with a 16-byte header:

| offset | size | field | notes |
|-------:|-----:|-------|-------|
| 0 | 4 | checksum | CRC-32 of bytes `[4, 4096)`. A page of all zeros fails it. |
| 4 | 2 | page type | 1 meta, 2 leaf, 3 internal |
| 6 | 2 | reserved | zero |
| 8 | 8 | page LSN | LSN of the log record that carried this image; 0 = never logged |

**Page 0, meta:** payload `u64 magic | u32 version (1) | u32 root page id | u32 page_count`.
Logged and recovered like any other page.

**Leaf page:** payload `u16 count | u32 next_leaf | count x (u16 key_len | u16 value_len | key | value)`,
cells sorted by key, keys unique. `next_leaf` = 0 for the last leaf.

**Internal page:** payload `u16 count | u32 child0 | count x (u16 key_len | key | u32 child)`.
`key_i` is a lower bound (inclusive) for every key under `child_i` and an upper bound
(exclusive) for every key under `child_{i-1}`.

Page 0 is the meta page, so page id 0 never names a tree node; it doubles as "no page".

### 4.2 Log file

```
header (24 bytes): u64 magic | u64 start_lsn | u32 crc32(first 16 bytes) | u32 zero
record (24-byte header + payload), repeated:
    u32 crc32(header bytes [4, 24) + payload)
    u32 payload_len           4096 for PageImage, 8 for Commit
    u64 lsn                   must equal the record's position (below)
    u8  type                  1 PageImage, 2 Commit
    u8[3] zero
    u32 page_id               PageImage only
    payload                   PageImage: the full sealed page; Commit: u64 transaction id
```

**LSN (log sequence number)** = `start_lsn + (file offset - 24)`: a byte position in a logical
log that never restarts. A checkpoint replaces the file with an empty one whose `start_lsn` is
the old end, so LSNs grow for the life of the database. LSN 0 is reserved for "never logged"
(pages written by database creation); a new log starts at LSN 1.

A record is valid if it is complete, its CRC matches, its type and length agree, and its `lsn`
field equals its position. Replay stops at the first invalid record: that is the torn tail of
a crash, or bytes left over from before a reset.

### 4.3 Atomic file creation and replacement

Database creation and log reset never write a header in place. They write `<name>.tmp`,
`fdatasync` it, `rename` it over the real name, and `fsync` the directory. `rename` is atomic
on POSIX filesystems, so a crash leaves the old file or the new one, never a mixture. The
directory `fsync` makes the rename itself durable.

### 4.4 Tree invariants (checked by `check()`)

1. Every leaf is at the same depth.
2. Keys within a leaf are strictly increasing.
3. Every key under `child_i` of an internal node lies in `[key_i, key_{i+1})` (open-ended at
   the ends), recursively.
4. Internal nodes have exactly `count + 1` children, all page ids below `page_count`.
5. Following `next_leaf` from the leftmost leaf visits exactly the leaves of an in-order walk,
   in the same order, and ends with 0.

## 5. Architecture

```
   Db  (public API, one mutex, transaction boundaries, commit protocol, checkpoint)
    |
    +--> BPlusTree   get / put / erase / scan / check     (decode page -> modify -> encode)
    |       |
    |       v
    +--> PageCache   fixed frames, pin counts (RAII PageRef), LRU, dirty bits,
    |       |        no-steal for the open transaction, before-images for abort,
    |       |        WAL rule hook before every write-back
    |       v
    +--> Pager       page N at offset N*4096, checksum on write, verify on read
    |       |
    |       v
    |    data file
    |
    +--> Wal         append page images + commit records, fdatasync, replay, reset
    |       |
    |       v
    |    log file
    |
    +--> Recovery    on open: replay log, redo committed page images, cut uncommitted tail
```

| component | file | responsibility |
|-----------|------|----------------|
| `File` | `file.hpp` | RAII descriptor; `pread`/`pwrite` loops with EINTR; `fdatasync`; atomic rename; directory fsync |
| `crc32` | `crc32.hpp` | CRC-32 for pages and log records |
| `Page`, `Meta` | `page.hpp` | the 4 KiB page, its header, checksum seal/verify; meta page encode/decode |
| `Pager` | `pager.hpp` | raw page I/O on the data file; atomic database creation |
| `PageCache` | `page_cache.hpp` | buffer pool: pinning, LRU replacement, dirty tracking, no-steal, abort |
| `BPlusTree` | `btree.hpp` | search, insert with leaf and internal splits, erase, range scan, invariant check |
| `Wal` | `wal.hpp` | log format, append, sync, replay, truncate, reset |
| `Recovery` | `recovery.hpp` | redo-only recovery |
| `Db` | `db.hpp` | wiring, locking, commit protocol, checkpoint policy |

## 6. Write path, read path, checkpoint

### 6.1 One `put`, step by step

```
Db::put(k, v)
  lock mutex
  BPlusTree::put: descend from the root, decode the leaf, insert, encode.
     If the leaf overflows: allocate a page (meta page_count++), split by bytes,
     push the separator up; internal nodes may split the same way; the root may split.
     Every page touched for writing goes through PageRef::write(), which:
        - saves the page's before-image in memory (first write in this transaction only)
        - marks the frame dirty and "in transaction" (no-steal: not evictable)
  commit:
     for every page in the transaction: Wal::append_page (stamps page LSN, seals, buffers)
     Wal::append_commit(txn id)
     Wal::sync()                      <-- fdatasync: the operation is durable from here
     PageCache::commit_txn()          pages become ordinary dirty pages (evictable)
  if the log is larger than checkpoint_wal_bytes: checkpoint
  unlock, return                      <-- the acknowledgement
```

If anything throws before `Wal::sync()` returns, `PageCache::abort_txn()` restores every
before-image, drops pages created in the transaction, and `Wal::discard_buffer()` forgets the
buffered records. Memory again matches the last committed state. If `fdatasync` itself fails,
the database refuses further operations until it is reopened (the kernel may have dropped
the dirty log pages; PostgreSQL's 2018 "fsyncgate" is the reference story), and recovery on
the next open decides what survived.

### 6.2 Policies, named

* **WAL rule (write-ahead):** a page is written to the data file only after the log record
  carrying that page image is durable. Enforced in `PageCache::write_back`, which calls
  `Wal::ensure_durable(page.lsn())` first.
* **No-steal:** pages changed by an uncommitted transaction never reach the data file. So the
  data file never needs undo, and the log needs no undo records.
* **No-force:** commit does not write data pages; it only syncs the log. Data pages are
  written later, at eviction or checkpoint. Commits cost one sequential log write, not random
  page writes.
* **Full-page images:** the log carries whole pages, not deltas. Redo is then "copy the
  bytes", which is idempotent, and a page torn by a crash mid-write is repaired by its image.
  PostgreSQL does the same for the first change to a page after each checkpoint
  (`full_page_writes`); minidb does it for every change, which is simpler and costs write
  volume.

Compared with ARIES (the algorithm behind most commercial engines): ARIES is steal/no-force,
so it needs undo as well as redo, compensation log records, and an analysis pass. minidb's
no-steal policy removes undo entirely, at the price that a transaction's dirty pages must fit
in the cache. For one-operation transactions that is a few pages.

### 6.3 Read path

`get` and `scan` take the mutex, descend through the cache (pin, read, decode, unpin) and
never touch the log. A cache miss reads the page and verifies its checksum; a mismatch throws
`CorruptionError` rather than returning wrong data.

### 6.4 Checkpoint

```
1. PageCache::flush_all()   write every dirty page (WAL rule: their records are durable)
2. Pager::sync()            fdatasync the data file
3. Wal::reset()             replace the log with an empty one starting at the current end LSN
```

A crash before step 3 completes leaves the old log; replaying it over the newer data file is
harmless (section 7). Checkpoints run when the log passes `checkpoint_wal_bytes`, on open
after recovery, and in the destructor, so a clean shutdown leaves an empty log.

## 7. Recovery

Runs in the `Db` constructor, before the cache reads any page.

```
pending = []
for each valid record in the log, in LSN order:
    PageImage -> append (page_id, image) to pending
    Commit    -> for each (page_id, image) in pending:
                     if the data file page is valid and its LSN >= image LSN: skip
                     else write the image to the data file
                 pending = []
                 end = position just after this commit record
discarded = size of pending               (a transaction that never committed)
fdatasync the data file
truncate the log at `end`                 (drop the uncommitted tail and any torn bytes)
then the Db constructor checkpoints
```

Why it is correct:

* **Nothing committed is lost.** A commit was acknowledged only after its records were
  `fdatasync`ed, so they are in the log. Recovery applies every committed image.
* **Nothing uncommitted appears.** No-steal means the data file never contained uncommitted
  bytes; recovery never applies images without a commit record.
* **Torn pages are repaired.** Any page written to the data file since the last checkpoint
  has its latest committed image in the log. A torn page fails its checksum, so the LSN check
  cannot skip it, and the image overwrites it.
* **Idempotent.** Writing a full image twice gives the same bytes. A crash during recovery is
  handled by running recovery again.
* **The LSN skip is only an optimisation**, and it needs LSN 0 to be reserved: a freshly
  created page has LSN 0, so if the first record also had LSN 0 the check `0 >= 0` would skip
  a redo that is needed. (This bug existed in the first draft of the reference code and was
  caught by `Recovery.UncommittedTailIsDiscardedAndNotReplayedLater`; tell that story.)
* **Truncating at the last commit is required**, not cosmetic. Without it, the orphaned page
  images of a crashed transaction would sit in front of the next transaction's records, and a
  later recovery would apply them as part of that next commit.

## 8. The crash test

`tests/crash_test.cpp`, run by `ctest` on every CI job.

1. `fork()` a child. The child opens the database with a 16-page cache and a 128 KiB checkpoint
   threshold (so kills land during evictions and checkpoints too) and runs an infinite,
   deterministic sequence of operations: operation `i` is a function of `i` only (mostly puts
   of varying sizes over 400 keys, every eleventh a delete). After each operation returns, the
   child writes `i + 1` to a pipe. That write is the acknowledgement.
2. The parent sleeps a random 20 to 200 ms, sends `SIGKILL` (cannot be caught: no destructor,
   no checkpoint, no flush runs), reaps the child, and drains the pipe to find `acked`, the
   last acknowledged count.
3. The parent opens the database (recovery runs), calls `check()`, and compares the full
   contents with a `std::map` model after exactly `acked` operations, or after `acked + 1`:
   the operation in flight may have committed just before the kill, without its
   acknowledgement reaching the pipe.
4. 25 rounds, each on a fresh database. The test also fails if nothing was ever acknowledged.

What "acknowledged" means precisely: every operation whose acknowledgement the parent read
must be present; no operation after the in-flight one may be visible; the in-flight one is
all or nothing.

What it does **not** simulate: power loss. After `SIGKILL` the kernel still writes the page
cache to disk, so an engine that forgot to `fdatasync` would pass. Testing that needs a layer
that drops unsynced writes: a VM snapshot taken at a random moment, `dm-flakey` or `dm-log-writes`
in Linux, or a FUSE filesystem like LazyFS. Name these if asked; they are the honest next step.

Unit-level crash tests complement it: a torn data page repaired from the log, a torn log tail
ignored, an uncommitted tail discarded and not replayed later, a corrupted byte detected by the
checksum.

The reference crash test was checked against a mutant: with redo disabled in `Recovery`, it
fails on the first kill. A crash test that cannot fail proves nothing; try the same mutation on
your version.

## 9. Non-functional requirements and testing

| requirement | how it is met | how it is checked |
|-------------|---------------|-------------------|
| `-Wall -Wextra -Wpedantic -Wshadow -Werror` clean | the tcpkv warnings target, reused | every CI job |
| no memory errors, no UB | RAII everywhere, `memcpy` for fixed fields, bounds-checked node decoding | ASan + UBSan + LSan job runs every test, the crash test included |
| durable acknowledged writes | WAL rule, commit protocol, atomic rename, directory fsync | crash test (process crash); power-loss testing is listed as future work |
| detects corruption, never returns wrong data | CRC-32 on every page and log record | `Pager.DetectsCorruptedPage`, `Wal.TornTailIsIgnored` |
| B+tree correct | invariant checker; model test against `std::map` | `Db.MatchesStdMapUnderRandomOperations`, `Db.ManySplitsKeepTheTreeValidAndScansOrdered` |
| no data races | one mutex around the whole `Db` | TSan job (cheap insurance; the design has one lock) |

Test files: `storage_test.cpp` (CRC, Pager, PageCache, Wal: 9 tests), `btree_test.cpp` (the
tree on a cache with no log, model test against `std::map`: 4 tests), `db_test.cpp` (tree
through the API, model test, recovery: 7 tests), `crash_test.cpp` (1 test, 25 kills). The
reference versions pass under a plain `-O2` build and under ASan+UBSan on GCC 13 in WSL.

Expect the fsync-bound tests to be slow: every `put` waits for `fdatasync`. On the WSL2
machine used to check the reference code, the roughly 15,000 synced writes in `db_test` took about 40
seconds. Measure your own machine before quoting a number.

## 10. Repository layout

```
minidb/
  CMakeLists.txt            library + minidb_cli + tests; warnings target; sanitizer option (as tcpkv)
  CMakePresets.json         debug, release, asan, tsan, ubsan (copy from tcpkv)
  .github/workflows/ci.yml  build-and-test matrix over the presets
  .clang-format .gitignore LICENSE (MIT) README.md
  include/minidb/           common.hpp crc32.hpp file.hpp page.hpp pager.hpp page_cache.hpp
                            wal.hpp recovery.hpp btree.hpp db.hpp
  src/                      crc32.cpp file.cpp pager.cpp page_cache.cpp wal.cpp recovery.cpp
                            btree.cpp db.cpp
  apps/                     minidb_cli.cpp
  tools/                    bench.cpp (puts/gets per second, with and without sync)
  tests/                    test_util.hpp storage_test.cpp btree_test.cpp db_test.cpp crash_test.cpp
```

README outline: what it is, the durability contract in one paragraph, the architecture
diagram, the on-disk format tables, the commit protocol and recovery in ten lines each, how to
build and run the crash test, benchmark numbers with the machine, limitations, what next.

## 11. Known limitations and what next

| limitation | the fix you can describe |
|------------|---------------------------|
| one fsync per operation | **group commit**: let concurrent committers share one `fdatasync` (a leader syncs for everyone queued behind it) |
| full-page log records | log **deltas** (physiological records: "insert cell at slot 3 of page 17"), with a full-page image only on the first change after a checkpoint, as PostgreSQL does |
| one mutex | **latch crabbing**: per-page reader/writer latches taken top-down, releasing a parent once the child is known not to split; or copy-on-write B-tree pages with a single writer (LMDB) |
| no multi-op transactions | the commit protocol already supports many pages; add `WriteBatch` and keep no-steal (or switch to steal and add undo, i.e. ARIES) |
| deletes never merge | merge or redistribute when a node falls below a fill threshold; a free list for empty pages in the meta page |
| values limited to 880 bytes | overflow pages chained from the cell |
| full log replay on open | bounded by the checkpoint threshold; fuzzy checkpoints let writes continue during a checkpoint |
| no power-loss test | `dm-log-writes` or LazyFS replay of every prefix of the write stream |
