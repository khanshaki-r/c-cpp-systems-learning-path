-- m06_slow_vs_fast_queries.sql : eight slow/fast pairs on the loan schema, with the plan shape
-- to expect in comments (module 06).
--
-- Run:  psql loans -f m06_slow_vs_fast_queries.sql > m06_plans.txt     then read the file
-- Better: paste pair by pair. Numbers (ms, buffers) depend on your machine and cache state; the
-- plan SHAPES are what to compare. Run a pair twice: the first run may read from disk.
--
-- How to read every plan below (bottom-up, innermost first):
--   cost=startup..total   planner's estimate in abstract units (seq_page_cost = 1)
--   rows=                 estimated rows; compare with "actual ... rows=" to find bad estimates
--   actual time=a..b      ms to first row .. ms to last row, PER LOOP; multiply by loops=
--   Buffers: shared hit   8 KB pages found in shared_buffers; read = from the OS (page cache or disk)
--   Rows Removed by Filter: rows read and thrown away: the classic sign of a missing index

\set ON_ERROR_STOP on
\pset pager off
SET search_path = loans, public;
SET max_parallel_workers_per_gather = 0;   -- simpler plans to read; remove to see Gather nodes

-- ===========================================================================================
\echo '=== Pair 1: a function on the column vs an expression index'
-- ===========================================================================================
DROP INDEX IF EXISTS customers_email_lower_idx;
EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM customers WHERE lower(email) = 'anna.k@example.com';
-- SLOW: Seq Scan on customers, Filter: (lower(email) = ...), Rows Removed by Filter: 199999.
-- Why not customers_email_live_key? It IS on lower(email), but it is PARTIAL
-- (WHERE deleted_at IS NULL) and the query does not imply that predicate.
EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM customers
WHERE lower(email) = 'anna.k@example.com' AND deleted_at IS NULL;
-- FAST: Index Scan using customers_email_live_key: the query now implies the index predicate.
CREATE INDEX customers_email_lower_idx ON customers (lower(email));
EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM customers WHERE lower(email) = 'anna.k@example.com';
-- FAST: Index Scan using customers_email_lower_idx. Lesson: the indexed EXPRESSION must match
-- the query expression, and a partial index must be implied by the WHERE clause.
DROP INDEX customers_email_lower_idx;      -- the live view + partial index is the intended design

-- ===========================================================================================
\echo '=== Pair 2: work queue ORDER BY ... LIMIT with and without a matching index'
-- ===========================================================================================
DROP INDEX IF EXISTS la_status_submitted_idx;
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, application_no FROM loan_applications
WHERE status = 'SUBMITTED' ORDER BY submitted_at LIMIT 50;
-- SLOW: no index starts with status (la_branch_status_idx has it SECOND). Typical plan:
-- Limit -> Index Scan using la_submitted_id_idx, walking ALL applications in submitted_at order
-- and discarding non-SUBMITTED ones (look at Rows Removed by Filter), or
-- Limit -> Sort (top-N heapsort) -> Seq Scan reading every row. Either way: far more rows than 50.
CREATE INDEX la_status_submitted_idx ON loan_applications (status, submitted_at);
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, application_no FROM loan_applications
WHERE status = 'SUBMITTED' ORDER BY submitted_at LIMIT 50;
-- FAST: Limit -> Index Scan using la_status_submitted_idx. No Sort node: the index already
-- returns rows in submitted_at order within status = 'SUBMITTED', and the scan stops after 50.

-- ===========================================================================================
\echo '=== Pair 3: a hash join that spills to disk, and the work_mem fix'
-- ===========================================================================================
-- With indexes on both join keys and little work_mem, the planner avoids the hash join altogether
-- (Merge Join over two index scans, or a Nested Loop of index probes). Turn those off to isolate
-- the hash join's memory behaviour; noticing that the planner routes around a small work_mem is
-- itself part of the lesson.
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET work_mem = '256kB';
EXPLAIN (ANALYZE, BUFFERS)
SELECT c.id, count(*) FROM customers c JOIN loan_applications a ON a.customer_id = c.id GROUP BY c.id;
-- SLOW: Hash Join ... Hash: Buckets ... Batches: 16 (or more) Memory Usage: ~256kB,
-- and "temp read=... written=..." in Buffers: the hash table did not fit, so both inputs were
-- partitioned to temp files. The aggregate may spill too ("Disk Usage" on HashAggregate).
SET work_mem = '64MB';
EXPLAIN (ANALYZE, BUFFERS)
SELECT c.id, count(*) FROM customers c JOIN loan_applications a ON a.customer_id = c.id GROUP BY c.id;
-- FAST: Batches: 1, no temp buffers. work_mem is PER SORT/HASH NODE PER BACKEND: 64MB x 200
-- connections x 3 nodes is how servers run out of memory. Raise it per session or per role.
RESET work_mem;
RESET enable_mergejoin;
RESET enable_nestloop;

