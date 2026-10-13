# 3. B-tree indexes in depth

**Time:** ~3 hours   **Prerequisites:** 02

## 1. In plain words

An index is a second, smaller, sorted copy of some columns, with a pointer from each entry back
to the row it came from. Because it is sorted, you can find an entry by halving the search space
repeatedly instead of reading everything. Because it is stored as a tree of pages where each
page holds hundreds of entries, three or four page reads find one row among hundreds of millions.

The analogy: a printed phone book is a B-tree index on (surname, first name). You open it
roughly in the middle, see "M", jump forward, see "Ra", jump to the right page, scan a few lines.
Four looks instead of reading 2000 pages. But notice: the phone book is useless if you know only
the first name. Column order in a composite index is exactly this problem, and it is the heart
of the "login path doing full scans" story on your resume.

Every index you add speeds up some reads and slows down every write to that table, because the
sorted copy must be maintained. Senior engineers know both halves.

## 2. The real thing

### 2.1 Structure on disk

PostgreSQL's default index type is a **B+tree** (all data in leaves, internal pages hold only
separators), of the Lehman and Yao variant: every page has a pointer to its right sibling and a
**high key** (an upper bound on what the page may contain), which lets readers cope with a
concurrent page split without locking the whole tree.

```
 block 0: meta page  (root block number, tree height, version)

                  +--------------------------+
 level 2 (root)   |  k1 | k2 | k3 | ... hk   |          internal: (separator key, child block)
                  +--+-----+-----+-----------+
                     |     |     |
          +----------+     |     +-----------+
          v                v                 v
 level 1  +---------+ <-> +---------+ <-> +---------+   siblings linked left/right
          | ... hk  |     | ... hk  |     | ... hk  |
          +--+--+---+     +--+--+---+     +---------+
             |  |            |  |
 level 0     v  v            v  v
 (leaves)  +--------------+ <-> +--------------+ <-> ...   leaf: (key, heap TID) in key order
           | k,tid k,tid  |     | k,tid k,tid  |
           +--------------+     +--------------+
```

Each index page is 8 KB with the same slotted layout as a heap page; the special space at the
end holds `BTPageOpaqueData`: left and right sibling block numbers, level, flags (leaf, root,
deleted, half-dead). An index tuple is an 8-byte header (6-byte heap TID + 2 bytes of
length/flags) followed by the key columns, aligned. For a `bigint` key that is 16 bytes, so a
leaf page holds about 400 entries (fewer with the 10% default free space). Since PostgreSQL 13,
duplicate keys in a leaf are stored once with a **posting list** of TIDs (deduplication), which
shrinks indexes on low-cardinality columns a lot.

**Fan-out and height.** If a page holds roughly 400 entries, a tree of height 3 (root, one
internal level, leaves) addresses 400 x 400 x 400 = 64 million entries; height 4 reaches 25
billion. A lookup reads one page per level plus the heap page: 4 or 5 page reads for a row in a
400-million-row table. The root and the first internal level are almost always in
`shared_buffers`, so in practice a point lookup costs 1 or 2 real I/Os. This is the number to
quote in interviews.

**Search.** `_bt_search()` starts at the root, binary-searches the separator keys on each page to
pick the child, descends to a leaf, then binary-searches within the leaf for the first matching
entry. A range scan then walks right along the sibling pointers reading leaves in order: this is
why `ORDER BY indexed_col` can be satisfied with no sort.

### 2.2 Page splits

A leaf is full; a new key must go in. The leaf is split into two: by default roughly 50/50 by
space, except when inserting at the rightmost end (ascending keys such as a `bigserial` or a
timestamp), where the split leaves the old page full and starts a fresh right page (the
"fillfactor" split, 90% by default). A new separator key is inserted into the parent; if the
parent is full, it splits too, and so on up; splitting the root adds a level. Splits are
WAL-logged as one atomic record. Costs: a split copies half a page, writes two index pages and
one parent page, and leaves two half-empty pages, which is one source of **index bloat**.

Random keys (UUID v4, hashes) split everywhere in the tree and keep every leaf around 50 to 75%
full; monotonically increasing keys append to the right edge and keep leaves near 90% full with
fewer splits. This is a real reason to prefer `bigint` identity keys or time-ordered UUIDs
(v7) over random UUIDs for high-insert tables.

### 2.3 Scan types the planner can choose

