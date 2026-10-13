# 6. Query planning and reading EXPLAIN

**Time:** ~3 hours   **Prerequisites:** 03, 05

## 1. In plain words

SQL says *what* you want, not *how* to get it. For `SELECT ... FROM a JOIN b JOIN c WHERE ...`
there are dozens of ways to execute: which table to read first, whether to use an index, how
to join (loop, hash table, or merge sorted streams), when to sort. The **planner** (optimiser)
estimates the cost of many alternatives using statistics about the data and picks the cheapest
estimate. **EXPLAIN** shows you the plan it picked; **EXPLAIN ANALYZE** runs it and shows what
actually happened next to each estimate.

The analogy is a satnav. It does not know the current traffic on every road; it uses a map
(statistics: how many rows, how many distinct values, the value distribution) and a cost per
kilometre (I/O and CPU cost constants) to choose a route. When the map is out of date (stale
statistics) or the costs are wrong for your car (random I/O on SSD is not really 4x sequential),
it chooses badly. Reading EXPLAIN is comparing the route it planned against the trip you actually
took and spotting where the estimate was off by 1000x.

Every performance problem you will be asked about in an interview reduces to: find the plan node
where estimated and actual rows diverge, or where time concentrates, and change the index, the
statistics, or the query so the planner can do better.

## 2. The real thing

### 2.1 What the planner knows: statistics

`ANALYZE` (run by autovacuum when about 10% of a table changed, or by you) samples
`300 x default_statistics_target` rows (30,000 at the default target of 100) and stores per column
in `pg_statistic` (readable through the `pg_stats` view):

| Column | Meaning | Used for |
|---|---|---|
| `null_frac` | fraction NULL | `IS NULL`, and reducing row estimates |
| `avg_width` | average bytes | `width` in plans, memory estimates for hash and sort |
| `n_distinct` | number of distinct values (negative = fraction of rows, for columns that grow) | `=` selectivity when the value is not in MCVs, GROUP BY estimates, hash sizing |
| `most_common_vals` / `most_common_freqs` | the top-N values and their frequencies | `= 'ACTIVE'` gets its exact frequency |
| `histogram_bounds` | equal-frequency bucket boundaries for the remaining values | ranges `<`, `>`, `BETWEEN` |
| `correlation` | -1 to 1, physical order vs logical order | whether an index range scan reads the heap sequentially (cheap) or randomly (expensive) |

Selectivity of `col = X`: if X is in the MCV list, its frequency; else `(1 - sum(mcv_freqs)) /
(n_distinct - number_of_mcvs)`. Selectivity of `a = X AND b = Y`: the planner **multiplies**,
assuming independence. If `branch_code` and `region` are correlated (every branch is in one
region), the product badly underestimates. Fix with **extended statistics**:
`CREATE STATISTICS s (dependencies, ndistinct, mcv) ON branch_code, region FROM app_users`.

Raise the target for skewed or high-cardinality columns:
`ALTER TABLE t ALTER COLUMN c SET STATISTICS 1000` (max 10000). More buckets, more MCVs, slower
ANALYZE and planning.

Also in `pg_class`: `reltuples` and `relpages`, the base numbers every estimate scales from.

### 2.2 The cost model

Cost is a dimensionless number built from these constants (defaults):

| Parameter | Default | Meaning |
|---|---|---|
| `seq_page_cost` | 1.0 | one page read in a sequential scan |
| `random_page_cost` | 4.0 | one page read at random (index probes); set 1.1 to 1.5 on SSD |
| `cpu_tuple_cost` | 0.01 | processing one heap tuple |
| `cpu_index_tuple_cost` | 0.005 | processing one index entry |
| `cpu_operator_cost` | 0.0025 | one operator or function call |
| `effective_cache_size` | 4 GB | how much of the index is assumed cached (lowers index cost); set to ~75% of RAM |
| `work_mem` | 4 MB | per sort/hash node budget before spilling; changes plan choice |
| `parallel_setup_cost`, `parallel_tuple_cost` | 1000, 0.1 | when parallel plans are worth it |

Each plan node shows `cost=startup..total`: startup is the cost before the first row can be
returned (a Sort or Hash build has high startup; an Index Scan near zero), total is for all rows.
A `LIMIT 10` prefers low-startup plans, which is why `ORDER BY created_at LIMIT 10` loves an
index on `created_at` and hates a Sort.