-- ===========================================================================================
\echo '=== Pair 4: a non-sargable date predicate vs a range'
-- ===========================================================================================
EXPLAIN (ANALYZE, BUFFERS)
SELECT count(*) FROM loan_applications
WHERE date_trunc('day', submitted_at AT TIME ZONE 'UTC') = DATE '2024-03-15';
-- SLOW: Seq Scan, Filter on date_trunc(...): the function hides the column from every index.
EXPLAIN (ANALYZE, BUFFERS)
SELECT count(*) FROM loan_applications
WHERE submitted_at >= TIMESTAMPTZ '2024-03-15 00:00+00' AND submitted_at < TIMESTAMPTZ '2024-03-16 00:00+00';
-- FAST: Index Only Scan using la_submitted_id_idx, Index Cond on the range. Rewrite predicates
-- so the bare column is compared with constants ("sargable": Search ARGument ABLE).

-- ===========================================================================================
\echo '=== Pair 5: NOT IN (subquery) vs NOT EXISTS'
-- ===========================================================================================
EXPLAIN (ANALYZE, BUFFERS)
SELECT count(*) FROM customers c
WHERE c.id NOT IN (SELECT a.customer_id FROM loan_applications a WHERE a.status = 'DISBURSED');
-- SLOW-ish and semantically dangerous: Filter (NOT (hashed SubPlan 1)). If the subquery result
-- does not fit in work_mem the plan degrades to a plain SubPlan rescanned per outer row. And if
-- the subquery ever returns a NULL, NOT IN returns no rows at all (x NOT IN (1, NULL) is NULL).
EXPLAIN (ANALYZE, BUFFERS)
SELECT count(*) FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM loan_applications a WHERE a.customer_id = c.id AND a.status = 'DISBURSED');
-- FAST and correct: Hash Anti Join (or Merge Anti Join). NOT EXISTS is NULL-safe and lets the
-- planner choose any anti-join method.

-- ===========================================================================================
\echo '=== Pair 6: OFFSET pagination vs keyset pagination'
-- ===========================================================================================
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, submitted_at FROM loan_applications
ORDER BY submitted_at DESC, id DESC OFFSET 500000 LIMIT 20;
-- SLOW: Limit -> Index Scan Backward ... actual rows=500020: the engine produces and discards
-- 500,000 rows to return 20. Page N costs O(N). (NULL submitted_at sort first in DESC: drafts.)
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, submitted_at FROM loan_applications
WHERE (submitted_at, id) < (TIMESTAMPTZ '2023-06-01 00:00+00', 500000)
ORDER BY submitted_at DESC, id DESC LIMIT 20;
-- FAST: Index Scan Backward using la_submitted_id_idx, Index Cond: (ROW(submitted_at, id) < ROW(...)),
-- 20 rows read. The client passes the last row's (submitted_at, id) as the cursor for the next page.

-- ===========================================================================================
\echo '=== Pair 7: correlated columns -> bad row estimate -> extended statistics'
-- ===========================================================================================
DROP STATISTICS IF EXISTS la_status_submitted_stats;
EXPLAIN (ANALYZE)
SELECT * FROM loan_applications WHERE status = 'DRAFT' AND updated_by IS NULL;
-- ESTIMATE WRONG: the planner multiplies selectivities as if independent:
-- P(DRAFT) x P(updated_by IS NULL) = 0.03 x 0.03 -> rows=~800 to 900, actual rows=~30,000.
-- Every draft has updated_by NULL (and vice versa), so the true combined selectivity is 0.03.
-- A 30x underestimate here is harmless; inside a join it is how nested loops over 30,000 rows happen.
CREATE STATISTICS la_status_submitted_stats (mcv) ON status, updated_by FROM loan_applications;
ANALYZE loan_applications;
EXPLAIN (ANALYZE)
SELECT * FROM loan_applications WHERE status = 'DRAFT' AND updated_by IS NULL;
-- ESTIMATE RIGHT: rows=~30,000. The multi-column MCV list stores the frequency of the combination.
SELECT statistics_name, attnames, most_common_vals[1:3], most_common_freqs[1:3]
FROM pg_stats_ext WHERE statistics_name = 'la_status_submitted_stats';