| Scan | What it does | When |
|---|---|---|
| Index Scan | descend, then for each matching entry fetch the heap tuple immediately | few rows, or ORDER BY needs index order |
| Index Only Scan | like Index Scan but skips the heap fetch when the VM says the page is all-visible | all needed columns are in the index |
| Bitmap Index Scan + Bitmap Heap Scan | collect all matching TIDs into a bitmap sorted by page, then read heap pages in physical order | many rows (hundreds to a few %), or combining several indexes with AND/OR |
| Seq Scan | read the whole heap | large fraction of rows, or no usable index |

The Bitmap scan exists because fetching 50,000 heap tuples in index order means 50,000 random
page reads with repeats; sorting the TIDs by page first turns that into one sequential-ish pass.
When the bitmap would exceed `work_mem` it becomes **lossy**: it remembers only pages, not
tuples, and every tuple on those pages is rechecked ("Recheck Cond" in EXPLAIN).

### 2.4 Covering indexes and INCLUDE

An index **covers** a query when every column the query needs is in the index, so the heap
need not be touched (given all-visible pages). Two ways:

```sql
-- key columns only: all are searchable and sorted
CREATE INDEX ON users (login_code, branch_code, status);

-- INCLUDE (PostgreSQL 11+): payload columns stored in leaf tuples only,
-- not part of the key: narrower internal pages, cannot be searched/sorted on,
-- but allows UNIQUE on (login_code, branch_code) while still covering status.
CREATE UNIQUE INDEX ON users (login_code, branch_code) INCLUDE (status, password_hash);
```

Check with EXPLAIN: `Index Only Scan ... Heap Fetches: 0`. If Heap Fetches is high, the table
needs a VACUUM to set visibility map bits. Covering indexes trade index size and write cost for
avoiding a heap read per row, which is a big win for hot point lookups like a login check.

### 2.5 Composite index column order: the rules

An index on `(a, b, c)` is sorted by `a`, then `b` within equal `a`, then `c`. It can
efficiently serve a condition on a **leftmost prefix**: `a`; `a, b`; `a, b, c`. It cannot
efficiently serve `b` alone or `b, c` (PostgreSQL 16 would have to scan the whole index; skip
scan arrived in 18). Rules, in priority order:

1. **Equality columns first.** Columns compared with `=` in the WHERE clause go leftmost. Among
   them, put the most selective first only if you also need prefix queries on it; otherwise order
   does not matter much for a full-match lookup.
2. **Then one range column.** `<`, `>`, `BETWEEN`, `LIKE 'abc%'`. After a range condition, the
   remaining index columns cannot narrow the scan (they are not sorted within the range), only
   filter rows that are already read.
3. **Then ORDER BY columns**, in the same direction, so the sort disappears.
4. **Then INCLUDE** what you merely need to output.

Example: `WHERE branch_code = $1 AND status = 'ACTIVE' AND created_at > $2 ORDER BY created_at`
wants `(branch_code, status, created_at)`. An index `(created_at, branch_code, status)` would
scan every row after `$2` and filter: often ten to a hundred times more pages.

### 2.6 Selectivity and when the planner refuses the index

**Selectivity** is the fraction of rows a condition matches. An index lookup costs roughly
(tree height + matching leaf pages + one random heap read per matching row). A seq scan costs
(number of heap pages, sequentially). With `random_page_cost = 4` and `seq_page_cost = 1`, once a
condition matches more than a few percent of rows, random heap fetches cost more than reading
everything. The planner then correctly picks Seq Scan or Bitmap Heap Scan even though an index
exists. An index on a boolean or a status column with 3 values is rarely used on its own; a
**partial index** (`WHERE status = 'PENDING'`) on the rare value is the right tool.

The planner's estimate comes from `pg_statistic` (module 06): `n_distinct`, most common values
and their frequencies, a histogram. Stale statistics give wrong selectivity and wrong plans.

### 2.7 The login path that did full scans

Your resume says the login latency bottleneck came from full-table scans, because the tables
were keyed by business codes rather than surrogate IDs. Here is the mechanism, so you can explain
it precisely.

The user table came from a Berkeley DB layout where the record key was a packed business code
(say `branch_code || user_code`). In the relational schema it got a surrogate `id bigserial
PRIMARY KEY`, and the business codes became ordinary columns. The login procedure, ported from
the old record-lookup logic, queried:

```sql
SELECT id, password_hash, status, failed_attempts
FROM app_users
WHERE user_code = $1 AND branch_code = $2;
```

