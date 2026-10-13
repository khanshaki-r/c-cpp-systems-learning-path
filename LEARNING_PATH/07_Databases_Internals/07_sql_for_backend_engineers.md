# 7. SQL for backend engineers

**Time:** ~3 hours   **Prerequisites:** 03, 06

## 1. In plain words

Most application code treats the database as a slow hash map: fetch one record, look at it,
fetch another, loop. That is how you had to work with Berkeley DB, because a cursor was the only
tool. SQL is a different tool: you describe the whole result you want (a set of rows) and the
engine computes it in one pass, inside the server, next to the data, with a plan chosen for the
whole job. One statement that joins, filters and aggregates 50,000 rows is almost always faster
than 50,000 round trips doing the same work, and it is often simpler to read.

The analogy: asking a librarian "bring me every overdue book borrowed by members from
branch 12, grouped by member" versus walking the shelves yourself, pulling each book, checking
the card, walking to the members' file for each. The librarian knows the layout and does it in
one trip.

This module is the practical SQL a senior backend engineer is expected to write without looking
things up: joins and their traps, CTEs, window functions, upserts, bulk loading, pagination,
and the anti-patterns that make interviewers wince.

## 2. The real thing

### 2.1 Joins, precisely

| Join | Returns | Rows when no match |
|---|---|---|
| `INNER JOIN` | pairs that satisfy the condition | row dropped |
| `LEFT [OUTER] JOIN` | every left row, matched right columns or NULL | left kept with NULLs |
| `RIGHT JOIN` | mirror of LEFT (rewrite as LEFT; nobody reads RIGHT) | |
| `FULL OUTER JOIN` | all rows from both, NULL-padded | both kept |
| `CROSS JOIN` | Cartesian product | |
| `LATERAL` | the right side can reference the left row (a correlated subquery as a join) | depends on inner |
| semi-join (`WHERE EXISTS (...)`, `IN (subquery)`) | left rows that have at least one match, never duplicated | |
| anti-join (`WHERE NOT EXISTS (...)`) | left rows with no match | |

Traps:

- **Filtering on the right table of a LEFT JOIN in WHERE turns it into an INNER JOIN.**
  `LEFT JOIN documents d ON d.application_id = a.id WHERE d.kind = 'ID_PROOF'` drops applications
  with no documents (their `d.kind` is NULL, and `NULL = 'ID_PROOF'` is not true). Put the
  condition in the `ON` clause, or test `d.id IS NULL` for anti-join semantics.
- **Fan-out.** Joining a parent to two child tables (documents and payments) multiplies rows:
  3 documents x 4 payments = 12 rows per application, and `SUM(payment.amount)` is tripled.
  Aggregate each child in a subquery first, then join the aggregates.
- **`NOT IN (subquery)` with NULLs** returns no rows if the subquery can produce a NULL. Use
  `NOT EXISTS`.
- **`DISTINCT` to fix duplicates** is a smell: it hides a fan-out or a missing join condition and
  forces a sort or hash of the whole result.

LATERAL is worth knowing by name: "for each customer, their latest 3 applications":

```sql
SELECT c.id, c.full_name, la.application_no, la.submitted_at
FROM customers c
CROSS JOIN LATERAL (
    SELECT application_no, submitted_at
    FROM loan_applications a
    WHERE a.customer_id = c.id
    ORDER BY submitted_at DESC
    LIMIT 3
) la;
-- plan: Nested Loop with an Index Scan Backward on (customer_id, submitted_at) per customer
```

### 2.2 CTEs (WITH) and when they are fences

A common table expression names a subquery so you can read the statement top-down. Since
PostgreSQL 12 a CTE referenced once and without side effects is **inlined** into the main query
(the planner can push predicates into it); a CTE referenced more than once, or written `AS
MATERIALIZED`, or containing INSERT/UPDATE/DELETE, is computed once into a tuplestore (an
"optimisation fence"). `AS NOT MATERIALIZED` forces inlining.

