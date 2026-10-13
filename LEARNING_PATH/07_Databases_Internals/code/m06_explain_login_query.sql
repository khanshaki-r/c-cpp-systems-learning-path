-- m06_explain_login_query.sql : the login-latency case study, step by step with EXPLAIN (ANALYZE, BUFFERS)
-- (module 06 section 2, module 03 section 5, and the resume bullet about full-table scans).
--
-- Run:  psql loans -f m06_explain_login_query.sql      (about a minute; works on a copy)
--
-- The story: the user table had a surrogate primary key, but every login looked users up by the
-- natural key (branch_code, user_code), which had no index. Each login was a full scan, and on the
-- hash-sharded cluster a full scan fans out to every node. loans.app_users already has the fix,
-- so this script rebuilds the "before" state as m06.app_users_legacy and fixes it step by step.

\set ON_ERROR_STOP on
\pset pager off
DROP SCHEMA IF EXISTS m06 CASCADE;
CREATE SCHEMA m06;
SET search_path = m06, loans, public;
SET max_parallel_workers_per_gather = 0;     -- one process: the numbers are easier to reason about

\echo '--- Step 0: the before state: 2 M users, primary key on id, nothing on the natural key'
CREATE TABLE app_users_legacy AS
SELECT id, branch_code, user_code, password_hash, status, failed_attempts, last_login_at, created_at
FROM loans.app_users;
ALTER TABLE app_users_legacy ADD PRIMARY KEY (id);
VACUUM ANALYZE app_users_legacy;
SELECT pg_relation_size('app_users_legacy') / 8192 AS heap_pages;

EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status, failed_attempts
FROM app_users_legacy
WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Seq Scan on app_users_legacy  (cost=0.00..N rows=1 ...) (actual time=...  rows=1 loops=1)
--   Filter: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
--   Rows Removed by Filter: 1999999
--   Buffers: shared hit=... read=...        <- hit + read = heap_pages above: the WHOLE table
-- How to read it:
--   * rows=1 estimated and actual: the planner KNEW one row would match. Its estimate was right;
--     it simply had no access path other than reading every page.
--   * Rows Removed by Filter = table size - 1: the definition of a missing index.
--   * Buffers = every page, on every login. With 50 logins/s that is 50 full scans/s competing
--     for shared_buffers and pushing hot pages out (cache pollution hurts other queries too).

\echo '--- Step 1: unique index on the natural key (what a UNIQUE constraint would have created)'
CREATE UNIQUE INDEX app_users_legacy_login_idx ON app_users_legacy (branch_code, user_code);
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status, failed_attempts
FROM app_users_legacy
WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Index Scan using app_users_legacy_login_idx  (cost=0.43..8.45 rows=1 ...)
--   Index Cond: ((branch_code = 'BR0456'::text) AND (user_code = 'U00123456'::text))
--   Buffers: shared hit=4 or 5      <- root, internal, leaf (+ meta on first use) + ONE heap page
-- From tens of thousands of buffers to about five. "Index Cond" (not "Filter") is the line that
-- proves the predicate drives the descent of the tree.

\echo '--- Step 2: cover the columns the login reads, so the heap is not touched at all'
CREATE UNIQUE INDEX app_users_legacy_login_cov_idx
    ON app_users_legacy (branch_code, user_code) INCLUDE (id, password_hash, status, failed_attempts);
DROP INDEX app_users_legacy_login_idx;
VACUUM app_users_legacy;        -- visibility map bits: without them Index Only Scan still visits the heap
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status, failed_attempts
FROM app_users_legacy
WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Index Only Scan using app_users_legacy_login_cov_idx
--   Heap Fetches: 0
--   Buffers: shared hit=3 or 4
-- id is in INCLUDE here because the login needs it for the session row. Leave it out and the plan
-- falls back to an Index Scan with one heap fetch: read the SELECT list, not just the WHERE.

\echo '--- Step 2b: what breaks it again'
-- (a) A parameter of the wrong type. A bigint bound against a text column does not even compile:
--     ERROR: operator does not exist: text = bigint. Casting the COLUMN to make it work kills the index:
EXPLAIN SELECT id FROM app_users_legacy WHERE branch_code = 'BR0456' AND user_code::text || '' = 'U00123456';
-- Index (Only) Scan with Index Cond on the branch_code prefix only + Filter on the expression:
-- all ~4,000 entries of the branch are read for one user. Any function or cast applied to an
-- indexed column turns an Index Cond into a Filter.
-- Note: a second "Buffers:" line under "Planning:" counts catalog pages read while planning.
-- (b) A new column in the SELECT list that is not in INCLUDE: Index Only Scan -> Index Scan.
EXPLAIN SELECT id, password_hash, last_login_at FROM app_users_legacy
WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- (c) Status updates on lock-out are not HOT (status is in the index): accepted, they are rare.

\echo '--- Step 3: collapse the round trips (one function call instead of five statements)'
-- The function is in code/plpgsql/m08_functions.sql (fn_login_check). If it is loaded:
--   EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM loans.fn_login_check('BR0456', 'U00123456');
-- shows only "Function Scan"; to see the plans INSIDE it:
--   LOAD 'auto_explain'; SET auto_explain.log_min_duration = 0;
--   SET auto_explain.log_nested_statements = on; SET auto_explain.log_analyze = on;
-- and read the server log. Measure the path end to end with pg_stat_statements (module 14):
--   SELECT calls, mean_exec_time, shared_blks_hit, shared_blks_read, query
--   FROM pg_stat_statements WHERE query ILIKE '%app_users%' ORDER BY total_exec_time DESC;

\echo '--- Step 4: how the same mistake looks on YugabyteDB: see code/m11_yugabyte_demo.sql steps 2 and 3'
-- EXPLAIN (ANALYZE, DIST) shows "Storage Read Requests" per node: the scan contacts every tablet.

RESET max_parallel_workers_per_gather;
\echo 'Done. Drop the copy with: DROP SCHEMA m06 CASCADE;'