With a primary key on `id` only, the planner has no index on `(user_code, branch_code)`, so it
does a **Seq Scan** of the whole table on every login, plus similar scans on the role and branch
tables it joined. On a single node with a small table this is a few milliseconds and nobody
notices. On a multi-node YugabyteDB cluster the table is hash-sharded across nodes, so a
sequential scan fans out to every tablet on every node and merges, and the latency is tens to
hundreds of milliseconds per statement, times the number of statements in the login path.

The fix was a **unique composite index on the natural key** with INCLUDE for the columns the
login needs, so the lookup is an index-only point read on exactly one tablet:

```sql
CREATE UNIQUE INDEX app_users_login_idx
    ON app_users (branch_code, user_code)
    INCLUDE (password_hash, status, failed_attempts);
```

Note the order: both are equality columns, so either order serves the login; `branch_code` first
also serves "all users in a branch" queries. Module 06 walks the EXPLAIN plans before and after;
module 11 explains what hash sharding does to this index on YugabyteDB.

Second part of the same story: implicit casts. If `user_code` is `varchar` and the application
bound the parameter as `bigint`, the condition becomes `user_code::bigint = $1` or
`user_code = $1::text` depending on the driver; the first form cannot use a `varchar` index at
all. Always bind parameters with the column's type (module 07).

### 2.8 Other index types, briefly

| Type | Structure | Use it for | Not for |
|---|---|---|---|
| **Hash** | hash buckets of TIDs | equality on long values where a B-tree would be wide; WAL-logged since 10 | ranges, ordering, multi-column |
| **GIN** | inverted index: each element/token -> posting list of TIDs | `jsonb @>`, arrays `@>`/`&&`, full-text `@@`, trigram `LIKE '%x%'` | point lookups on scalars; slower updates (uses a pending list) |
| **GiST** | balanced tree of bounding predicates (generalised search tree) | geometry, ranges (`&&`), nearest-neighbour `ORDER BY <->`, exclusion constraints | plain equality |
| **SP-GiST** | space-partitioned tree (quad tree, k-d tree, radix tree) | points, IP ranges, text prefixes | general use |
| **BRIN** | per block range (default 128 pages) min/max summary | huge append-only tables where the column correlates with physical order (timestamps) | random data; it is tiny but lossy |

B-tree handles 95% of OLTP needs. Mention the others by name and one use case each.

### 2.9 Index bloat and HOT updates

**Bloat.** Deleting a row leaves its index entries until VACUUM removes them; even then the
space stays in half-empty pages, because a B-tree page is only recycled when completely empty.
Indexes on columns that are updated often, or on tables with heavy churn, grow to several times
their ideal size. Measure with `pgstatindex('idx_name')`: `avg_leaf_density` under 50% and a
large `leaf_fragmentation` say rebuild. Rebuild online with `REINDEX INDEX CONCURRENTLY`
(PostgreSQL 12+). PostgreSQL 14 added **bottom-up index deletion**: before splitting a page, the
B-tree tries to remove entries that point at dead heap tuples, which greatly reduces bloat from
non-HOT updates.

**HOT (heap-only tuple) updates.** An UPDATE normally creates a new heap tuple *and* a new entry
in every index on the table, because the new tuple has a new TID. If (a) no indexed column
changed and (b) the new tuple fits on the same heap page, PostgreSQL skips the index updates:
the new tuple is marked `HEAP_ONLY_TUPLE`, the old one `HOT_UPDATED`, and the old tuple's
`t_ctid` points at the new one. Index entries still point at the old slot; readers follow the
chain within the page. When the old version dies, pruning turns its line pointer into
`LP_REDIRECT` straight to the live version. Result: updating `last_login_at` on a user row costs
one heap page write and zero index writes, as long as `last_login_at` is not indexed and the page
has room.

Two design consequences: do not index columns that change on every update unless you must; and
set a lower `fillfactor` (e.g. 80) on hot update tables so pages keep room for new versions.
Watch `n_tup_hot_upd / n_tup_upd` in `pg_stat_user_tables`.

## 3. Code

`code/m03_index_demos.sql` builds a 2 million row user table and demonstrates each idea.

