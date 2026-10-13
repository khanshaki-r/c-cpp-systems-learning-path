-- m11_yugabyte_demo.sql : distributed SQL on YugabyteDB YSQL (module 11).
--
-- Start the node:   cd code && docker compose up -d yugabyte
-- Wait ~30 s, then: docker exec -it yb bin/ysqlsh -h yb -f /code/m11_yugabyte_demo.sql
--   (the compose file mounts this folder at /code inside the container)
-- or from WSL with the stock PostgreSQL 16 client:
--   psql -h localhost -p 5433 -U yugabyte -d yugabyte -f m11_yugabyte_demo.sql
-- Web UIs: http://localhost:15433 (yugabyted) and http://localhost:7000 (master: tables, tablets, leaders)
--
-- Better: paste step by step and compare the EXPLAIN (ANALYZE, DIST) outputs. The numbers to read:
--   Storage Read Requests / Storage Rows Scanned   RPCs to DocDB and rows read there
--   Storage Write Requests                         RPCs for writes (each one a Raft round)
--   Catalog Read Requests                          metadata lookups (first run after connect)
-- Exact output lines differ a little between YugabyteDB versions; the counts are what matter.

\set ON_ERROR_STOP on
\pset pager off

-- ===========================================================================================
\echo '--- 1. A colocated database: small reference tables share one tablet, big tables opt out'
-- ===========================================================================================
DROP DATABASE IF EXISTS loans_yb;
CREATE DATABASE loans_yb WITH COLOCATION = true;
\c loans_yb

CREATE TABLE branches (code text PRIMARY KEY, name text NOT NULL);                 -- colocated: one tablet for all small tables
CREATE TABLE app_users (
    id              bigint GENERATED ALWAYS AS IDENTITY,
    branch_code     text NOT NULL,
    user_code       text NOT NULL,
    password_hash   text NOT NULL,
    status          text NOT NULL DEFAULT 'ACTIVE',
    failed_attempts int  NOT NULL DEFAULT 0,
    last_login_at   timestamptz,
    PRIMARY KEY (id HASH)                                                          -- hash-sharded on id
) WITH (COLOCATION = false) SPLIT INTO 6 TABLETS;                                  -- its own 6 tablets

INSERT INTO branches SELECT 'BR' || lpad(g::text, 4, '0'), 'Branch ' || g FROM generate_series(0, 499) g;
INSERT INTO app_users (branch_code, user_code, password_hash)
SELECT 'BR' || lpad((g % 500)::text, 4, '0'), 'U' || lpad(g::text, 8, '0'), md5(g::text)
FROM generate_series(1, 200000) g;                                                 -- small enough for a laptop container
ANALYZE app_users;                                                                 -- you must: there is no autovacuum/autoanalyze
ANALYZE branches;                                                                  -- in older versions (check yb docs for yours)

-- ===========================================================================================
\echo '--- 2. The scatter: login by natural key with no index on it'
-- ===========================================================================================
EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Seq Scan on app_users ... Storage Filter (pushed down to DocDB, good) but
-- Storage Rows Scanned: 200000 and Storage Read Requests: one or more PER TABLET: every tablet,
-- on every node, scans its share of the table for one row. With 3 nodes this is 3 network hops
-- per login at minimum, and it gets worse with every node you add.

-- ===========================================================================================
\echo '--- 3. The fix: hash-sharded unique index on the natural key, covering the login columns'
-- ===========================================================================================
CREATE UNIQUE INDEX app_users_login_idx ON app_users ((branch_code, user_code) HASH)
    INCLUDE (password_hash, status, failed_attempts);
-- The index is itself a distributed table (its own tablets), built online by a backfill.
SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'app_users_login_idx'::regclass;
EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Index Only Scan using app_users_login_idx ... Storage Read Requests: 1, Storage Rows Scanned: 1.
-- The hash of (branch_code, user_code) names exactly one index tablet; the INCLUDE columns mean the
-- main table is never visited. One RPC to one tablet leader per login.

EXPLAIN (ANALYZE, DIST, COSTS OFF)
SELECT id, password_hash, status FROM app_users WHERE branch_code = 'BR0456' AND user_code = 'U00123456';
-- Asking for id too: Index Scan = index tablet + main table tablet: 2 Storage Read Requests.
-- On a cluster every extra table visit is a possible extra network hop: cover what you read.