```sql
WITH recent AS (
    SELECT customer_id, count(*) AS n
    FROM loan_applications
    WHERE submitted_at > now() - interval '30 days'
    GROUP BY customer_id
)
SELECT c.full_name, r.n
FROM recent r JOIN customers c ON c.id = r.customer_id
WHERE r.n >= 3;
```

**Recursive CTEs** walk hierarchies (an org chart, a workflow state graph, bill of materials):

```sql
WITH RECURSIVE chain AS (
    SELECT id, from_state, to_state, 1 AS depth FROM workflow_transitions WHERE from_state = 'DRAFT'
    UNION ALL
    SELECT t.id, t.from_state, t.to_state, chain.depth + 1
    FROM workflow_transitions t JOIN chain ON t.from_state = chain.to_state
    WHERE chain.depth < 20            -- always bound recursion
)
SELECT * FROM chain;
```

**Data-modifying CTEs** let one statement do several things atomically and pass rows along:

```sql
WITH moved AS (
    DELETE FROM loan_applications WHERE status = 'WITHDRAWN' AND updated_at < now() - interval '2 years'
    RETURNING *
)
INSERT INTO loan_applications_archive SELECT * FROM moved;
```

### 2.3 Window functions

A window function computes a value over a set of rows *related to the current row* without
collapsing them the way GROUP BY does. Syntax: `fn(...) OVER (PARTITION BY ... ORDER BY ...
[frame])`.

```sql
-- running balance per account, row number per customer, and gap to the previous application
SELECT a.customer_id, a.application_no, a.submitted_at, a.requested_amount,
       row_number() OVER w                                   AS nth_for_customer,
       sum(a.requested_amount) OVER (w ROWS UNBOUNDED PRECEDING) AS running_total,
       lag(a.submitted_at) OVER w                            AS previous_submitted_at,
       a.requested_amount - avg(a.requested_amount) OVER (PARTITION BY a.customer_id) AS vs_customer_avg
FROM loan_applications a
WINDOW w AS (PARTITION BY a.customer_id ORDER BY a.submitted_at);
```

Common uses: `row_number()` to pick "the latest per group" (then filter `= 1` in an outer query;
`DISTINCT ON (customer_id) ... ORDER BY customer_id, submitted_at DESC` is the shorter PostgreSQL
idiom), `rank()`/`dense_rank()` for leaderboards, `lag()`/`lead()` for deltas between events,
`sum() OVER (ORDER BY ...)` for running totals, `ntile()` for buckets. Frames: `ROWS BETWEEN n
PRECEDING AND CURRENT ROW` (moving average), the default frame for `ORDER BY` is `RANGE
UNBOUNDED PRECEDING` which includes peers with equal sort keys (surprising for running totals
with ties; use `ROWS`).

Window functions are evaluated after WHERE/GROUP BY/HAVING and before ORDER BY/LIMIT; you cannot
filter on them in the same SELECT, hence the outer query.

### 2.4 Upserts

```sql
INSERT INTO customers (national_id, full_name, email)
VALUES ($1, $2, $3)
ON CONFLICT (national_id) DO UPDATE
    SET full_name = EXCLUDED.full_name,
        email     = EXCLUDED.email,
        updated_at = now()
    WHERE customers.email IS DISTINCT FROM EXCLUDED.email      -- skip no-op updates (saves WAL and HOT chains)
RETURNING id, (xmax = 0) AS inserted;                           -- xmax = 0 means the row was freshly inserted
```

`ON CONFLICT` needs a unique index or constraint matching the conflict target (an arbiter);
`DO NOTHING` is the idempotent-insert form your migration engine used for restartability (module
12). `MERGE` (PostgreSQL 15+) is the standard-SQL form, supports WHEN MATCHED / NOT MATCHED with
DELETE, and does not require a unique index, but it is **not** safe against concurrent inserts
the way `ON CONFLICT` is (it can raise a unique violation under a race).

### 2.5 Set-based thinking versus row-at-a-time cursors

Your resume says you moved the application from record-level cursor access to set-based SQL.
Here is the shape of that change, so you can show it:

```c
/* Berkeley DB era: for each application in branch X, fetch customer, fetch documents, decide. */
for (ret = cur->get(cur, &key, &val, DB_SET_RANGE); ret == 0; ret = cur->get(cur, &key, &val, DB_NEXT)) {
    app = decode_app(&val);
    if (app.branch != X) break;
    cust = fetch_customer(app.customer_id);          /* one more lookup */
    ndocs = count_documents(app.id);                 /* a cursor over another DB */
    if (cust.kyc_ok && ndocs >= 2) mark_ready(app.id);   /* one write */
}
```

Ported literally to SQL, that becomes a loop in the application with 3 statements per row:
N applications -> 3N round trips. The set-based version is one statement:

```sql
UPDATE loan_applications a
SET status = 'READY_FOR_REVIEW', updated_at = now()
FROM customers c
WHERE c.id = a.customer_id
  AND a.branch_code = $1
  AND a.status = 'SUBMITTED'
  AND c.kyc_verified
  AND (SELECT count(*) FROM documents d WHERE d.application_id = a.id) >= 2;
```

One round trip, one plan, one transaction, and the engine chooses hash joins over the three
tables instead of 3N index probes. The rules of thumb for the translation:

| Row-at-a-time habit | Set-based replacement |
|---|---|
| loop, SELECT by key, decide in code, UPDATE | one `UPDATE ... FROM ... WHERE` with the decision in the WHERE |
| loop, SELECT child count per parent | `JOIN (SELECT parent_id, count(*) ... GROUP BY parent_id)` or `LATERAL` |
| loop, INSERT one row | multi-row `INSERT ... VALUES (...), (...), ...` or `INSERT ... SELECT` or `COPY` |
| loop, "if exists then update else insert" | `INSERT ... ON CONFLICT DO UPDATE` |
| loop, running total in a variable | `sum() OVER (ORDER BY ...)` |
| loop, "latest row per group" | `DISTINCT ON` or `row_number() OVER ... = 1` |
| temp arrays of ids, then `WHERE id = ?` per id | `WHERE id = ANY($1::bigint[])` with one array parameter, or a `VALUES` list joined |

When a cursor is still right: processing that must call an external system per row (send an
email, call a payment gateway), or per-row logic too complex for SQL, or when you must bound
memory on the client and the result is huge (use a server-side cursor: `DECLARE c CURSOR FOR
...; FETCH 1000 FROM c`, which streams instead of materialising the whole result in the client).
Even then, fetch in batches of hundreds, not one.

### 2.6 Batch inserts and COPY

Cost per row, roughly, from cheapest to most expensive: `COPY` (binary or text, one stream, no
per-row parsing of SQL) < multi-row `INSERT ... VALUES` with hundreds of rows per statement <
prepared single-row `INSERT` in one transaction < single-row autocommit `INSERT` (one fsync per
row). On a laptop SSD, autocommit single rows reach a few hundred to a couple of thousand rows
per second; `COPY` reaches hundreds of thousands (typical, not measured here).

```sql
-- From a file on the server (needs superuser or pg_read_server_files)
COPY loan_applications (application_no, customer_id, requested_amount, status)
FROM '/tmp/apps.csv' WITH (FORMAT csv, HEADER true);

-- From the client: psql's \copy streams the client-side file through the connection
\copy loan_applications (application_no, customer_id, requested_amount, status) FROM 'apps.csv' CSV HEADER

-- In libpq: PQputCopyData / PQputCopyEnd. Your migration engine's sink (module 12) should use this.
```

Bulk-load checklist: load into an unindexed staging table (or drop secondary indexes and recreate
after), use `COPY`, run in few large transactions (one per 10k to 100k rows for restartability),
disable triggers if you can prove the data is already valid, `ANALYZE` afterwards, `VACUUM` to set
visibility bits. `COPY ... FREEZE` on a table created or truncated in the same transaction
writes tuples pre-frozen with hint bits set.

### 2.7 Pagination