```sql
-- A user table shaped like the login story: surrogate PK, business codes as columns
CREATE TABLE app_users (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    branch_code     text    NOT NULL,
    user_code       text    NOT NULL,
    password_hash   text    NOT NULL,
    status          text    NOT NULL DEFAULT 'ACTIVE',
    failed_attempts int     NOT NULL DEFAULT 0,
    last_login_at   timestamptz,
    created_at      timestamptz NOT NULL DEFAULT now()
);
INSERT INTO app_users (branch_code, user_code, password_hash, status)
SELECT 'BR' || lpad((g % 500)::text, 4, '0'),            -- 500 branches
       'U'  || lpad(g::text, 8, '0'),
       md5(g::text),
       CASE WHEN g % 50 = 0 THEN 'LOCKED' ELSE 'ACTIVE' END
FROM generate_series(1, 2000000) g;
ANALYZE app_users;

-- 1. Before: Seq Scan. Note "Rows Removed by Filter" and the buffer count.
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status FROM app_users
WHERE user_code = 'U00123456' AND branch_code = 'BR0456';

-- 2. The fix: unique composite index on the natural key, covering the login columns.
CREATE UNIQUE INDEX app_users_login_idx
    ON app_users (branch_code, user_code) INCLUDE (password_hash, status, failed_attempts);
VACUUM app_users;   -- set visibility map bits so the index-only scan skips the heap

EXPLAIN (ANALYZE, BUFFERS)
SELECT password_hash, status FROM app_users
WHERE user_code = 'U00123456' AND branch_code = 'BR0456';
-- expect: Index Only Scan using app_users_login_idx ... Heap Fetches: 0, Buffers: shared hit=4

-- 3. Leftmost prefix: branch alone uses the index; user_code alone cannot.
EXPLAIN SELECT count(*) FROM app_users WHERE branch_code = 'BR0456';     -- index
EXPLAIN SELECT count(*) FROM app_users WHERE user_code  = 'U00123456';   -- seq scan (or parallel seq scan)

-- 4. Tree height and fan-out
CREATE EXTENSION IF NOT EXISTS pageinspect;
SELECT * FROM bt_metap('app_users_login_idx');           -- "level" = height - 1
SELECT type, live_items, avg_item_size, free_size
FROM bt_page_stats('app_users_login_idx', (SELECT root FROM bt_metap('app_users_login_idx')));

-- 5. Selectivity: the planner ignores an index on a low-cardinality column...
CREATE INDEX app_users_status_idx ON app_users (status);
EXPLAIN SELECT count(*) FROM app_users WHERE status = 'ACTIVE';   -- 98% of rows: seq scan
EXPLAIN SELECT count(*) FROM app_users WHERE status = 'LOCKED';   -- 2%: bitmap or index scan
-- ... and a partial index is the right tool for the rare value
DROP INDEX app_users_status_idx;
CREATE INDEX app_users_locked_idx ON app_users (branch_code) WHERE status = 'LOCKED';

-- 6. Implicit cast breaks the index
EXPLAIN SELECT id FROM app_users WHERE user_code = 123456;                   -- ERROR or cast of column
EXPLAIN SELECT id FROM app_users WHERE user_code::bigint = 123456;           -- seq scan, cannot use idx
EXPLAIN SELECT id FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U' || lpad('123456', 8, '0');  -- fine

-- 7. HOT updates: an update that does not touch indexed columns
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 'app_users';
UPDATE app_users SET last_login_at = now() WHERE id BETWEEN 1 AND 1000;
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 'app_users';
-- hot_upd should rise by ~1000. Now update an indexed column:
UPDATE app_users SET status = 'LOCKED' WHERE id BETWEEN 1 AND 1000;
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 'app_users';
-- hot_upd does not rise: status is in the INCLUDE list, so every index got a new entry

-- 8. Bloat measurement and online rebuild
CREATE EXTENSION IF NOT EXISTS pgstattuple;
SELECT avg_leaf_density, leaf_fragmentation FROM pgstatindex('app_users_login_idx');
REINDEX INDEX CONCURRENTLY app_users_login_idx;
```

## 4. Under the hood (senior-level)

**Concurrency without tree locks (Lehman and Yao).** A reader descending the tree holds a shared
lock on one page at a time and releases it before locking the child ("lock coupling" is *not*
used going down). If a split happens in between, the reader may land on a page whose high key is
less than its search key; it then follows the right-sibling link until it finds the right page.
The high key and right link make this safe. Writers hold an exclusive lock on the leaf while
inserting, and on the parent only while inserting the new separator. The result: readers never
block on the root, and the tree scales to many cores. Berkeley DB's B-tree, by comparison, uses
page-level locks held for the transaction's duration in transactional mode, which is one reason
it deadlocks more often under concurrent writers (module 10).