-- ===========================================================================================
\echo '--- 4. A hash primary key: ORDER BY id must sort, and a range fans out'
-- ===========================================================================================
EXPLAIN (ANALYZE, DIST, COSTS OFF) SELECT id FROM app_users WHERE id BETWEEN 1000 AND 1100 ORDER BY id;
-- Sort -> Seq Scan with a Storage Filter on id: hash order is not id order, so a range of ids is
-- spread over all tablets. Compare a range-sharded key:
CREATE TABLE app_users_range (
    id bigint PRIMARY KEY,                    -- PRIMARY KEY (id ASC) is the same: range sharding
    branch_code text NOT NULL,
    user_code   text NOT NULL
) WITH (COLOCATION = false);
INSERT INTO app_users_range SELECT id, branch_code, user_code FROM app_users;
ANALYZE app_users_range;
EXPLAIN (ANALYZE, DIST, COSTS OFF) SELECT id FROM app_users_range WHERE id BETWEEN 1000 AND 1100 ORDER BY id;
-- Index Scan (on the primary key) with an Index Cond, no Sort, one tablet. The price: inserts of
-- increasing ids all land on the LAST tablet (a write hotspot) until it splits.

-- ===========================================================================================
\echo '--- 5. Sequences are cached per node: ids are unique, not monotonic across nodes'
-- ===========================================================================================
SELECT nextval(pg_get_serial_sequence('app_users', 'id')) FROM generate_series(1, 3);
-- Values jump in blocks (ysql_sequence_cache_minval, 100 by default): each connection/node takes
-- a block. Never rely on ids for ordering or "gapless" numbering on a cluster.

-- ===========================================================================================
\echo '--- 6. Where are the tablets?'
-- ===========================================================================================
SELECT host, port, node_type, cloud, region, zone FROM yb_servers();
SELECT * FROM yb_table_properties('app_users'::regclass);     -- num_tablets, num_hash_key_columns, is_colocated
SELECT * FROM yb_table_properties('branches'::regclass);      -- is_colocated = true
-- http://localhost:7000/tables -> app_users -> the tablet list with each tablet's hash range
-- (0x0000-0x2AAA, ...) and its Raft leader. With one node every leader is on that node.

-- ===========================================================================================
\echo '--- 7. Isolation: snapshot isolation by default, and the error you must retry'
-- ===========================================================================================
BEGIN; SELECT current_setting('transaction_isolation'); ROLLBACK;
-- In two sessions (A and B):
--   A: BEGIN ISOLATION LEVEL REPEATABLE READ; UPDATE app_users SET failed_attempts = 1 WHERE id = 5;
--   B: BEGIN ISOLATION LEVEL REPEATABLE READ; UPDATE app_users SET failed_attempts = 2 WHERE id = 5;
--   Depending on the concurrency-control mode (fail-on-conflict vs wait-on-conflict, version and
--   flags), B either waits for A or fails at once with SQLSTATE 40001. After A commits, B gets
--   40001. Your client must retry the whole transaction, exactly as in module 05 section 3.4.

-- ===========================================================================================
\echo '--- 8. Follower reads for a read-only report (stale by design)'
-- ===========================================================================================
BEGIN TRANSACTION READ ONLY;
SET LOCAL yb_read_from_followers = true;
SET LOCAL yb_follower_read_staleness_ms = 30000;
SELECT count(*) FROM app_users;
COMMIT;
-- With RF = 3 the read can be served by the nearest replica instead of the leader, at a
-- timestamp up to 30 s old. Fine for a dashboard; never for a balance check.

-- ===========================================================================================
\echo '--- 9. Writes: a single-row insert vs a multi-row insert (Storage Write Requests)'
-- ===========================================================================================
EXPLAIN (ANALYZE, DIST, COSTS OFF)
INSERT INTO app_users (branch_code, user_code, password_hash) VALUES ('BR0001', 'U90000001', 'x');
EXPLAIN (ANALYZE, DIST, COSTS OFF)
INSERT INTO app_users (branch_code, user_code, password_hash)
SELECT 'BR0002', 'U9' || lpad(g::text, 7, '0'), 'x' FROM generate_series(2, 1001) g;
-- The single row writes to the main table AND the unique index (two tablets: a distributed
-- transaction with provisional "intents" on both). The 1000-row insert batches writes per tablet
-- (ysql_session_max_batch_size), so far fewer RPCs per row: this is why the migration engine
-- (module 12) writes in batches.

\echo 'Done. To clean up: connect to database yugabyte, then DROP DATABASE loans_yb;'