-- ===========================================================================================
\echo '=== Pair 8: prepared statements, custom vs generic plans on a skewed parameter'
-- ===========================================================================================
DEALLOCATE ALL;
-- sum(requested_amount) needs the heap, so the plan shape depends on how many rows match.
PREPARE by_status(text) AS SELECT sum(requested_amount) FROM loan_applications WHERE status = $1;
-- Custom plans for the first five executions: each planned with the real value.
EXPLAIN EXECUTE by_status('READY_FOR_REVIEW');   -- 2% of rows: Bitmap Heap Scan, rows=~20,000
EXPLAIN EXECUTE by_status('CLOSED');             -- 40% of rows: Seq Scan, rows=~400,000
EXECUTE by_status('CLOSED'); EXECUTE by_status('CLOSED'); EXECUTE by_status('CLOSED');
EXECUTE by_status('CLOSED'); EXECUTE by_status('CLOSED');
EXPLAIN EXECUTE by_status('READY_FOR_REVIEW');
-- After 5 executions the plan cache compares the average custom-plan cost with the generic
-- plan's cost; if the generic one is not much worse it is used from now on. Spot it: the
-- condition shows "status = $1" and rows= is the AVERAGE estimate (~110,000 = 1 M / 9 states),
-- the same for every value. A generic plan built for the average status is wrong for rare values
-- (and for the 40% one). This is the "fast in psql, slow from the application" mystery.
SET plan_cache_mode = force_custom_plan;          -- per session, per role, or ALTER FUNCTION ... SET
EXPLAIN EXECUTE by_status('READY_FOR_REVIEW');    -- planned for the literal again: rows=~20,000
RESET plan_cache_mode;

-- The login statement as a prepared statement: uniform data, so the generic plan is fine.
PREPARE login_q(text, text) AS
  SELECT password_hash, status FROM app_users WHERE branch_code = $1 AND user_code = $2;
EXPLAIN (ANALYZE, BUFFERS) EXECUTE login_q('BR0456', 'U00123456');
-- Index Only Scan using app_users_login_key ... Heap Fetches: 0
DEALLOCATE ALL;

-- ===========================================================================================
\echo '=== Statistics the planner used'
-- ===========================================================================================
SELECT attname, null_frac, n_distinct, most_common_vals, most_common_freqs, correlation
FROM pg_stats
WHERE schemaname = 'loans' AND tablename = 'loan_applications' AND attname IN ('status', 'submitted_at', 'branch_code');
SELECT reltuples::bigint, relpages FROM pg_class WHERE oid = 'loan_applications'::regclass;
-- Module 06 exercise 3: estimated rows for status = 'APPROVED' = reltuples x the matching most_common_freqs entry.

SELECT relname, seq_scan, seq_tup_read, idx_scan, n_live_tup, n_dead_tup, last_autoanalyze
FROM pg_stat_user_tables WHERE schemaname = 'loans' ORDER BY seq_tup_read DESC LIMIT 5;

-- ===========================================================================================
\echo '=== auto_explain: log the plan of anything slower than 20 ms (this session only)'
-- ===========================================================================================
LOAD 'auto_explain';
SET auto_explain.log_min_duration = '20ms';
SET auto_explain.log_analyze = on;
SET auto_explain.log_buffers = on;
SET auto_explain.log_nested_statements = on;      -- also statements inside PL/pgSQL functions
SELECT count(*) FROM loan_applications WHERE term_months = 60;   -- now look at the server log
-- For every session: shared_preload_libraries = 'auto_explain' (or session_preload_libraries)
-- plus the settings in postgresql.conf. log_analyze adds timing overhead to every statement.

RESET max_parallel_workers_per_gather;