The planner enumerates join orders exhaustively (dynamic programming) up to
`join_collapse_limit` tables (8), and switches to a genetic search (`geqo_threshold`, 12) above
that. A 15-table join gets a heuristic plan; break it up or use CTEs as optimisation fences
(`WITH ... AS MATERIALIZED`).

### 2.3 Scan and join methods

**Scans** (module 03): Seq Scan, Index Scan, Index Only Scan, Bitmap Index + Bitmap Heap Scan,
plus `Parallel Seq Scan` under a `Gather` node, `TID Scan` (by ctid), `Function Scan`
(`generate_series`), `CTE Scan`, `Subquery Scan`, `Values Scan`.

**Joins**:

| Method | How | Cost shape | Best when |
|---|---|---|---|
| **Nested Loop** | for each outer row, scan inner (ideally an Index Scan parameterised by the outer row's key) | outer_rows x inner_cost | outer side small (tens to a few thousand rows) and inner has an index on the join key. The only method for inequality joins. |
| **Hash Join** | build a hash table on the smaller input (hashed on the join key), probe with each row of the larger | read both once + hash build; needs `work_mem`, else `Batches > 1` spills to temp files | equality joins of medium and large sets; the default workhorse |
| **Merge Join** | both inputs sorted on the join key (by index or an explicit Sort), walk them together | read both once + sort cost if not pre-sorted | both sides already ordered (indexes on join keys), very large joins where hash would spill, or when output order is needed anyway |

PostgreSQL 14 added **Memoize**: a cache in front of the inner side of a nested loop so repeated
outer values do not re-probe. You will see it as `Memoize` with `Hits`/`Misses`.

**Other nodes**: `Sort` (quicksort in memory, `top-N heapsort` for LIMIT, `external merge` on
disk), `Incremental Sort` (input already sorted on a prefix), `Aggregate` / `HashAggregate` /
`GroupAggregate`, `Unique`, `Limit`, `Append` (UNION ALL, partitions), `Materialize`, `WindowAgg`,
`Gather`/`Gather Merge` (parallel workers), `Result`, `SetOp`, `LockRows` (FOR UPDATE),
`ModifyTable` (INSERT/UPDATE/DELETE).

### 2.4 Reading EXPLAIN (ANALYZE, BUFFERS) line by line

```
EXPLAIN (ANALYZE, BUFFERS, VERBOSE off, SETTINGS off, WAL off, FORMAT text)
SELECT c.full_name, a.application_no, a.requested_amount
FROM loan_applications a
JOIN customers c ON c.id = a.customer_id
WHERE a.status = 'SUBMITTED' AND a.submitted_at > now() - interval '7 days'
ORDER BY a.submitted_at DESC
LIMIT 20;

Limit  (cost=0.86..118.42 rows=20 width=52) (actual time=0.061..0.412 rows=20 loops=1)
  Buffers: shared hit=104
  ->  Nested Loop  (cost=0.86..3121.70 rows=531 width=52) (actual time=0.059..0.405 rows=20 loops=1)
        Buffers: shared hit=104
        ->  Index Scan Backward using la_status_submitted_idx on loan_applications a
                (cost=0.43..1032.21 rows=531 width=31) (actual time=0.031..0.090 rows=20 loops=1)
              Index Cond: ((status = 'SUBMITTED'::text) AND (submitted_at > (now() - '7 days'::interval)))
              Buffers: shared hit=24
        ->  Index Scan using customers_pkey on customers c
                (cost=0.43..3.94 rows=1 width=29) (actual time=0.014..0.014 rows=1 loops=20)
              Index Cond: (id = a.customer_id)
              Buffers: shared hit=80
Planning:
  Buffers: shared hit=12
Planning Time: 0.410 ms
Execution Time: 0.455 ms
```

How to read it:

1. **Indentation is the tree**; the deepest-indented nodes run first. Children feed parents.
   Here: the Index Scan Backward produces applications, the inner Index Scan looks up each
   customer, Nested Loop combines, Limit stops after 20.
2. **`cost=a..b`**: estimated startup..total. **`rows`**: estimated rows *per loop*. **`width`**:
   estimated bytes per row.
3. **`actual time=a..b`**: milliseconds to first row..all rows, *per loop*. **`rows`**: actual
   rows per loop. **`loops`**: how many times the node ran. Total rows = rows x loops; total time
   = time x loops. The inner Index Scan ran 20 times, 0.014 ms each.
4. **Estimated vs actual rows**: the outer scan estimated 531 but stopped at 20 because of
   LIMIT; that is fine. A mismatch of 100x or more on a node *without* a LIMIT above it is the
   usual root cause of a bad plan.
5. **`Buffers: shared hit=N read=M dirtied=D written=W`**: pages from shared_buffers, pages
   read from the OS (cache or disk), pages dirtied, pages written. `temp read/written` means
   sort or hash spilled to disk. `read` is where your I/O time is; enable `track_io_timing` to
   get `I/O Timings: read=…`.
6. **`Index Cond`** is what the index narrowed; **`Filter`** is applied to rows after they are
   fetched, with `Rows Removed by Filter` telling you how much was wasted. A large Rows Removed
   by Filter under an Index Scan means the index is not selective enough for this query.
7. **`Recheck Cond`** under a Bitmap Heap Scan means the bitmap went lossy (increase `work_mem`)
   or the index is lossy by nature (GIN, BRIN).
8. **`Heap Fetches`** under an Index Only Scan: non-zero means the visibility map is not set;
   VACUUM.
9. **`Sort Method: external merge  Disk: 41232kB`**: `work_mem` too small for this sort.
   **`Hash Batches: 4`**: same for a hash join.
10. **Planning Time** vs **Execution Time**: a 20 ms planning time on a 0.5 ms query is a sign
    you want prepared statements or fewer partitions/joins.

Use `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` and paste into explain.dalibo.com or
explain.depesz.com for big plans; both highlight the node where time or misestimation concentrates.

### 2.5 Common plan problems and their fixes

| Symptom in the plan | Cause | Fix |
|---|---|---|
| Seq Scan on a big table with `Filter` and `Rows Removed by Filter` huge | no usable index for the predicate | create the index (right column order); or rewrite predicate to be sargable |
| Index exists, Seq Scan chosen, `rows` estimate far above actual | stale statistics, or correlated columns | `ANALYZE`; raise statistics target; `CREATE STATISTICS` |
| Index exists, Seq Scan chosen, estimate correct | low selectivity: index would not help | accept it; partial index for the rare value; cover more of the query |
| `Filter: (lower(email) = ...)` | function on the column | expression index `ON (lower(email))` or store normalised |
| `Filter: ((user_code)::bigint = $1)` or `(id)::text = $1` | implicit cast on the column | bind the right parameter type |
| `Filter: (code ~~ '%abc%')` | leading-wildcard LIKE | GIN with `pg_trgm`, or full-text |
| Nested Loop with inner Seq Scan, `loops=50000` | inner side has no index on the join key, planner thought outer was small | index the join column; `ANALYZE` |
| Nested Loop chosen, outer `rows=1` estimated but `rows=80000` actual | misestimate turned a hash join into millions of inner probes | fix statistics; extended stats; as a last resort `SET enable_nestloop = off` for that query to confirm |
| `Sort Method: external merge` or `Hash Batches > 1` | `work_mem` too low for this node | `SET LOCAL work_mem = '64MB'` for that session/statement; add an index providing the order |
| `Heap Fetches: 48000` on Index Only Scan | VM bits not set | VACUUM the table |
| `Bitmap Heap Scan` with `Recheck Cond` and `lossy=` pages | bitmap exceeded `work_mem` | raise `work_mem` |
| `Limit` over `Sort` over `Seq Scan` for `ORDER BY x LIMIT n` | no index on `x` (in that direction) | index on `x`, or on `(filter_col, x)` |
| `OR` across two columns -> Seq Scan | one index cannot serve both | two indexes (planner can BitmapOr), or rewrite as UNION ALL |
| Many `CTE Scan` nodes | materialised CTEs re-read (pre-12 always; 12+ only when referenced twice or `MATERIALIZED`) | inline with `NOT MATERIALIZED` or rewrite |
| `Gather` with `Workers Launched: 0` | no free parallel workers | check `max_parallel_workers`, concurrency |
| `OFFSET 100000` | reads and discards 100,000 rows | keyset pagination (module 07) |

### 2.6 Prepared statements and "parameter sniffing"

A **prepared statement** (`PREPARE` / libpq `PQprepare` / JDBC `PreparedStatement`) parses and
analyses once and executes many times. The plan question: plan with the actual parameter values
(a **custom plan**, best plan for this value, but costs planning every time) or plan once for
any value (a **generic plan**, plans once, but cannot use value-specific statistics)?

PostgreSQL's rule (`plan_cache_mode = auto`): the first five executions use custom plans; after
that, if the generic plan's estimated cost is not worse than the average custom plan cost, it
switches to the generic plan for good. This causes the PostgreSQL version of SQL Server's
**parameter sniffing**: a query that is fast for most values and slow for a skewed value (`status
= 'ACTIVE'` = 98% of rows vs `status = 'LOCKED'` = 2%) may get a generic plan that is wrong for
one of them. Options: `SET plan_cache_mode = force_custom_plan` for that session or function;
rewrite so the skew is not a parameter (partial index, separate statements); or raise statistics
so the generic estimate is better. In EXPLAIN you recognise a generic plan by `$1` in the
conditions instead of a literal. Oracle calls its version bind peeking with adaptive cursor
sharing; SQL Server has `OPTION (RECOMPILE)` and `OPTIMIZE FOR`. PL/pgSQL caches plans for every
SQL statement in a function the same way (module 08).

### 2.7 Case study: the login query, from Seq Scan to Index Only Scan

Setting: `app_users` from module 03 (2 M rows, surrogate PK, business codes as columns),
originally no index on the codes. The login procedure runs this per attempt.

**Step 0: the slow plan.**

```
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status, failed_attempts
FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';

Gather  (cost=1000.00..38492.33 rows=1 width=49) (actual time=168.211..171.904 rows=1 loops=1)
  Workers Planned: 2
  Workers Launched: 2
  Buffers: shared hit=2112 read=20314
  ->  Parallel Seq Scan on app_users  (cost=0.00..37492.23 rows=1 width=49)
                                      (actual time=112.480..160.337 rows=0 loops=3)
        Filter: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
        Rows Removed by Filter: 666666
        Buffers: shared hit=2112 read=20314
Planning Time: 0.190 ms
Execution Time: 171.960 ms
```

(Typical output from the demo table on a laptop; your numbers will differ.) Reading it: the
estimate (`rows=1`) is right, so statistics are fine; the problem is structural: there is no
access path other than reading all 22,426 pages. 170 ms per login, multiplied by the 4 other
lookups in the login path (branch, roles, policy, session insert), and on a cluster each
sequential scan fans out to every node. Users saw 1 to 2 second logins.

**Step 1: the natural key index.**

```sql
CREATE UNIQUE INDEX CONCURRENTLY app_users_login_idx ON app_users (branch_code, user_code);
```

```
Index Scan using app_users_login_idx on app_users  (cost=0.43..8.45 rows=1 width=49)
                                                   (actual time=0.038..0.039 rows=1 loops=1)
  Index Cond: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
  Buffers: shared hit=5
Execution Time: 0.061 ms
```

5 buffers: 4 index pages (height 3 plus the meta page is cached; realistically root, internal,
leaf) and 1 heap page. 2800x faster.

**Step 2: cover the columns the login needs** so the heap page is not read either (saves one
random I/O when the heap page is cold, and on YugabyteDB saves a second storage read):

```sql
CREATE UNIQUE INDEX CONCURRENTLY app_users_login_idx2
    ON app_users (branch_code, user_code) INCLUDE (password_hash, status, failed_attempts);
DROP INDEX CONCURRENTLY app_users_login_idx;
VACUUM app_users;
```

```
Index Only Scan using app_users_login_idx2 on app_users  (cost=0.43..4.45 rows=1 width=49)
                                                         (actual time=0.021..0.022 rows=1 loops=1)
  Index Cond: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
  Heap Fetches: 0
  Buffers: shared hit=4
```

The login still needs `id` for the session row. It is in the index implicitly? No: the heap TID
is, not the `id` column. Add `id` to INCLUDE if the SELECT list needs it, or accept the one heap
fetch. This is the kind of detail that shows you actually read plans.

**Step 3: collapse the round trips.** The remaining cost was five statements, five round trips
(and on the cluster, five potential cross-node hops). Moving the whole login check into one
PL/pgSQL function (module 08) made it one round trip; the function's internal statements each
use the indexes above. Measured by `pg_stat_statements`: mean time of the login path went from
hundreds of milliseconds to a few milliseconds. Say the mechanism, not just the number.

**Step 4: keep it fast.** `auto_explain` with `auto_explain.log_min_duration = '50ms'` logs any
plan that regresses; the covering index must be extended if the query grows a column; the
`status` column is updated on lock-out so this index is non-HOT for those updates, which is an
accepted cost.

## 3. Code

`code/m06_slow_vs_fast_queries.sql` contains eight paired slow/fast queries over the
loan-origination schema with the expected plan shape in comments. Three of them:

```sql
-- Pair 1: function on the column vs expression index
EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM customers WHERE lower(email) = 'anna.k@example.com';
--   Seq Scan, Filter: (lower(email) = ...)   -- cannot use customers_email_idx
CREATE INDEX customers_email_lower_idx ON customers (lower(email));
EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM customers WHERE lower(email) = 'anna.k@example.com';
--   Index Scan using customers_email_lower_idx

-- Pair 2: work queue ORDER BY ... LIMIT with and without a matching index
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, application_no FROM loan_applications
WHERE status = 'SUBMITTED' ORDER BY submitted_at LIMIT 50;
--   Limit -> Sort (top-N heapsort) -> Bitmap Heap Scan ... reads every SUBMITTED row to sort
CREATE INDEX la_status_submitted_idx ON loan_applications (status, submitted_at);
--   Limit -> Index Scan using la_status_submitted_idx: stops after 50 rows, no Sort node

-- Pair 3: a hash join that spills, and the work_mem fix
SET work_mem = '256kB';
EXPLAIN (ANALYZE, BUFFERS)
SELECT c.id, count(*) FROM customers c JOIN loan_applications a ON a.customer_id = c.id GROUP BY c.id;
--   Hash Join ... Batches: 16  Memory Usage: 256kB ; temp read=... written=...
SET work_mem = '64MB';
--   Batches: 1, no temp buffers
RESET work_mem;
```

Also in that file: the statistics views, extended statistics for correlated columns, the
prepared-statement generic/custom demonstration, and `auto_explain` setup:

```sql
LOAD 'auto_explain';
SET auto_explain.log_min_duration = '20ms';
SET auto_explain.log_analyze = on;
SET auto_explain.log_buffers = on;
-- now any statement over 20 ms writes its ANALYZE plan to the server log

-- Generic vs custom plans
PREPARE login_q(text, text) AS
  SELECT password_hash, status FROM app_users WHERE branch_code = $1 AND user_code = $2;
EXPLAIN (ANALYZE) EXECUTE login_q('BR0456', 'U00123456');   -- run 6 times; watch literals turn into $1, $2
SET plan_cache_mode = force_custom_plan;                     -- per session or per function
```

## 4. Under the hood (senior-level)

**Planning pipeline.** `standard_planner` -> `subquery_planner` (pull up subqueries, flatten
joins, simplify constant expressions, convert `IN (subquery)` to semi-joins) -> `query_planner`
builds **RelOptInfo** for each base relation with its possible **paths** (SeqScan path, each
usable IndexPath, BitmapHeapPath, TidPath), each with cost and output ordering ("pathkeys").
`make_one_rel` then joins relations level by level (pairs, then triples) keeping for each set of
relations only the cheapest path per distinct ordering and parameterisation (`add_path` prunes
dominated paths). Finally `create_plan` turns the chosen path tree into Plan nodes. Planning is
pure CPU on catalog and statistics; it never touches the table.

**Parameterised paths.** The reason a Nested Loop can be good: the inner side's Index Scan can
take the outer row's join key as a parameter (`Index Cond: (id = a.customer_id)`), so the inner
cost is one probe, not a scan. The planner represents this as a path parameterised by the outer
relation. Hash and merge joins have no such option; they read the inner side fully.

**Selectivity functions.** Each operator in `pg_operator` names a restriction selectivity
function (`eqsel`, `scalarltsel`, `scalargtsel`) and a join selectivity function (`eqjoinsel`).
`eqsel` is the MCV / n_distinct logic above. `eqjoinsel` for `a.x = b.y` compares the MCV lists
of both sides and estimates the match fraction. When you write a custom operator or use a
function in WHERE, the planner falls back to default selectivities (0.5 for most, 0.005 for
`=` on unknowns), which is why function calls in predicates produce absurd estimates.

**Why `random_page_cost = 4` is wrong for you.** The default encodes spinning disks (one random
read costs several sequential ones, less so because of cache). On NVMe with a large OS cache, a
random read costs 1.1 to 1.5 sequential reads. Leaving the default makes the planner too afraid
of index scans and too fond of sequential and bitmap scans on large tables. Set it to 1.1 to 1.5
for SSD and re-check your top queries' plans; this is one of the few globally correct tuning
changes.

**Executor memory and spilling.** A Sort node collects tuples until `work_mem`, then switches to
external sort: writes sorted runs to temp files (`pgsql_tmp`), merges them. Hash Join computes
the number of batches from estimated inner size / `work_mem` up front; if the estimate was low
it increases batches on the fly, writing tuples to temp files per batch. Both appear as `temp
read/written` in BUFFERS. `work_mem` is per node per query (and per parallel worker), so a
query with four Sort nodes under 20 connections can use 80 x work_mem; hence the low default.
PostgreSQL 13+ has `hash_mem_multiplier` (default 2.0) to let hashes use more than sorts.

**JIT.** For queries whose total cost exceeds `jit_above_cost` (100000), PostgreSQL compiles
expression evaluation and tuple deforming with LLVM. It helps long analytic scans and hurts
short queries with high plan cost estimates (compilation overhead shows as `JIT: ... Timing`).
Turn `jit = off` on OLTP systems that show JIT time in plans.

**On YugabyteDB** the same planner runs, but the costs of a Seq Scan include network fan-out and
the executor pushes predicates down to DocDB; `EXPLAIN (ANALYZE, DIST)` adds `Storage Read
Requests` and `Storage Read Execution Time` per node, which is the number to minimise. Module 11.

## 5. In production

- **`pg_stat_statements` first, EXPLAIN second.** Sort by `total_exec_time` to find what burns
  the server, by `mean_exec_time` to find what users feel, and look at `shared_blks_read`
  for I/O-bound statements. Then EXPLAIN the top three. Optimising a query nobody runs is a hobby.
- **Run EXPLAIN ANALYZE on production-shaped data.** A plan on a 1000-row dev table is
  meaningless; the planner's choices depend on `reltuples` and distributions. Use a masked copy
  or `pg_stats` exported from production.
- **ANALYZE after bulk loads and big deletes.** Autovacuum's analyze threshold (10% changed) is
  fine for drip changes, not for "we loaded 50 M rows at midnight".
- **Plan regressions after upgrades or after `ANALYZE` itself** happen: a new sample tips a
  borderline estimate. `auto_explain` gives you the before/after. Pinning plans (Oracle's SQL
  plan baselines, SQL Server's Query Store forcing) does not exist in PostgreSQL; `pg_hint_plan`
  is the extension people use when they must.
- **Misestimates compound through joins.** A 10x underestimate on the first table becomes 10x on
  every join above it and turns a hash join into a nested loop over 100,000 rows. Fix the
  lowest misestimated node first.
- **`statement_timeout`** in the application connection (say 5 s for OLTP) converts "the whole
  system is slow because one bad plan holds 50 connections" into "one request failed and we have
  a log line".
- **3 AM**: everything slow, CPU at 100%, `pg_stat_activity` full of the same SELECT. Plan
  flipped: EXPLAIN shows Seq Scan where yesterday's log shows Index Scan. Immediate: `ANALYZE
  the_table`; if that does not fix it, check for an implicit cast introduced by a deployment, or a
  dropped/invalid index (`pg_index.indisvalid`).

## 6. Try it

1. **(easy)** Run the case study steps 0 to 2 on `app_users` from module 03 and save the three
   plans. Done when you can explain every line of the final Index Only Scan plan.
2. **(easy)** Run `code/m06_slow_vs_fast_queries.sql` pairs 1 to 4. Done when for each pair you
   can name the node that changed and why.
3. **(medium)** Take `pg_stats` for `loan_applications.status`: compute by hand the planner's
   estimated rows for `status = 'APPROVED'` from `most_common_freqs` x `reltuples`, then compare
   with `EXPLAIN SELECT * FROM loan_applications WHERE status = 'APPROVED'`. Done when they match
   within rounding.
4. **(medium)** Make the planner choose a nested loop badly: join `customers` to
   `loan_applications` with a predicate on two correlated columns so the estimate is 1 row but
   the actual is thousands. Then add `CREATE STATISTICS` and `ANALYZE` and show the plan changes
   to a hash join. Done when you can show both plans and the estimate before/after.
5. **(hard)** Write the five-statement login path as five separate `psql` statements and time
   it with `\timing` over 100 iterations in a shell loop; then as one PL/pgSQL function call
   (module 08 has the function). Done when you can explain the difference in terms of round
   trips, planning, and buffer hits.
6. **(hard)** Set `random_page_cost = 1.1` and re-run all pairs from the demo file. Note any
   plan that changed. Done when you can argue for or against the change on your hardware with
   `track_io_timing` numbers.

## 7. Interview questions

**Q: How does the PostgreSQL planner choose a plan?**
A: It builds candidate access paths for each table (sequential, index, bitmap) using per-column
statistics (MCVs, histograms, n_distinct) to estimate selectivity, then combines tables with
nested loop, hash and merge joins, exploring join orders by dynamic programming up to about 8
tables. Each path gets a cost from constants such as seq_page_cost, random_page_cost and
cpu_tuple_cost; the cheapest estimated total (or startup, under LIMIT) wins.

**Q: How do you read an EXPLAIN ANALYZE plan?**
A: Innermost nodes run first and feed their parents. For each node compare estimated rows with
actual rows times loops; a large divergence is the planner's mistake. Look at where actual time
concentrates, at Buffers read versus hit for I/O, at Filter rows removed for wasted work, at
Sort Method and Hash Batches for memory spills, and at Heap Fetches for index-only scans.

**Q: Nested loop versus hash join versus merge join: when is each right?**
A: Nested loop when the outer side is small and the inner has an index on the join key; it is
also the only option for non-equality joins. Hash join for equality joins of larger sets when the
smaller side's hash table fits in work_mem. Merge join when both inputs are already sorted on
the join key or when the result is needed sorted and the sets are too big to hash.

**Q: Your index exists but the planner does a sequential scan. Why?**
A: Either the predicate is not selective enough for random heap reads to beat a sequential read,
which is the correct choice; or the estimate is wrong because statistics are stale or columns are
correlated; or the predicate is not sargable: a function or implicit cast wraps the column, or
a leading-wildcard LIKE. Check estimated versus actual rows to tell which.

**Q: What is parameter sniffing and how does PostgreSQL handle it?**
A: A plan built for one parameter value being reused for values with very different
selectivity. PostgreSQL plans prepared statements with custom plans for the first five
executions, then switches to a generic plan if its estimated cost is not worse than the custom
average. For skewed columns that can pick the wrong plan; plan_cache_mode = force_custom_plan or
rewriting the query avoids it.

**Q: Describe how you fixed the slow login query.**
A: EXPLAIN showed a parallel sequential scan with a correct row estimate of 1, so statistics were
fine and the issue was the missing access path: the lookup was by natural key with only a
surrogate primary key indexed. I added a unique composite index on the natural key, then made it
covering with INCLUDE so the plan became an Index Only Scan with zero heap fetches and four buffer
hits, and finally collapsed the login's five statements into one function call to remove round
trips. On the cluster the index also confined the read to one tablet.

**Q: What does `work_mem` affect and why is the default so low?**
A: It is the memory budget per sort or hash node before spilling to temp files, and it also
influences plan choice because an in-memory hash is cheaper than a batched one. It is per node
per query per worker, so a high global value multiplied by concurrency can exhaust RAM; set it
per session or per statement for the queries that need it.

## 8. Recap

- The planner estimates, it does not know; estimates come from `pg_statistic` sampled by ANALYZE.
- Selectivity: MCV frequency for common values, `1/n_distinct` otherwise, histogram for ranges; AND multiplies (independence).
- Costs are dimensionless: seq_page 1, random_page 4 (lower it on SSD), cpu_tuple 0.01; LIMIT prefers low startup cost.
- Joins: nested loop (small outer + indexed inner), hash (equality, fits work_mem), merge (pre-sorted inputs).
- Read EXPLAIN inside-out; compare estimated vs actual rows x loops; follow the time and the `read` buffers.
- Filter vs Index Cond; Rows Removed by Filter; Heap Fetches; Sort Method; Batches; Recheck Cond.
- Non-sargable predicates (functions, casts, leading wildcards) disable indexes.
- Prepared statements: 5 custom plans, then possibly generic; `plan_cache_mode` controls it.
- Login case study: Seq Scan 170 ms -> Index Only Scan 0.02 ms -> one round trip via a function.
- `pg_stat_statements` finds what to fix; `auto_explain` catches regressions.
