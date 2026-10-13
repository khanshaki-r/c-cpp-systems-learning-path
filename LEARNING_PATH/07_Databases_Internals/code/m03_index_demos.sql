-- m03_index_demos.sql : B+tree indexes in practice on a 2 million row login table (module 03).
--
-- Run:  psql loans -f m03_index_demos.sql      (about 1 to 3 minutes; builds its own table)
-- Better: paste one numbered step at a time and read the plan before moving on.
--
-- Everything here lives in its own schema "m03", so it never disturbs loans.app_users (which
-- already has the fixed, covering unique constraint). The table starts the way the real one did
-- before the fix: a surrogate primary key and NO index on the login natural key.

\set ON_ERROR_STOP on
\pset pager off
DROP SCHEMA IF EXISTS m03 CASCADE;
CREATE SCHEMA m03;
SET search_path = m03, public;

\echo '--- 0. A user table shaped like the login story: surrogate PK, business codes as columns'
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
VACUUM ANALYZE app_users;
SELECT pg_size_pretty(pg_relation_size('app_users')) AS heap_size,
       pg_relation_size('app_users') / 8192          AS heap_pages;

\echo '--- 1. Before: Seq Scan. Note "Rows Removed by Filter" and the buffer count.'
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status FROM app_users
WHERE user_code = 'U00123456' AND branch_code = 'BR0456';
-- Expect: Seq Scan (or Gather + Parallel Seq Scan), Rows Removed by Filter: ~2,000,000,
-- Buffers: shared hit/read ~ the number of heap pages printed above. Every login reads the table.

\echo '--- 2. The fix: unique composite index on the natural key, covering the login columns'
CREATE UNIQUE INDEX app_users_login_idx
    ON app_users (branch_code, user_code) INCLUDE (password_hash, status, failed_attempts);
VACUUM app_users;   -- set visibility-map bits so the index-only scan can skip the heap

EXPLAIN (ANALYZE, BUFFERS)
SELECT password_hash, status FROM app_users
WHERE user_code = 'U00123456' AND branch_code = 'BR0456';
-- Expect: Index Only Scan using app_users_login_idx ... Heap Fetches: 0, Buffers: shared hit=4
-- (root, internal, leaf, plus the metapage on first access). Note the WHERE order does not
-- matter: the planner matches predicates to index columns, not to text order.

EXPLAIN (ANALYZE, BUFFERS)
SELECT id, password_hash, status FROM app_users
WHERE user_code = 'U00123456' AND branch_code = 'BR0456';
-- Asking for id as well: id is NOT in the index (the heap TID is, the id column is not), so this
-- is an Index Scan with one heap page read. Add id to INCLUDE if the login needs it.

\echo '--- 3. Leftmost prefix: branch alone uses the index; user_code alone cannot'
EXPLAIN SELECT count(*) FROM app_users WHERE branch_code = 'BR0456';     -- Index Only Scan, prefix match
EXPLAIN SELECT count(*) FROM app_users WHERE user_code  = 'U00123456';   -- Seq Scan (or parallel): no prefix

\echo '--- 4. Tree height and fan-out'
SELECT magic, version, root, level, fastroot, fastlevel FROM bt_metap('app_users_login_idx');
-- level = height - 1: level 2 means root -> internal -> leaf, i.e. 3 page reads to a leaf.
SELECT type, live_items, avg_item_size, free_size
FROM bt_page_stats('app_users_login_idx', (SELECT root FROM bt_metap('app_users_login_idx')));
-- live_items on the root = its fan-out. Leaf items are wide (they carry the INCLUDE payload);
-- internal items are narrow (suffix truncation keeps only the separating key prefix).
SELECT type, live_items, avg_item_size
FROM bt_page_stats('app_users_login_idx', 1);   -- block 1 is normally a leaf
SELECT * FROM bt_metap('app_users_pkey');       -- compare: 8-byte keys, higher fan-out