```sql
-- OFFSET pagination: page 500 reads and discards 10,000 rows. O(page) per request, and
-- rows shift between pages when data changes.
SELECT * FROM loan_applications ORDER BY submitted_at DESC, id DESC LIMIT 20 OFFSET 9980;

-- Keyset (cursor) pagination: remember the last row's sort key, ask for what comes after.
-- O(log n) with an index on (submitted_at, id); stable under inserts.
SELECT * FROM loan_applications
WHERE (submitted_at, id) < ($1, $2)              -- row comparison: compares lexicographically
ORDER BY submitted_at DESC, id DESC
LIMIT 20;
```

The tiebreaker column (`id`) is mandatory: without it, equal timestamps make pages overlap or
skip. The API returns an opaque cursor token encoding `(submitted_at, id)`. Downside: no "jump
to page 37"; for that, OFFSET on a bounded range is acceptable, or a precomputed page index.

### 2.8 Anti-patterns

- **N+1 queries**: one query for a list, then one per row for details. Replace with a join, a
  `WHERE id = ANY($1)` batch fetch, or LATERAL. Detect it in `pg_stat_statements` as a
  single-row statement with an enormous `calls` count.
- **`SELECT *`**: fetches TOASTed columns you do not use, breaks index-only scans, couples code
  to column order, and makes `INSERT INTO ... SELECT *` fragile. Name the columns.
- **Implicit casts that break indexes**: comparing a `text` column to an integer parameter
  (`WHERE user_code = 123456`) or a `varchar` to a `bigint` makes the planner cast the *column*
  (or error). Bind parameters with the column's type; in libpq pass `paramTypes` or use
  `$1::text` in the SQL. Likewise `timestamptz` vs `timestamp`, `numeric` vs `int`, and
  `date_trunc('day', created_at) = $1` (function on the column) instead of
  `created_at >= $1 AND created_at < $1 + interval '1 day'`.
- **Functions on indexed columns**: `WHERE lower(email) = ...`, `WHERE created_at::date = ...`.
  Either an expression index or a rewritten range predicate.
- **Leading wildcard LIKE** `'%smith'`: no B-tree can help; `pg_trgm` GIN index or full-text.
- **`OR` across columns**: often a Seq Scan; rewrite as `UNION ALL` of two indexable queries.
- **`count(*)` for "exists"**: `SELECT EXISTS (SELECT 1 FROM ... WHERE ...)` stops at the first row.
- **`ORDER BY random()`** on a big table sorts everything; use `TABLESAMPLE` or a random key range.
- **Unbounded `IN (...)` lists** of thousands of literals: huge parse cost and plans; pass an
  array parameter (`= ANY($1)`) or join a `VALUES` list / temp table.
- **Doing math on money in floating point**: `numeric(18,2)`, never `double precision`.
- **Storing dates as text or epoch integers**: `timestamptz` (module 09).
- **Long transactions around user interaction**: see module 05.

## 3. Code

`code/m07_sql_patterns.sql` has runnable versions of everything above against the loan schema,
each with a comment on the expected plan. Highlights:

