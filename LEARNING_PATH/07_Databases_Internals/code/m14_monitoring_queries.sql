-- m14_monitoring_queries.sql : the queries you run when PostgreSQL is slow at 3 AM (module 14).
--
-- Run:  psql loans -f m14_monitoring_queries.sql       (as a superuser or a member of pg_monitor)
-- Each section is independent: in an incident, copy the one you need.
--
-- pg_stat_statements needs, in postgresql.conf (then a restart):
--     shared_preload_libraries = 'pg_stat_statements'
--     pg_stat_statements.track = all          # also statements inside functions
--     track_io_timing = on                    # adds blk_read_time / blk_write_time
-- On Ubuntu: sudo nano /etc/postgresql/16/main/postgresql.conf ; sudo service postgresql restart

\pset pager off
\x off
SET search_path = loans, public;

\echo '=== 0. What is configured (the settings module 14 discusses)'
SELECT name, setting, unit, source
FROM pg_settings
WHERE name IN ('max_connections', 'shared_buffers', 'effective_cache_size', 'work_mem',
               'maintenance_work_mem', 'wal_buffers', 'max_wal_size', 'checkpoint_timeout',
               'checkpoint_completion_target', 'random_page_cost', 'effective_io_concurrency',
               'synchronous_commit', 'wal_level', 'max_wal_senders', 'hot_standby_feedback',
               'autovacuum_max_workers', 'autovacuum_vacuum_cost_limit', 'autovacuum_vacuum_scale_factor',
               'autovacuum_naptime', 'idle_in_transaction_session_timeout', 'statement_timeout',
               'lock_timeout', 'log_min_duration_statement', 'shared_preload_libraries')
ORDER BY name;

\echo '=== 1. Top statements by total time (needs pg_stat_statements)'
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
SELECT left(regexp_replace(query, '\s+', ' ', 'g'), 70)            AS query,
       calls,
       round(total_exec_time::numeric, 1)                           AS total_ms,
       round(mean_exec_time::numeric, 3)                            AS mean_ms,
       round(stddev_exec_time::numeric, 3)                          AS stddev_ms,
       rows / greatest(calls, 1)                                    AS rows_per_call,
       shared_blks_hit + shared_blks_read                           AS blocks,
       round(100.0 * shared_blks_hit / nullif(shared_blks_hit + shared_blks_read, 0), 1) AS hit_pct,
       temp_blks_written
FROM pg_stat_statements
WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
ORDER BY total_exec_time DESC
LIMIT 15;
-- Read it like this: total_ms ranks what costs the server most (a 2 ms query called 5 M times
-- beats a 3 s report run twice). mean_ms with a big stddev = plan flips or lock waits.
-- blocks per call >> rows per call = reading much more than returning: a missing index.
-- temp_blks_written > 0 = sorts or hashes spilling: work_mem or a bad plan.
-- Reset the counters before a measurement window: SELECT pg_stat_statements_reset();

\echo '=== 2. Who is connected, and doing what'
SELECT state, wait_event_type, count(*) AS sessions,
       max(now() - state_change) AS longest_in_state
FROM pg_stat_activity
WHERE backend_type = 'client backend'
GROUP BY state, wait_event_type
ORDER BY sessions DESC;

\echo '=== 3. Long-running statements and transactions (top 10 by transaction age)'
SELECT pid, usename, application_name, client_addr, state,
       now() - xact_start  AS xact_age,
       now() - query_start AS query_age,
       wait_event_type, wait_event,
       backend_xmin,
       left(regexp_replace(query, '\s+', ' ', 'g'), 60) AS query
FROM pg_stat_activity
WHERE backend_type = 'client backend' AND xact_start IS NOT NULL AND pid <> pg_backend_pid()
ORDER BY xact_start
LIMIT 10;
-- "idle in transaction" with a large xact_age is the classic VACUUM blocker: its backend_xmin
-- holds back the cleanup horizon for the WHOLE cluster. Kill it: SELECT pg_terminate_backend(pid);
-- Prevent it: idle_in_transaction_session_timeout = '60s' (per role for the application).

\echo '=== 4. Blocking chains: who waits for whom'
SELECT blocked.pid                                   AS blocked_pid,
       now() - blocked.query_start                   AS waiting_for,
       left(blocked.query, 50)                       AS blocked_query,
       blocking.pid                                  AS blocking_pid,
       blocking.state                                AS blocking_state,
       now() - blocking.xact_start                   AS blocking_xact_age,
       left(blocking.query, 50)                      AS blocking_query
FROM pg_stat_activity blocked
JOIN LATERAL unnest(pg_blocking_pids(blocked.pid)) AS b(pid) ON true
JOIN pg_stat_activity blocking ON blocking.pid = b.pid
ORDER BY waiting_for DESC;
-- Follow blocking_pid until you reach a session that is not itself blocked: that is the root.
-- Often it is "idle in transaction": the application opened a transaction and went away.

\echo '=== 5. Lock detail (pg_locks joined to relations)'
SELECT l.pid, l.locktype, l.mode, l.granted,
       coalesce(l.relation::regclass::text, l.transactionid::text, l.virtualxid) AS target,
       a.state, left(a.query, 40) AS query
FROM pg_locks l
JOIN pg_stat_activity a ON a.pid = l.pid
WHERE a.datname = current_database() AND l.pid <> pg_backend_pid()
ORDER BY l.granted, l.pid
LIMIT 30;
-- granted = false rows are the waiters. An AccessExclusiveLock wanted by a DDL statement queues
-- behind a long reader and then blocks EVERY later reader too: always SET lock_timeout before DDL.