**Index entries and MVCC.** Index tuples have no `xmin`/`xmax`. An index entry exists for every
heap tuple version that was ever indexed, visible or not. That is why an Index Scan must fetch
the heap tuple (to check visibility) unless the visibility map says the whole page is visible,
and why dead heap tuples leave dead index entries behind until VACUUM. The `LP_DEAD` hint bit in
index pages (set by a scan that found the heap tuple dead) lets later inserts skip over or
remove those entries cheaply ("simple index deletion").

**Unique indexes and `INSERT ... ON CONFLICT`.** Uniqueness is checked at insert time by
searching the leaf for the same key and checking the visibility of each matching heap tuple. If a
matching tuple was inserted by an uncommitted transaction, the inserter waits for that
transaction (this is the "waits on another session's insert" you can see in `pg_locks` as a
transaction-id lock). `ON CONFLICT` uses **speculative insertion**: insert the heap tuple, try
to insert the index entry, and if a conflict appears, super-delete the heap tuple and take the
alternative path.

**Deduplication and posting lists (13+).** When a leaf page is about to split, the engine first
tries to merge duplicate keys into one entry with a sorted TID list. For an index on `status`
with 3 values this cuts the index to a fraction of its size and makes bitmap scans faster. It is
off for unique indexes except as a bloat control for non-HOT updates.

**Suffix truncation (12+).** Internal pages store only as much of a key as is needed to
separate children, so an index on `(branch_code, user_code)` has small internal tuples and high
fan-out even if leaf tuples are wide. The heap TID is treated as a tiebreaker column so that
duplicates are stored in TID order, which makes VACUUM's deletion of entries efficient.

**Why a B-tree and not a hash by default.** Hash indexes are O(1) for equality but cannot do
ranges, prefix matches, sorts, or multi-column prefix use, and they do not shrink. A B-tree does
equality in 3 to 4 page reads anyway, nearly all from cache. The constant factor difference is
negligible; the flexibility is not.

**Collation.** `text` comparisons under a non-C collation (`en_US.UTF-8`) use `strcoll`, which
is slow and which does not let `LIKE 'abc%'` use a plain B-tree (the ordering is not bytewise).
Use `COLLATE "C"` on code-like columns, or create the index with `text_pattern_ops`. Also, glibc
collation changes between OS versions have corrupted indexes after upgrades (the sort order
changed under the index); PostgreSQL 15+ tracks collation versions and warns.

## 5. In production