```sql
-- Latest application per customer, two idioms, same plan shape
SELECT DISTINCT ON (customer_id) customer_id, application_no, submitted_at
FROM loan_applications
ORDER BY customer_id, submitted_at DESC;

SELECT customer_id, application_no, submitted_at
FROM (SELECT *, row_number() OVER (PARTITION BY customer_id ORDER BY submitted_at DESC) AS rn
      FROM loan_applications) x
WHERE rn = 1;

-- Batch fetch instead of N+1: one statement, one array parameter
PREPARE fetch_apps(bigint[]) AS
SELECT id, application_no, status FROM loan_applications WHERE id = ANY($1);
EXECUTE fetch_apps(ARRAY[1,2,3,4,5]);
-- plan: Index Scan on pkey with Index Cond: (id = ANY ('{1,2,3,4,5}'))

-- Fan-out trap and its fix
-- WRONG: documents x payments multiplies sums
SELECT a.id, count(d.id) AS docs, sum(p.amount) AS paid
FROM loan_applications a
LEFT JOIN documents d ON d.application_id = a.id
LEFT JOIN payments  p ON p.application_id = a.id
GROUP BY a.id;
-- RIGHT: aggregate each child separately
SELECT a.id, coalesce(d.docs, 0) AS docs, coalesce(p.paid, 0) AS paid
FROM loan_applications a
LEFT JOIN (SELECT application_id, count(*) AS docs FROM documents GROUP BY application_id) d ON d.application_id = a.id
LEFT JOIN (SELECT application_id, sum(amount) AS paid FROM payments GROUP BY application_id) p ON p.application_id = a.id;

-- Row comparison for keyset pagination uses the composite index
EXPLAIN SELECT id, submitted_at FROM loan_applications
WHERE (submitted_at, id) < ('2026-09-01 00:00+00', 500000)
ORDER BY submitted_at DESC, id DESC LIMIT 20;
-- plan: Limit -> Index Scan Backward using la_submitted_id_idx, Index Cond: (ROW(submitted_at, id) < ROW(...))

-- Set-based UPDATE replacing a cursor loop
EXPLAIN (ANALYZE)
UPDATE loan_applications a
SET status = 'READY_FOR_REVIEW', updated_at = now()
FROM customers c
WHERE c.id = a.customer_id AND a.branch_code = 'BR0001' AND a.status = 'SUBMITTED'
  AND c.kyc_verified
  AND (SELECT count(*) FROM documents d WHERE d.application_id = a.id) >= 2;
```

And the libpq side of `COPY`, which is what a migration sink should look like:

```c
/* Stream rows into the server without per-row SQL. Text format: tab-separated, \N for NULL. */
PGresult *r = PQexec(conn, "COPY loan_applications (application_no, customer_id, requested_amount, status) FROM STDIN");
if (PQresultStatus(r) != PGRES_COPY_IN) { /* error */ }
PQclear(r);
for (each record) {
    char line[512];
    int n = snprintf(line, sizeof line, "%s\t%lld\t%.2f\t%s\n", app_no, (long long)cust_id, amount, status);
    if (PQputCopyData(conn, line, n) != 1) { /* error */ }
}
if (PQputCopyEnd(conn, NULL) != 1) { /* error */ }
r = PQgetResult(conn);                       /* COPY's final status; check PGRES_COMMAND_OK */
```

## 4. Under the hood (senior-level)

**Why set-based wins: the executor runs once.** A single statement is parsed, planned, and
executed with one pass of the Volcano iterator over a plan optimised for the whole workload: a
hash join reads each table once; a nested loop with 50,000 outer rows probes an index 50,000
times but inside one process with hot caches, no protocol framing, no syscalls per row. The
application loop pays per row: protocol encode/decode, two syscalls, a context switch, a
snapshot (`GetSnapshotData`), planning or plan-cache lookup, and executor start/stop. At 50 us a
round trip that is 2.5 s for 50,000 rows before any real work.

**`= ANY(array)` and arrays as parameters.** The planner turns `col = ANY($1)` into an index
scan with a `ScalarArrayOpExpr`, probing the B-tree once per array element in sorted order (since
PostgreSQL 9.2 for B-tree; 17 improved it further). Thousands of elements are fine; the cost is
linear in elements and logarithmic in table size.

**How DISTINCT ON executes.** Sort (or Index Scan) on the DISTINCT ON columns plus the ORDER BY
columns, then a `Unique` node that emits the first row of each group. With an index on
`(customer_id, submitted_at DESC)` there is no sort at all.

**Window functions execute as `WindowAgg`** over input sorted by PARTITION BY then ORDER BY. All
windows with the same partition and order share one sort; windows with different ordering need a
separate sort each (you will see stacked `WindowAgg -> Sort` nodes). Frame evaluation for
`ROWS BETWEEN` is incremental; for `RANGE` with peers the engine buffers each peer group.