\echo '--- 5. Selectivity: the planner ignores an index on a low-cardinality column...'
CREATE INDEX app_users_status_idx ON app_users (status);
ANALYZE app_users;
EXPLAIN SELECT count(*) FROM app_users WHERE status = 'ACTIVE';   -- 98% of rows: Seq Scan
EXPLAIN SELECT count(*) FROM app_users WHERE status = 'LOCKED';   -- 2%: Bitmap or Index Only Scan
-- ... and a partial index is the right tool for the rare value
DROP INDEX app_users_status_idx;
CREATE INDEX app_users_locked_idx ON app_users (branch_code) WHERE status = 'LOCKED';
EXPLAIN SELECT count(*) FROM app_users WHERE status = 'LOCKED' AND branch_code = 'BR0450';
SELECT pg_size_pretty(pg_relation_size('app_users_locked_idx')) AS partial_index_size,
       pg_size_pretty(pg_relation_size('app_users_login_idx'))  AS login_index_size;

\echo '--- 6. Implicit casts and functions on the column break the index'
DO $$
BEGIN
    PERFORM id FROM m03.app_users WHERE user_code = 123456;          -- text = integer
EXCEPTION WHEN undefined_function THEN
    RAISE NOTICE 'expected error: %', SQLERRM;                      -- operator does not exist: text = integer
END $$;
EXPLAIN SELECT id FROM app_users WHERE user_code::bigint = 123456;  -- fails at runtime on 'U...' and cannot use the index anyway
EXPLAIN SELECT id FROM app_users
WHERE branch_code = 'BR0456' AND user_code = 'U' || lpad('123456', 8, '0');   -- fine: the cast is on the constant side
EXPLAIN SELECT id FROM app_users WHERE lower(branch_code) = 'br0456';         -- function on the column: Seq Scan

\echo '--- 7. HOT updates: an update that does not touch indexed columns'
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relid = 'm03.app_users'::regclass;
UPDATE app_users SET last_login_at = now() WHERE id % 2000 = 0;      -- 1000 logins scattered over the table
SELECT pg_sleep(1);   -- statistics are sent asynchronously; give the stats system a moment
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relid = 'm03.app_users'::regclass;
-- Surprise: n_tup_hot_upd stays at 0. The bulk load filled every page (fillfactor 100), so the
-- new versions do not fit on their own page, move to the end of the table, and need new index
-- entries. HOT requires (a) no indexed column changed AND (b) room on the same page.
--
-- The same experiment on a copy that leaves 20% of every page free:
CREATE TABLE app_users_ff80 WITH (fillfactor = 80) AS SELECT * FROM app_users WHERE id <= 200000;
CREATE UNIQUE INDEX ON app_users_ff80 (branch_code, user_code) INCLUDE (password_hash, status, failed_attempts);
VACUUM ANALYZE app_users_ff80;
UPDATE app_users_ff80 SET last_login_at = now() WHERE id % 200 = 0;  -- 1000 scattered logins, about one per page
SELECT pg_sleep(1);   -- statistics are sent asynchronously
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relid = 'm03.app_users_ff80'::regclass;
-- Now n_tup_hot_upd = n_tup_upd: one heap page write per update, zero index writes.
-- This is why loans.app_users is created WITH (fillfactor = 90). Now update an indexed column:
UPDATE app_users_ff80 SET status = 'DISABLED' WHERE id % 200 = 101;
SELECT pg_sleep(1);
SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_user_tables WHERE relid = 'm03.app_users_ff80'::regclass;
-- hot_upd does not rise: status is in the INCLUDE list, so the index needs a new entry
-- (it stores the value) and HOT is impossible, free space or not. Trap: "updating" status to the
-- value it already has (e.g. LOCKED on a locked user) changes nothing indexed and IS HOT.

\echo '--- 8. Bloat measurement and online rebuild'
SELECT avg_leaf_density, leaf_fragmentation, leaf_pages FROM pgstatindex('app_users_login_idx');
DELETE FROM app_users WHERE id % 3 = 0;           -- create churn: a third of the entries die
VACUUM app_users;                                  -- removes the index entries, does not shrink the index
SELECT avg_leaf_density, leaf_fragmentation, leaf_pages FROM pgstatindex('app_users_login_idx');
REINDEX INDEX CONCURRENTLY app_users_login_idx;   -- online rebuild, PostgreSQL 12+
SELECT avg_leaf_density, leaf_fragmentation, leaf_pages FROM pgstatindex('app_users_login_idx');
-- leaf_pages drops by about a third after the rebuild; density returns to ~90%.

\echo 'Done. Drop the demo with: DROP SCHEMA m03 CASCADE;'
