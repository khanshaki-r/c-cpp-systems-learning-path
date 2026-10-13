# 10. Berkeley DB and embedded key-value stores

**Time:** ~3 hours   **Prerequisites:** 02, 03, 04

## 1. In plain words

An embedded key-value store is a database with no server: a library you link into your program
that stores `key -> value` pairs in a file and gives you `put`, `get`, `delete` and an ordered
cursor. Berkeley DB (1991, Sleepycat, later Oracle) is the classic. SQLite is the embedded
*relational* store. LevelDB (Google, 2011) and RocksDB (Facebook's fork, 2012) are the modern
embedded KV stores built on a different structure, the LSM tree, and RocksDB is what sits under
YugabyteDB, CockroachDB, TiKV, MyRocks and Kafka Streams.

The analogy: Berkeley DB is a filing cabinet in your own office. Nobody else needs to be hired
(no server process), you can reach it instantly (function calls), but you must lock the drawers
yourself, keep the ledger yourself, and if you knock it over, you clean it up. PostgreSQL is a
records department down the hall with staff.

You spent two years inside Berkeley DB from the application side. This module gives you the
vocabulary to describe its architecture precisely, a complete C program you can write from
memory, and the comparison with LSM-based stores that explains why YugabyteDB chose RocksDB and
why your migration target still had a key-value store at the bottom.

## 2. The real thing

### 2.1 Berkeley DB architecture

```
  your process                       other process (same DB_ENV)
  +-------------------------------+  +----------------------+
  | application code              |  | application code     |
  |  DB handles, DBC cursors      |  |  DB handles          |
  |  DB_TXN transactions          |  |                      |
  +-------------------------------+  +----------------------+
  | access methods: BTREE HASH    |  | access methods       |
  |                 RECNO QUEUE   |  |                      |
  +-------------------------------+--+----------------------+
  | shared memory region (DB_ENV home: __db.001, __db.002 ...)                   |
  |   mpool   buffer cache of pages, pinned, dirty, LRU-ish                      |
  |   lock    lock table: lockers, objects (page or record), modes, waiters      |
  |   log     in-memory log buffer -> log.0000000001 files on disk               |
  |   txn     transaction table: active txns, their LSNs                          |
  |   mutex   region of shared mutexes                                            |
  +------------------------------------------------------------------------------+
  | files: mydb.db (one or more databases per file), log.*, __db.* regions      |
  +------------------------------------------------------------------------------+
```

The four **subsystems** map one-to-one onto PostgreSQL layers from module 01: mpool = buffer
manager, lock = lock manager, log = WAL, txn = transaction manager. They are enabled with flags on
`DB_ENV->open`: `DB_INIT_MPOOL`, `DB_INIT_LOCK`, `DB_INIT_LOG`, `DB_INIT_TXN`. Without `DB_INIT_TXN`
you have a fast, non-durable, non-isolated store (the "Data Store" product); with all four you
have the "Transactional Data Store" (TDS), which is what a banking server needed. All processes
that open the same environment home directory share the regions, so the environment, not the
process, is the unit of consistency.

**Access methods** (chosen per database at `DB->open`):

| Method | Key | Structure | Use |
|---|---|---|---|
| `DB_BTREE` | arbitrary bytes, ordered by a comparison function (default lexicographic) | B+tree with slotted pages, prefix compression on internal pages | the default: range scans, ordered cursors, duplicates (`DB_DUP`, `DB_DUPSORT`) |
| `DB_HASH` | arbitrary bytes | extendible linear hashing | pure point lookups on large tables where a B-tree would be deep; no ordering |
| `DB_RECNO` | logical record number (1, 2, 3...) | B-tree keyed by record number, optionally over a flat text file | line-numbered data, renumbering on delete |
| `DB_QUEUE` | record number | fixed-length records in extent files, record-level locking | high-concurrency FIFO queues; `DB_CONSUME` pops atomically |

The "B-tree cursor traversal" on your resume: a `DB_BTREE` database with a packed business
key, walked with `DB_SET_RANGE` to position and `DB_NEXT` to advance, exactly as the code in
section 3 does.

**Handles**: `DB_ENV` (the environment), `DB` (one database = one key space), `DBC` (a cursor
over one DB), `DB_TXN` (a transaction spanning any number of DBs in the environment). Every
operation takes an optional `DB_TXN *`; `NULL` means autocommit when `DB_AUTO_COMMIT` was set, or
non-transactional otherwise.

**DBT** (data-base thunk) is the struct for keys and values:

```c
typedef struct {
    void    *data;     /* pointer to the bytes */
    u_int32_t size;    /* bytes used */
    u_int32_t ulen;    /* bytes available in a user-supplied buffer (DB_DBT_USERMEM) */
    u_int32_t dlen, doff;  /* partial reads/writes (DB_DBT_PARTIAL) */
    u_int32_t flags;   /* DB_DBT_MALLOC (library allocates, you free), DB_DBT_REALLOC, DB_DBT_USERMEM */
} DBT;
```

Values are opaque bytes: your packed C structs. Berkeley DB never looks inside them, except
through callbacks you supply (comparison functions, secondary key extractors). That is the
"record-to-relational mapping" problem in one sentence: the schema lived in your `#pragma pack`
headers (module 12).

**Secondary indexes**: `primary->associate(primary, txn, secondary, callback, flags)`. The callback
receives the primary key and value and fills a DBT with the secondary key (or returns
`DB_DONOTINDEX`). Berkeley DB then maintains the secondary database on every put/delete; a
`secondary->pget()` or a cursor on it returns the primary key and value. This is `CREATE INDEX`
done by hand, with you writing the key extractor in C.

**Duplicates**: `DB->set_flags(db, DB_DUP)` allows multiple values per key in insertion order;
`DB_DUPSORT` sorts them. Cursors walk duplicates with `DB_NEXT_DUP` / `DB_NEXT_NODUP`;
`DB_GET_BOTH` finds a specific key/value pair. Secondaries nearly always need `DB_DUPSORT`.

### 2.2 Transactions, locking and logging in Berkeley DB

- **Transactions**: `env->txn_begin(env, parent, &txn, flags)`, `txn->commit(txn, flags)`,
  `txn->abort(txn)`. Nested transactions via `parent`. `DB_TXN_NOSYNC` / `DB_TXN_WRITE_NOSYNC`
  trade durability for speed per transaction or environment-wide; `DB_TXN_SNAPSHOT` enables MVCC
  reads (available since 4.5, off by default).
- **Locking**: **page-level** for BTREE/HASH/RECNO, record-level for QUEUE. A reader locks a
  page in read mode and a writer in write mode, held until **transaction end** for transactional
  handles (two-phase locking, 2PL). Readers block writers and writers block readers on the same
  page, unless you use `DB_TXN_SNAPSHOT` (MVCC via copy-on-write page versions in mpool) or
  `DB_READ_UNCOMMITTED`. Deadlocks are frequent under concurrent writers to nearby keys because
  two transactions touching different records on one page conflict.
- **Deadlock handling**: there is no automatic detector thread unless you configure one:
  `env->set_lk_detect(env, DB_LOCK_DEFAULT)` makes the lock manager run detection on every
  blocked lock request, or you run the `db_deadlock` utility / `env->lock_detect()` periodically.
  A chosen victim's operation returns `DB_LOCK_DEADLOCK`; the application must `abort()` the
  transaction and retry the whole unit of work. Every transactional Berkeley DB program is a
  retry loop. (You will recognise the shape from module 05's 40001 handling.)
- **Logging**: write-ahead, physical undo/redo records with before and after images (module
  04); `log.NNNNNNNNNN` files of `set_lg_max` bytes (default 10 MB); `env->txn_checkpoint()` must
  be called by you (typically a thread every minute or N KB of log); `DB_RECOVER` at open runs
  normal recovery, `DB_RECOVER_FATAL` catastrophic recovery from archived logs; `db_archive`
  lists logs that can be removed, or `DB_LOG_AUTO_REMOVE` deletes them (and kills your ability to
  do catastrophic recovery). `DB_REGISTER` lets processes detect that another process died
  holding the environment and run recovery safely.

Operational truths from the field, phrased for an interview: with Berkeley DB the application
owns checkpointing, log archiving, deadlock detection, recovery invocation, and the record
schema. Each is a place where a mistake corrupts or loses data without anyone noticing. Moving to
a server database transfers all five to the database.

### 2.3 The C API in one program

`code/m10_bdb_cursor_txn.c` is complete and compiles on Ubuntu 24.04 with
`sudo apt install libdb-dev` and `gcc -Wall -Wextra m10_bdb_cursor_txn.c -o bdbdemo -ldb`
(libdb 5.3). It opens an environment with all four subsystems, creates a BTREE primary keyed
by a packed `(branch, user_code)` struct, a DUPSORT secondary keyed by branch, inserts records in
a transaction, traverses with `DB_SET_RANGE`/`DB_NEXT` under a second transaction, updates
through the cursor, demonstrates deadlock-retry structure, checkpoints, and closes cleanly.
The essential parts:

```c
#include <db.h>
#define CHECK(ret, what) do { if ((ret) != 0) { fprintf(stderr, "%s: %s\n", what, db_strerror(ret)); goto fail; } } while (0)

#pragma pack(push, 1)
struct user_key  { char branch[6]; char user_code[9]; };            /* 15 bytes, big-endian-ish text sorts lexicographically */
struct user_rec  { uint32_t id; uint8_t status; uint16_t failed_attempts; int64_t last_login_epoch; char name[32]; };
#pragma pack(pop)

int main(void) {
    DB_ENV *env = NULL; DB *dbp = NULL, *sdbp = NULL; DBC *cur = NULL; DB_TXN *txn = NULL; int ret;

    CHECK(db_env_create(&env, 0), "db_env_create");
    env->set_errpfx(env, "bdbdemo");
    CHECK(env->set_lk_detect(env, DB_LOCK_DEFAULT), "set_lk_detect");       /* detect deadlocks on each blocked request */
    CHECK(env->open(env, "./bdb_env",
                    DB_CREATE | DB_INIT_MPOOL | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_RECOVER | DB_THREAD,
                    0), "env->open");                                          /* DB_RECOVER: always run recovery at startup */

    CHECK(db_create(&dbp, env, 0), "db_create");
    CHECK(dbp->open(dbp, NULL, "users.db", "users", DB_BTREE, DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644), "db->open");

    CHECK(db_create(&sdbp, env, 0), "db_create secondary");
    CHECK(sdbp->set_flags(sdbp, DB_DUPSORT), "set_flags DUPSORT");           /* many users per branch */
    CHECK(sdbp->open(sdbp, NULL, "users.db", "users_by_branch", DB_BTREE, DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644), "sdb->open");
    CHECK(dbp->associate(dbp, NULL, sdbp, branch_key_extractor, DB_AUTO_COMMIT), "associate");

    /* ---- insert in one transaction ---- */
    CHECK(env->txn_begin(env, NULL, &txn, 0), "txn_begin");
    for (int i = 0; i < N; ++i) {
        struct user_key k; struct user_rec r; fill(&k, &r, i);
        DBT key = {0}, val = {0};
        key.data = &k; key.size = sizeof k; val.data = &r; val.size = sizeof r;
        ret = dbp->put(dbp, txn, &key, &val, DB_NOOVERWRITE);                /* NOOVERWRITE: DB_KEYEXIST if present */
        if (ret == DB_KEYEXIST) continue;
        CHECK(ret, "put");
    }
    CHECK(txn->commit(txn, 0), "commit"); txn = NULL;                         /* 0 = fsync the log: durable */

    /* ---- cursor traversal: all users of branch BR0007, in key order ---- */
    CHECK(env->txn_begin(env, NULL, &txn, 0), "txn_begin");
    CHECK(dbp->cursor(dbp, txn, &cur, 0), "cursor");
    struct user_key start = {0}; memcpy(start.branch, "BR0007", 6);
    DBT key = {0}, val = {0};
    key.data = &start; key.size = sizeof start; key.flags = DB_DBT_USERMEM; key.ulen = sizeof start;
    struct user_rec buf; val.data = &buf; val.ulen = sizeof buf; val.flags = DB_DBT_USERMEM;  /* no malloc per record */
    for (ret = cur->get(cur, &key, &val, DB_SET_RANGE);                          /* position at first key >= start */
         ret == 0;
         ret = cur->get(cur, &key, &val, DB_NEXT)) {
        const struct user_key *k = key.data;
        if (memcmp(k->branch, "BR0007", 6) != 0) break;                          /* left the branch: stop */
        struct user_rec r; memcpy(&r, val.data, sizeof r);                       /* copy out: val.data may be unaligned */
        if (r.status == STATUS_LOCKED) {
            r.status = STATUS_ACTIVE; r.failed_attempts = 0;
            val.data = &r; val.size = sizeof r;
            CHECK(cur->put(cur, &key, &val, DB_CURRENT), "cursor put");          /* update the record under the cursor */
        }
    }
    if (ret != DB_NOTFOUND) CHECK(ret, "cursor get");
    CHECK(cur->close(cur), "cursor close"); cur = NULL;                          /* close cursors BEFORE commit */
    CHECK(txn->commit(txn, 0), "commit"); txn = NULL;

    CHECK(env->txn_checkpoint(env, 0, 0, 0), "checkpoint");                       /* your job in Berkeley DB */
    /* close order: cursors, secondary, primary, env */
fail:
    if (cur) cur->close(cur);
    if (txn) txn->abort(txn);
    if (sdbp) sdbp->close(sdbp, 0);
    if (dbp) dbp->close(dbp, 0);
    if (env) env->close(env, 0);
    return ret == 0 ? 0 : 1;
}
```

The deadlock retry wrapper the full file uses:

```c
/* Run a unit of work; on DB_LOCK_DEADLOCK abort and retry with backoff. Every TDS program needs this. */
static int run_txn(DB_ENV *env, int (*work)(DB_TXN *, void *), void *arg) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        DB_TXN *t = NULL; int ret = env->txn_begin(env, NULL, &t, 0);
        if (ret) return ret;
        ret = work(t, arg);
        if (ret == 0) return t->commit(t, 0);
        t->abort(t);
        if (ret != DB_LOCK_DEADLOCK) return ret;
        usleep(1000u << attempt);                 /* 1 ms, 2 ms, 4 ms ... */
    }
    return DB_LOCK_DEADLOCK;
}
```

### 2.4 LSM trees: the other way to build a KV store

A B-tree updates pages in place: every write is a random read-modify-write of a leaf (plus WAL).
A **log-structured merge tree** never modifies a file in place:

```
  write path                             files on disk (SSTables = sorted string tables, immutable)
  put(k,v) --> WAL append                 L0: [sst][sst][sst][sst]          small, may overlap in key range
           --> memtable (skiplist in RAM)  L1: [  sst  ][  sst  ][  sst  ]   10x bigger, non-overlapping
  memtable full --> flush to L0 SSTable   L2: [ ... 10x bigger again ... ]
                                          L3: ...
  background compaction: merge-sort overlapping SSTables from Ln into Ln+1, dropping overwritten
  and deleted (tombstoned) keys
```

- **Write**: append to WAL (sequential), insert into the memtable (RAM). No random I/O. When the
  memtable reaches ~64 MB it is frozen and written out as one sorted file. Writes are therefore
  sequential and batched, which is why LSM stores win on write-heavy workloads and on SSDs
  (fewer, larger writes also mean less flash wear).
- **Read**: check memtable, then each L0 file (they can overlap), then one file per deeper level
  (binary search on the level's key ranges). Each file has a **bloom filter** so most non-matching
  files are skipped without I/O, and a block index. A point read still touches more places than a
  B-tree's one leaf path; range scans must merge iterators from every level.
- **Compaction**: the background merge that keeps levels sorted and bounded. **Leveled**
  compaction (RocksDB default, LevelDB): each level is 10x the previous, non-overlapping; a key is
  rewritten about once per level: low space amplification (~1.1x), higher write amplification
  (~10 to 30x). **Tiered / size-tiered / universal**: merge files of similar size; lower write
  amplification, higher space amplification (duplicates across tiers) and more files to check per read.
- **Deletes** are tombstones that must propagate down to the bottom level before the space is
  freed; a table with many deletes reads slowly until compaction catches up (the "tombstone
  problem", also behind YugabyteDB's and Cassandra's advice about deleting in bulk).

**The three amplifications** (the RUM conjecture: you cannot minimise Read, Update and Memory
overhead at once):

| | B-tree (PostgreSQL, Berkeley DB) | LSM leveled (RocksDB) |
|---|---|---|
| Write amplification | one page rewrite per update (8 KB for a 100 B change) + WAL, + FPIs | ~10 to 30x via compaction, but all sequential |
| Read amplification | 1 path: height pages, mostly cached | memtable + L0 files + 1 per level, bloom filters reduce it |
| Space amplification | fragmentation 1.3 to 2x after churn (bloat) | ~1.1x leveled; higher tiered; plus temporary space during compaction |
| Latency profile | predictable | compaction stalls and write stalls when L0 fills (`level0_slowdown_writes_trigger`) |
| Best for | read-heavy OLTP, point and range reads, in-place updates | write-heavy, append-heavy, large datasets on SSD, compressible data |

RocksDB adds over LevelDB: column families (separate LSM trees sharing one WAL, like tables),
multi-threaded compaction and flush, universal compaction, prefix bloom filters, transactions
(optimistic and pessimistic), merge operators (read-modify-write without a read), rate limiting,
block cache + optional row cache, pluggable comparators and compression per level (LZ4 upper,
ZSTD bottom), and `DeleteRange`. It is a library like Berkeley DB: embedded, no server, your
process owns the files.

### 2.5 Where embedded KV stores are used, and why YugabyteDB is built on RocksDB

Embedded stores appear wherever a process needs durable ordered storage without the cost or
operational weight of a server: browsers (Chrome's IndexedDB on LevelDB), mobile apps (SQLite),
message brokers' state stores (Kafka Streams on RocksDB), caches with persistence, blockchain
nodes (LevelDB/RocksDB), your banking RPC server before the migration, and, crucially, **as the
per-node storage engine of distributed databases**.

A distributed SQL database has to build replication, sharding, distributed transactions and a
query layer. It does not want to also reinvent a crash-safe sorted storage engine with
compression and caching. So YugabyteDB, CockroachDB (until its own Pebble, an LSM rewrite in Go),
TiKV and MyRocks all put RocksDB at the bottom: one RocksDB instance per tablet (shard) per node.
RocksDB's properties fit: writes arrive as Raft log entries that are already sequential;
replication and recovery are handled above RocksDB by Raft, so RocksDB's own WAL is turned off
in YugabyteDB; the LSM's immutable SSTables make snapshots, backups and tablet moves cheap (copy
files); MVCC with hybrid timestamps is encoded directly in the key suffix; and the merge
operator and prefix bloom filters support the document-key encoding (module 11).

So the honest shape of your migration: from one process-local B-tree key-value store (Berkeley
DB) to a SQL layer over a replicated, sharded LSM key-value store (DocDB on RocksDB). The
key-value store did not go away; it moved down a layer and gained a schema, a planner, and
consensus replication above it.

## 3. Code

Build and run `code/m10_bdb_cursor_txn.c`:

```bash
sudo apt install -y libdb-dev
gcc -Wall -Wextra -O2 m10_bdb_cursor_txn.c -o bdbdemo -ldb
./bdbdemo
ls -la bdb_env/            # __db.001.. regions, log.0000000001, users.db
db_stat -h bdb_env -c      # lock statistics: see lockers, deadlocks, waits
db_stat -h bdb_env -l      # log statistics
db_stat -h bdb_env -m      # mpool: cache hits, pages created
db_dump -h bdb_env -p users.db | head   # keys and values as printable bytes
db_recover -h bdb_env -v   # run recovery by hand (what DB_RECOVER does)
```

The file also shows the **secondary key extractor** signature, which you should be able to write
unprompted:

```c
/* Called by the library for every put/delete on the primary: fill `result` with the secondary key. */
static int branch_key_extractor(DB *secondary, const DBT *pkey, const DBT *pdata, DBT *result) {
    (void)secondary; (void)pdata;
    const struct user_key *k = pkey->data;
    memset(result, 0, sizeof *result);
    result->data = (void *)k->branch;       /* points into pkey: valid for the duration of the call */
    result->size = sizeof k->branch;
    return 0;                               /* or DB_DONOTINDEX to skip this record */
}
```

A minimal RocksDB equivalent, for comparison only (C API, `librocksdb-dev`):

```c
rocksdb_options_t *o = rocksdb_options_create();
rocksdb_options_set_create_if_missing(o, 1);
rocksdb_t *db = rocksdb_open(o, "./rocks", &err);
rocksdb_put(db, wo, key, klen, val, vlen, &err);                 /* WAL append + memtable insert */
rocksdb_iterator_t *it = rocksdb_create_iterator(db, ro);
for (rocksdb_iter_seek(it, "BR0007", 6); rocksdb_iter_valid(it); rocksdb_iter_next(it)) { ... }  /* merges all levels */
```

Same shape as the Berkeley DB cursor loop. Under it, totally different physics.

## 4. Under the hood (senior-level)

**Berkeley DB page locking and why it deadlocks.** With default (non-snapshot) TDS, a `DB_NEXT`
cursor read takes a read lock on each leaf page it visits and holds it to transaction end
(`DB_READ_COMMITTED` on the cursor releases it when the cursor moves). A concurrent writer wanting
any record on that page blocks. Two transactions each reading page A then writing page B in
opposite orders deadlock; the detector picks a victim by policy (`DB_LOCK_DEFAULT` = youngest,
or `DB_LOCK_MINWRITE`, etc.). PostgreSQL's row-level MVCC never takes read locks, so the same
workload simply does not deadlock on reads. If your migration engine read the source with a
long-lived transaction while the server kept writing, you fought exactly this; the fix was
either `DB_TXN_SNAPSHOT` (MVCC reads via copy-on-write pages, at the cost of mpool size) or
short read transactions with cursor repositioning by last key (which is also the restartable
cursor pattern in module 12).

**Berkeley DB B-tree internals.** Page types: internal (`P_IBTREE`) holding (key prefix, child
pgno, record count), leaf (`P_LBTREE`) holding key and data items in slots, overflow pages for
items larger than a quarter page, duplicate pages (off-page duplicates become their own small
B-tree). Keys on internal pages are prefix-compressed by default (`bt_prefix`). The default
comparison is `memcmp`-like on the bytes: that is why your packed key struct had to be laid out
with the most significant field first and in big-endian or text form, or the cursor order would
not be the business order. `DB->set_bt_compare` installs a custom comparator when it is not.
Reverse-split (merging under-full pages) is on by default and can be disabled with
`DB_REVSPLITOFF` for append-heavy workloads.

**Memory pool and the OS.** The mpool region is a shared memory segment (System V or
file-backed mmap in the environment home) of `set_cachesize` bytes (default a few hundred KB,
far too small; production used hundreds of MB). Like PostgreSQL's `shared_buffers`, it double
buffers with the OS page cache. `DB_DIRECT_DB` and `DB_DIRECT_LOG` use O_DIRECT to avoid that.
Pages are checksummed only if `DB_CHKSUM` is set per database.

**Why LSM reads are not as bad as they look.** With leveled compaction and 10x fan-out, a 1 TB
store has about 7 levels. A point read checks the memtable, the few L0 files, and one file per
level, but each file's bloom filter (10 bits per key gives ~1% false positives) is in memory, so
only the one file that actually contains the key costs an I/O (plus its index block if not
cached). Range scans are worse: a merging iterator must open every level's candidate file and
heap-merge them; tombstones in the range are read and skipped. This is the reason YugabyteDB
documents that heavy deletes degrade scans until compaction.

**Write stalls.** When flushes and compactions cannot keep up (L0 file count exceeds
`level0_slowdown_writes_trigger`, or pending compaction bytes exceed a soft limit), RocksDB
throttles writers, then stops them. Latency spikes of hundreds of milliseconds during compaction
are the LSM's version of PostgreSQL's checkpoint I/O spikes. Both are tuned by spreading
background work: `max_background_jobs`, rate limiter, versus `checkpoint_completion_target`.

**MVCC in the key.** RocksDB itself has a sequence number per write; YugabyteDB appends a hybrid
timestamp to every key so that multiple versions of a document sub-key are adjacent in the
LSM in reverse time order, and a read at timestamp T seeks to the first version <= T. Garbage
collection of old versions happens during compaction, bounded by a history retention interval,
which is DocDB's VACUUM (module 11).

## 5. In production

- **Berkeley DB environments and process death.** A process that dies holding region mutexes
  leaves the environment unusable until recovery. `DB_REGISTER` with `DB_RECOVER` lets the next
  opener detect and recover automatically; without it, you need an operator to run `db_recover`.
  Build this into startup; do not rely on runbooks.
- **Cache size and log size are the two knobs.** Too small an mpool means every cursor step is a
  disk read; too small `set_lg_max` with no checkpoint thread means thousands of log files.
  Monitor `db_stat -m` hit rate and `db_stat -l` for log growth.
- **Schema drift in packed records.** Adding a field to the C struct without a version byte in
  the record makes old records unreadable. Every record format must carry a version, and the
  reader must handle every version ever written. Your migration decoder had to (module 12).
- **Choosing an embedded store today.** Need SQL and one process: SQLite. Need ordered KV, write
  heavy, large: RocksDB. Need a tiny dependency and moderate data: LMDB (B+tree, copy-on-write,
  mmap, read-optimised). Need a server: PostgreSQL. Berkeley DB for new systems is hard to
  justify (AGPL licence since 6.0, Oracle-owned, declining ecosystem), which is another honest
  bullet in the migration motivation.
- **RocksDB tuning pitfalls** you will meet through YugabyteDB: too few background threads
  (stalls), block cache too small (every read is an SSD read), bloom filters disabled on a
  column family (reads check every level), and bulk deletes without compaction (scan latency).
- **3 AM with Berkeley DB**: `DB_RUNRECOVERY: Fatal error, run database recovery`. Stop all
  processes, `db_recover -h env`, restart. If the logs were auto-removed and the database file is
  corrupt, you restore from the last `db_hotbackup` and replay nothing. This is why the migration
  was a risk-reduction project, not only a performance project.

## 6. Try it

1. **(easy)** Build and run `code/m10_bdb_cursor_txn.c`. Run `db_stat -h bdb_env -c` and
   `-m`. Done when you can point at lock requests, deadlocks (0), and cache hit counts.
2. **(easy)** Modify the program to insert the same key twice without `DB_NOOVERWRITE` and with
   it; observe the return codes. Then add `DB_DUPSORT` to the primary and insert two values for
   one key; walk them with `DB_NEXT_DUP`. Done when you can state what each flag changed.
3. **(medium)** Run two instances of the program concurrently that update overlapping keys in
   long transactions. Make them deadlock (`DB_LOCK_DEADLOCK`), then show the retry wrapper resolves
   it. Then open the environment with `DB_MULTIVERSION` and cursors with `DB_TXN_SNAPSHOT` and show
   reads no longer block. Done when you can explain page locking versus snapshot reads.
4. **(medium)** Kill the program with `kill -9` mid-transaction and restart it. Verify with
   `db_dump` that the partial transaction's records are absent, then remove `DB_RECOVER` from the
   open flags and kill again: observe the failure mode on restart. Done when you can describe what
   `DB_RECOVER` does and why it must always be passed.
5. **(hard)** Install `librocksdb-dev`, write a 60-line C program that inserts 5 M random keys
   into RocksDB and into a Berkeley DB BTREE with equal cache sizes, timing both, then do 100k
   random point reads on each. Done when you have the four numbers and can explain them with write
   and read amplification.

## 7. Interview questions

**Q: Describe Berkeley DB's architecture.**
A: An embedded library with four subsystems shared across processes through a shared-memory
environment: a memory pool for pages, a lock manager with page-level locking, a write-ahead log
with undo and redo images, and a transaction manager. Databases use one of four access methods,
B-tree being the usual one, and values are opaque bytes; secondary indexes are maintained through
application-supplied key extractor callbacks.

**Q: Walk me through a transactional cursor traversal in the C API.**
A: Open the environment with the four INIT flags plus DB_RECOVER, open the DB handle with
DB_AUTO_COMMIT, begin a transaction with txn_begin, open a cursor on the DB within that
transaction, position with DB_SET_RANGE on the packed start key, loop with DB_NEXT until the key
leaves the range or DB_NOTFOUND, update under the cursor with DB_CURRENT if needed, close the
cursor before committing, and wrap the whole unit in a retry loop on DB_LOCK_DEADLOCK.

**Q: What did Berkeley DB make the application responsible for that PostgreSQL does not?**
A: Checkpointing and log archiving, running recovery at startup, deadlock detection
configuration and retry, cache sizing, the record format and its versioning, and secondary index
maintenance logic. Each is a correctness risk owned by application code; moving to a server
database transferred them to the engine.

**Q: Explain an LSM tree and compare it with a B-tree.**
A: Writes go to a WAL and an in-memory memtable, which is flushed as an immutable sorted file;
background compaction merges files into larger non-overlapping levels. Writes are sequential and
batched, so write throughput and SSD wear are better than a B-tree's in-place page updates, at
the cost of read amplification across levels (mitigated by bloom filters), compaction CPU and
I/O, and latency spikes during stalls. B-trees give predictable reads and in-place updates with
fragmentation and random writes.

**Q: What are write, read and space amplification?**
A: Write amplification is bytes written to storage per byte of user data: a B-tree rewrites whole
pages and logs full-page images; a leveled LSM rewrites each key once per level during compaction.
Read amplification is storage accesses per logical read: one tree path for a B-tree, several
levels for an LSM. Space amplification is disk used over live data: B-tree fragmentation versus
LSM duplicate versions awaiting compaction. You can minimise two of the three, not all.

**Q: Why is YugabyteDB built on RocksDB?**
A: A distributed SQL database needs a crash-safe, ordered, compressed, cache-aware local storage
engine per shard and should not reinvent one. RocksDB is embedded, write-optimised for the
sequential stream of Raft log entries, and its immutable SSTables make snapshots and tablet moves
cheap; YugabyteDB disables RocksDB's WAL because Raft provides durability, and encodes MVCC
hybrid timestamps in the keys.

**Q: When would you choose an embedded store over a server database?**
A: When a single process owns the data, needs microsecond access without a network, and
operational simplicity matters more than multi-client access: local caches, state stores in stream
processors, desktop and mobile apps, or as the storage layer inside a larger system. Once several
services or people need the data, or you need SQL, replication and managed operations, a server
database wins.

## 8. Recap

- Berkeley DB = library + shared-memory environment with mpool, lock, log, txn subsystems.
- Access methods: BTREE (ordered, cursors, duplicates), HASH, RECNO, QUEUE.
- Handles: DB_ENV, DB, DBC, DB_TXN; DBT carries opaque bytes; secondaries via associate callbacks.
- Page-level 2PL locking: readers block writers, deadlocks common; app configures detection and retries.
- Undo/redo physical logging; the app calls txn_checkpoint and passes DB_RECOVER.
- Cursor traversal: DB_SET_RANGE then DB_NEXT; close cursors before commit; DB_CURRENT to update.
- LSM: WAL + memtable + immutable SSTables + compaction; sequential writes, multi-level reads, bloom filters.
- Leveled compaction: low space, high write amplification; tiered: the reverse.
- RocksDB = LevelDB + column families, parallel compaction, transactions, merge operators.
- YugabyteDB puts one RocksDB per tablet under Raft: the KV store moved down a layer, it did not disappear.