\echo '=== 6. Buffer cache hit ratio per database (a trend, not a target)'
SELECT datname, blks_hit, blks_read,
       round(100.0 * blks_hit / nullif(blks_hit + blks_read, 0), 2) AS hit_pct,
       xact_commit, xact_rollback, deadlocks, temp_files, pg_size_pretty(temp_bytes) AS temp
FROM pg_stat_database
WHERE datname IS NOT NULL
ORDER BY blks_hit + blks_read DESC;
-- blks_read counts reads from the OS, which may still be served by the OS page cache. A ratio of
-- 99% can hide a hot spot; pg_stat_statements per query is the better instrument.

\echo '=== 7. Tables: dead tuples, last vacuum and analyze, seq scans'
SELECT relname, n_live_tup, n_dead_tup,
       round(100.0 * n_dead_tup / nullif(n_live_tup + n_dead_tup, 0), 1) AS dead_pct,
       last_autovacuum, last_autoanalyze, autovacuum_count,
       seq_scan, seq_tup_read, idx_scan,
       n_tup_upd, n_tup_hot_upd
FROM pg_stat_user_tables
ORDER BY n_dead_tup DESC
LIMIT 10;
-- seq_tup_read huge relative to n_live_tup x seq_scan on a big table: some query scans it often.

\echo '=== 8. Autovacuum: thresholds vs reality, and what is running now'
SELECT s.relname, s.n_dead_tup,
       current_setting('autovacuum_vacuum_threshold')::int
         + current_setting('autovacuum_vacuum_scale_factor')::float8 * c.reltuples AS default_trigger_at,
       c.reloptions
FROM pg_stat_user_tables s JOIN pg_class c ON c.oid = s.relid
ORDER BY s.n_dead_tup DESC
LIMIT 5;
SELECT p.pid, p.relid::regclass AS table, p.phase,
       p.heap_blks_scanned, p.heap_blks_total, p.index_vacuum_count, p.num_dead_tuples
FROM pg_stat_progress_vacuum p;

\echo '=== 9. Transaction id wraparound: how close is each database and table?'
SELECT datname, age(datfrozenxid) AS xid_age,
       round(100.0 * age(datfrozenxid) / 2000000000, 1) AS pct_of_hard_limit
FROM pg_database ORDER BY xid_age DESC;
SELECT c.oid::regclass AS table, age(c.relfrozenxid) AS xid_age,
       pg_size_pretty(pg_total_relation_size(c.oid)) AS size
FROM pg_class c
WHERE c.relkind IN ('r', 'm', 't')
ORDER BY age(c.relfrozenxid) DESC
LIMIT 5;
-- autovacuum_freeze_max_age (200 M) triggers anti-wraparound vacuums; at ~2 billion the server
-- stops assigning xids to protect data. Alert at 500 M to 1 B; find what blocks freezing.

\echo '=== 10. Indexes: unused, and largest'
SELECT s.relname AS table, s.indexrelname AS index, s.idx_scan,
       pg_size_pretty(pg_relation_size(s.indexrelid)) AS size,
       i.indisunique
FROM pg_stat_user_indexes s JOIN pg_index i ON i.indexrelid = s.indexrelid
WHERE s.idx_scan = 0 AND NOT i.indisunique AND NOT i.indisprimary
ORDER BY pg_relation_size(s.indexrelid) DESC
LIMIT 10;
-- idx_scan = 0 since the last stats reset: a candidate to drop, AFTER checking replicas (their
-- reads are counted there, not here) and month-end jobs that have not run yet.

\echo '=== 11. Checkpoints and background writer (PostgreSQL 16 column names)'
SELECT checkpoints_timed, checkpoints_req,
       round(100.0 * checkpoints_req / nullif(checkpoints_timed + checkpoints_req, 0), 1) AS req_pct,
       buffers_checkpoint, buffers_clean, buffers_backend, maxwritten_clean,
       stats_reset
FROM pg_stat_bgwriter;
-- req_pct high: max_wal_size too small. buffers_backend high: backends write their own dirty
-- pages because the bgwriter cannot keep up (bgwriter_lru_maxpages, more shared_buffers).

\echo '=== 12. WAL generation'
SELECT wal_records, wal_fpi, pg_size_pretty(wal_bytes) AS wal_bytes, wal_buffers_full, stats_reset
FROM pg_stat_wal;

\echo '=== 13. Replication: standbys and slots (run on the primary)'
SELECT application_name, client_addr, state, sync_state,
       pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn)) AS replay_lag_bytes,
       write_lag, flush_lag, replay_lag
FROM pg_stat_replication;
SELECT slot_name, slot_type, active, database,
       pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)) AS retained_wal,
       wal_status
FROM pg_replication_slots;
-- An inactive slot retains WAL forever: the disk fills and the PRIMARY stops. Alert on
-- retained_wal; set max_slot_wal_keep_size as a safety net.

\echo '=== 14. Sizes: the biggest relations, with indexes and TOAST'
SELECT c.oid::regclass AS relation,
       pg_size_pretty(pg_total_relation_size(c.oid)) AS total,
       pg_size_pretty(pg_relation_size(c.oid))       AS heap,
       pg_size_pretty(pg_indexes_size(c.oid))        AS indexes
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind = 'r' AND n.nspname NOT IN ('pg_catalog', 'information_schema')
ORDER BY pg_total_relation_size(c.oid) DESC
LIMIT 10;