- **Index every foreign key column** that you join or delete through; PostgreSQL does not do it
  for you (unlike Oracle's recommendation and SQL Server, which also do not; MySQL InnoDB does).
  A `DELETE FROM customers WHERE id = 1` without an index on `loan_applications.customer_id`
  seq-scans the child table for the FK check, under a lock.
- **Unused indexes cost writes.** `pg_stat_user_indexes.idx_scan = 0` since the last stats reset
  on a production system means the index only costs you. Drop it (after checking replicas, where
  reads may differ).
- **Duplicate and redundant indexes**: `(a)` is redundant with `(a, b)` for lookups on `a`.
  Review before adding.
- **`CREATE INDEX CONCURRENTLY`** in production, always: the plain form takes a `SHARE` lock
  that blocks writes for the duration. CONCURRENTLY does two table scans and waits for existing
  transactions; it can leave an `INVALID` index if interrupted, which you must drop and retry.
- **Index-only scans need VACUUM.** A fresh bulk load has no VM bits set; the first queries
  do heap fetches for every row and look slow. Run `VACUUM` (not FULL) after a bulk load.
- **The 3 AM version**: "login is slow since the deployment". `pg_stat_statements` shows the
  login SELECT with `mean_exec_time` up 50x and `shared_blks_read` huge; EXPLAIN shows Seq Scan;
  the deployment added a column to the query that is not in the covering index, or changed a
  parameter type so the index no longer matches. Fix the query or the index; `ANALYZE`.
- On YugabyteDB the same mistakes cost far more because scans fan out across nodes (module 11).

## 6. Try it

1. **(easy)** Run `code/m03_index_demos.sql` steps 1 to 3. Done when you have both EXPLAIN
   outputs saved and can read `Buffers: shared hit` for each.
2. **(easy)** With `bt_metap`, find the height of `app_users_login_idx` and `app_users_pkey`.
   Done when you can compute the approximate fan-out from `live_items` on the root.
3. **(medium)** Write three queries that need indexes on `loan_applications`: by
   `(customer_id)`, by `(status, submitted_at)` for a work queue sorted by `submitted_at`, and by
   `application_no`. Design the minimum set of indexes, create them, and show with EXPLAIN that
   each query uses an index with no Sort node. Done when all three plans are index-driven.
4. **(medium)** Measure HOT: update a non-indexed column 100,000 times on `app_users` with
   fillfactor 100 vs a copy with `fillfactor = 80`. Compare `n_tup_hot_upd` and
   `pg_relation_size`. Done when you can state the HOT ratio for each.
5. **(hard)** Create a table with a random `uuid` primary key and one with a `bigint` identity
   key, insert 2 million rows into each, and compare index size (`pg_relation_size`),
   `avg_leaf_density` from `pgstatindex`, and insert time. Done when you can explain the
   difference in terms of page splits at the right edge versus everywhere.
6. **(hard)** Reproduce the implicit-cast problem: with `user_code text`, bind a `bigint`
   parameter from the libpq program in module 01 (`paramTypes[0] = 20`). Observe the error or
   the plan, then fix the binding. Done when you can explain which side of the comparison gets
   cast and why that disables the index.

## 7. Interview questions

**Q: How does a B-tree index find a row, and how many page reads does it take?**
A: Start at the root, binary-search the separator keys to choose a child, repeat down to a leaf,
binary-search the leaf for the key, read the heap TID, fetch the heap page. Height is 3 or 4 for
tables up to billions of rows because each 8 KB page holds hundreds of entries, so a point
lookup is 4 or 5 page reads, of which the upper levels are nearly always cached.

**Q: What is a covering index and what is INCLUDE for?**
A: A covering index contains every column a query needs, so the executor can answer from the
index alone without visiting the heap (an Index Only Scan, if the visibility map says the pages
are all-visible). INCLUDE adds payload columns to leaf entries without making them part of the
key, which keeps the key unique and the internal pages narrow.

**Q: How do you order columns in a composite index?**
A: Equality predicates first, then at most one range predicate, then ORDER BY columns in the same
direction, then INCLUDE columns for output. The index can only use a leftmost prefix of its
columns to narrow the scan; columns after a range predicate only filter.

**Q: Why did your login query do a full table scan?**
A: The table carried a surrogate primary key, but the login looked up by the natural key
(branch code plus user code) that had no index, so every login was a sequential scan, which on a
hash-sharded YugabyteDB cluster fans out to every node. A unique composite index on the natural
key with INCLUDE for the needed columns turned it into a single-tablet index-only point read.

**Q: Why does the planner sometimes ignore an index?**
A: Because the estimated selectivity is low: when a condition matches more than a few percent of
rows, one random heap read per matching row costs more than scanning the table sequentially.
That is correct behaviour. Fix the query, use a partial index on the rare value, or update
statistics if the estimate is wrong.

**Q: What is a HOT update?**
A: An update where no indexed column changed and the new tuple fits on the same heap page. The
engine skips all index insertions; the old tuple's ctid points to the new version and readers
follow the chain within the page. It makes updates of non-indexed columns far cheaper and is a
reason not to index columns that change on every write, and to leave fillfactor headroom.

**Q: What causes index bloat and how do you fix it?**
A: Deletes and non-HOT updates leave dead entries; page splits leave half-empty pages; B-tree
pages are only recycled when empty. Measure with `pgstatindex` (leaf density), rebuild online
with `REINDEX CONCURRENTLY`, and reduce the cause: fewer indexed volatile columns, HOT-friendly
fillfactor, timely VACUUM.

**Q: When would you use GIN, GiST or BRIN instead of B-tree?**
A: GIN for containment and membership on composite values: jsonb, arrays, full-text, trigram
search. GiST for spatial and range overlap, nearest neighbour, and exclusion constraints. BRIN
for very large append-only tables where the column correlates with physical order, such as
timestamps, where a tiny lossy summary per block range is enough.

## 8. Recap

- B+tree: meta page, root, internal separators, linked leaves holding (key, heap TID).
- Fan-out of hundreds gives height 3 to 4 for billions of rows; point lookup is 4 or 5 page reads.
- Splits are 50/50 except at the right edge; random keys bloat, sequential keys pack.
- Scan types: Index, Index Only (needs VM), Bitmap (many rows, sorted by page), Seq.
- Composite order: equality, one range, ORDER BY, INCLUDE. Leftmost prefix only.
- Low selectivity means the planner is right to skip the index; partial indexes for rare values.
- The login full scan: natural key not indexed; fix = unique composite covering index.
- Implicit casts on the column side disable indexes; bind the right type.
- HOT updates skip index maintenance when no indexed column changes and the page has room.
- Index entries have no MVCC info; dead entries wait for VACUUM; rebuild bloat with REINDEX CONCURRENTLY.