**`INSERT ... ON CONFLICT` internals**: speculative insertion (module 03). The heap tuple is
inserted with a speculative token, the unique index is probed; on conflict the tuple is
"super-deleted" and the `DO UPDATE` path locks and updates the conflicting row with the same
EvalPlanQual re-check as a normal UPDATE. Under READ COMMITTED it is race-free; two sessions
upserting the same key serialise correctly.

**COPY's fast path.** `COPY FROM` parses the input in bulk, forms tuples, and uses
`heap_multi_insert` to place many tuples on a page under one buffer lock with one WAL record per
page (instead of one per row). With `FREEZE` it writes them with hint bits and frozen flags set.
This is why COPY is an order of magnitude cheaper than inserts even inside one transaction.

**Row comparison** `(a, b) < (x, y)` is true lexicographic comparison (`a < x OR (a = x AND b <
y)`), and the B-tree AM understands it directly as a boundary condition on a multi-column
index, so keyset pagination is a single index descent to the boundary plus a walk.

**Cursors on the server.** `DECLARE ... CURSOR` plans with `cursor_tuple_fraction` (0.1) biasing
toward fast-start plans, keeps the executor state open, and `FETCH` pulls N rows at a time
through the protocol. The transaction stays open while the cursor exists (`WITH HOLD` copies the
result to a tuplestore so the transaction can end). This is the right tool for streaming a 50 M
row table out of PostgreSQL without 50 M round trips or 50 GB in client RAM.

## 5. In production

- **The N+1 you cannot see** hides in ORMs and in "helper" functions that fetch one entity.
  `pg_stat_statements` with `calls` in the millions for a `WHERE id = $1` statement is the
  signature. Batch the ids.
- **Big UPDATE statements and locks.** A set-based `UPDATE` of 10 M rows is one transaction
  holding 10 M row locks and generating GBs of WAL, blocking everyone touching those rows until
  it commits. Batch by key range (`WHERE id BETWEEN $1 AND $2`, 10k to 50k rows per
  transaction), commit each, and let autovacuum keep up. The same applies to backfills in
  migrations (module 12).
- **COPY failure semantics**: one bad row aborts the whole COPY (no partial load; it is one
  statement). Load into a staging table with text columns, validate with SQL, then
  `INSERT ... SELECT` with casts into the real table. Tooling like `pg_bulkload` or the 17+
  `ON_ERROR` option can skip rows, but staging is the robust approach.
- **Implicit cast outages are deployment outages.** A driver upgrade changed how it binds
  numeric strings; the login index stopped matching; CPU hit 100%. The plan in `auto_explain`
  shows `Filter: ((user_code)::bigint = $1)`. Fix the binding, not the index.
- **Keyset pagination and API design**: the token must be opaque and must encode all sort keys;
  changing the sort order means a new token format. Tell product that "jump to page N" costs
  O(N) and let them choose.
- **Your PL/pgSQL layer** (module 08) is where set-based thinking pays most: a procedure that
  loops `FOR r IN SELECT ... LOOP UPDATE ... END LOOP` is a cursor loop with extra steps; write
  the single UPDATE.

## 6. Try it

1. **(easy)** Write the LEFT JOIN trap and its fix on `loan_applications` and `documents`.
   Done when you can show the row counts differ and explain why in terms of NULL comparison.
2. **(easy)** Write "latest application per customer" both ways and compare plans with an index
   on `(customer_id, submitted_at DESC)`. Done when neither plan has a Sort node.
3. **(medium)** Implement keyset pagination over `loan_applications` ordered by
   `(submitted_at DESC, id DESC)` and walk 10 pages in psql using the last row of each page as
   the next cursor. Insert a new row in between pages and show no row is skipped or duplicated.
   Done when the plan is an Index Scan Backward with a row-comparison Index Cond.
4. **(medium)** Load 1 M rows into a staging table three ways: autocommit single-row inserts from
   a shell loop (stop after 10k and extrapolate), multi-row INSERT of 1000 rows per statement,
   and `\copy`. Done when you have three rates and can explain each in terms of round trips,
   parsing, fsyncs and `heap_multi_insert`.
5. **(hard)** Take the Berkeley DB style loop in 2.5 and write it as (a) a PL/pgSQL `FOR` loop
   with three statements per row and (b) a single set-based UPDATE. Time both on 100k
   applications with `\timing`. Done when you have both numbers and can explain the ratio.
6. **(hard)** Write a recursive CTE over `workflow_transitions` that lists every reachable state
   from `DRAFT` with the shortest path length, and prove with a cycle in the data that your
   depth bound stops it. Done when it returns correct depths and terminates.

## 7. Interview questions

**Q: What is the difference between putting a condition in ON versus WHERE for a LEFT JOIN?**
A: In ON, the condition decides which right rows match; unmatched left rows survive with NULLs.
In WHERE, the condition filters the joined result, and since unmatched rows have NULL in right
columns they fail the test, turning the LEFT JOIN into an INNER JOIN. Use ON for match
conditions and `right.id IS NULL` in WHERE for anti-joins.

**Q: What are window functions and when do you use them instead of GROUP BY?**
A: They compute an aggregate or ranking over a window of rows related to the current row while
keeping every row in the output, whereas GROUP BY collapses rows. Use them for running totals,
rankings, "latest per group", and comparing a row to its group's average or to the previous row
(lag/lead).

**Q: How do you implement an upsert safely under concurrency?**
A: `INSERT ... ON CONFLICT (unique_cols) DO UPDATE SET ...`, which requires a unique index as
arbiter and uses speculative insertion so two concurrent sessions serialise correctly. MERGE is
standard SQL and more flexible but can raise a unique violation under a concurrent insert race.

**Q: Explain "set-based versus row-at-a-time" and why it mattered in your migration.**
A: The Berkeley DB application iterated records with cursors and did one lookup per related
record, which ported to SQL as N round trips of three statements each. Set-based SQL expresses
the whole operation as one UPDATE with joins and subqueries; the engine plans it once, uses hash
joins, and does it in one round trip and one transaction. Typical speedups are one to two orders
of magnitude, and the code shrinks.

**Q: Why is OFFSET pagination slow and what is the alternative?**
A: OFFSET makes the executor produce and discard all preceding rows, so cost grows linearly with
page number, and concurrent inserts shift rows between pages. Keyset pagination uses the last
seen sort key in a row comparison, `WHERE (ts, id) < ($1, $2) ORDER BY ts DESC, id DESC LIMIT n`,
which an index satisfies in logarithmic time and which is stable under inserts.

**Q: Fastest way to load 10 million rows?**
A: COPY, into a table without secondary indexes, in a few large transactions, with triggers off
if the data is validated, then build indexes, ANALYZE and VACUUM. COPY avoids per-row SQL parsing
and uses multi-insert with one WAL record per page; with FREEZE on a freshly created table it also
skips later hint-bit and freeze work.

**Q: Name three ways a query can fail to use an existing index.**
A: A function or implicit cast applied to the indexed column, a predicate on a non-leading index
column, and a leading-wildcard LIKE. Also low selectivity, where the planner correctly prefers a
sequential scan.

## 8. Recap

- LEFT JOIN filter in WHERE becomes INNER JOIN; aggregate children separately to avoid fan-out; `NOT EXISTS` over `NOT IN`.
- CTEs: inlined when referenced once (12+); `MATERIALIZED` is a fence; recursive CTEs need a depth bound.
- Window functions keep rows and add per-partition computations; filter them in an outer query.
- `INSERT ... ON CONFLICT` is the race-safe upsert; `DO NOTHING` gives idempotent inserts.
- Set-based: one UPDATE ... FROM replaces a loop of SELECT/decide/UPDATE; one array parameter replaces N lookups.
- COPY > multi-row INSERT > prepared INSERT in a transaction > autocommit INSERT.
- Keyset pagination with a tiebreaker column and a row comparison; OFFSET is O(page).
- Anti-patterns: N+1, SELECT *, implicit casts and functions on columns, leading wildcards, OR across columns, floats for money.
- Batch big UPDATEs by key range to limit locks and WAL.
- Server-side cursors stream huge results; still fetch in batches.
