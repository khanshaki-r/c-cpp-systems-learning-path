# Exercises: 07_Databases_Internals

Every exercise from modules 01 to 14 in one place, grouped by module, with difficulty and a hint.
The full wording and the exact "done when" condition are in each module's section 6; this list is
for planning and for getting unstuck. Before any of them: load `code/m09_schema.sql` and
`code/m09_seed.sql` (see `code/README.md`).

Suggested path when time is short: all **easy** ones in modules 01 to 06, then the **medium**
ones in 03, 05, 06, 08 and 12. Those cover most interview questions.

Count: 79 exercises (28 easy, 28 medium, 23 hard).

---

## 01. What a database actually does

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 1.1 | Install PostgreSQL 16 on WSL, create `loans`, load schema and seed; `\dt` lists tables | easy | Follow `code/README.md` section 1. If `createdb` fails with "role does not exist", you skipped `createuser --superuser $USER`. |
| 1.2 | Run the five psql snippets of section 3.2 and name the layer behind each output | easy | Syntax error = parser (no catalog access); unknown table = analyzer; plan = planner/executor; file path = storage manager; `pg_locks` = lock manager. |
| 1.3 | Identify postmaster, checkpointer, bgwriter, walwriter, autovacuum launcher and your backend with `ps` | medium | `ps -o pid,ppid,cmd -u postgres --forest`. Your backend's pid equals `SELECT pg_backend_pid()`; its parent is the postmaster. |
| 1.4 | Loop the libpq query 1000 times, time it; then in one transaction; then with an UPDATE | medium | Read-only: no difference (no commit cost). UPDATE: per-statement autocommit pays one WAL flush per row; one transaction pays one. Use `clock_gettime(CLOCK_MONOTONIC)`. |
| 1.5 | Draw an `UPDATE ... WHERE id = 42` through all layers, including pages touched and when bytes are durable | hard | Heap page (old + new version), every index whose key changed (all of them if not HOT), WAL records, commit flush. Data pages reach disk later, at checkpoint or by the bgwriter. Check against modules 02 to 05. |

## 02. Storage engines, pages and heap files

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 2.1 | Run `m02_page_inspect.sql` steps 1 to 3; point at `pd_lower`/`pd_upper` and the free bytes | easy | Free space on the page = `upper - lower`. The header is 24 bytes; each line pointer 4 bytes. |
| 2.2 | Run the alignment step and explain the bytes per row difference | easy | `int8` needs 8-byte alignment: a `bool` before it costs 1 + 7 padding bytes. Compare `avg(pg_column_size(t.*))`. |
| 2.3 | 200k rows with a 3 KB text column: main vs TOAST size; then `STORAGE EXTERNAL` + `VACUUM FULL` | medium | Values over ~2 KB are compressed first, then moved out of line. EXTERNAL skips compression, so the TOAST table grows. `pg_relation_size(reltoastrelid)`. |
| 2.4 | `count(*)` on a table bigger than `shared_buffers/4`, twice, with BUFFERS: `read` stays high | medium | Large sequential scans use a 256 KB ring buffer so they cannot evict the whole cache. The second run is served by the OS page cache (fast) but still counts as `read`. |
| 2.5 | Update one row 5 times without VACUUM, dump `heap_page_items` each time, then VACUUM | hard | Follow `t_ctid` from old to new. Look for `lp_flags` 2 (REDIRECT) after pruning and 3 (DEAD) before VACUUM's index pass. Do the updates on a column that is not indexed to see HOT chains. |

## 03. B-tree indexes in depth

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 3.1 | Run `m03_index_demos.sql` steps 1 to 3; save both plans and read `Buffers: shared hit` | easy | Before: buffers ~ number of heap pages. After: 3 to 5. The script works in its own schema `m03`. |
| 3.2 | Height of `app_users_login_idx` and `app_users_pkey` with `bt_metap`; fan-out from the root | easy | `level` = height - 1. Fan-out ~ `live_items` on the root (or an internal page). Narrow keys give higher fan-out. |
| 3.3 | Minimum index set for three `loan_applications` queries, all plans index-driven, no Sort | medium | Equality columns first, then the ORDER BY column. The schema already has most of them: check `\d loan_applications` before creating duplicates. |
| 3.4 | HOT ratio with fillfactor 100 vs 80 for 100k updates of a non-indexed column | medium | Update scattered rows (`WHERE id % k = 0`), not a contiguous range: consecutive rows share pages and exhaust the free space. Compare `n_tup_hot_upd / n_tup_upd`. |
| 3.5 | Random `uuid` PK vs `bigint` identity: index size, `avg_leaf_density`, insert time for 2 M rows | hard | Random keys split pages everywhere and leave them ~70% full; sequential keys split only the rightmost page and fill ~90%. `gen_random_uuid()` is built in. |
| 3.6 | Reproduce the implicit-cast problem from libpq with `paramTypes[0] = 20` (int8) | hard | `text = bigint` has no operator: an error. Casting the column instead (`user_code::bigint`) runs but disables the index. Fix by binding type 25 (text) or 0 (unknown). |

## 04. WAL and crash recovery

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 4.1 | WAL bytes of an UPDATE right after `CHECKPOINT` vs immediately again | easy | `m04_wal_inspect.sh` step 2 does it on `accounts`: ~8 KB (a full-page image) then ~100 bytes. |
| 4.2 | In `pg_waldump`, find a Heap UPDATE, its Btree insert, and the COMMIT | easy | Update an indexed column so index records appear. `blkref #0: rel A/B/C blk N` = tablespace/database/relfilenode and block; `FPW` = full-page image. |
| 4.3 | `kill -9` mid-transaction; read redo start/end LSNs; confirm the update is absent | medium | `bash m04_wal_inspect.sh --crash` on a dev box. The uncommitted xid has no commit record, so its tuples are invisible: no undo pass. |
| 4.4 | 2000 single-row inserts: `synchronous_commit` on vs off vs one transaction | medium | Count fsyncs: 2000, ~0 (the WAL writer flushes in the background), 1. `pg_test_fsync` shows your disk's fsync rate. |
| 4.5 | PITR: archive WAL, base backup, drop a table, restore to just before the drop on port 5433 | hard | `recovery_target_time` plus `restore_command = 'cp /var/lib/postgresql/wal_archive/%f %p'` and an empty `recovery.signal`. Note the time BEFORE the drop with `SELECT now()`. |

## 05. Transactions, ACID and MVCC

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 5.1 | Demos 1 and 2 from the two-session scripts | easy | Paste in label order (A1, B1, A2). Set different psql prompts in the two windows. |
| 5.2 | Update a row in an open transaction and dump both versions' `t_xmin`/`t_xmax`/`t_ctid` | easy | `pg_current_xact_id()` gives your xid; from the other session `pg_current_snapshot()` lists it as in progress. |
| 5.3 | Visibility table for snapshot `200:210:203,207` and six tuples; verify two | medium | A version is visible if its xmin committed and is visible to the snapshot (below 210 and not 203 or 207), AND its xmax is 0, aborted, or NOT visible to the snapshot (203, 207, or 210 and above count as still running). xmin 211 is in the future: invisible. |
| 5.4 | Reproduce a deadlock with `m05_deadlock_a/b.sql`, read the log, fix by ordering | medium | The detector runs after `deadlock_timeout` (1 s). Fix: lock rows in a global order (`ORDER BY id FOR UPDATE`). |
| 5.5 | Idle REPEATABLE READ transaction blocks VACUUM: see "cannot be removed yet, oldest xmin" | hard | The snapshot is taken at the first statement, so run `SELECT 1` after `BEGIN`. Compare `pg_relation_size` before and after the second VACUUM. |
| 5.6 | libpq retry loop on 40001/40P01 under the write-skew demo from two processes | hard | Retry the WHOLE transaction including the reads; add jittered backoff; count retries. The invariant query must never return 0. |

## 06. Query planning and EXPLAIN

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 6.1 | Case study steps 0 to 2; explain every line of the final Index Only Scan plan | easy | `code/m06_explain_login_query.sql` builds the before state on a copy. Index Cond vs Filter, Heap Fetches, Buffers. |
| 6.2 | `m06_slow_vs_fast_queries.sql` pairs 1 to 4: name the node that changed and why | easy | Read bottom-up. Compare `rows=` estimated vs actual and `Rows Removed by Filter`. |
| 6.3 | Compute the estimate for `status = 'APPROVED'` from `pg_stats` by hand | medium | `reltuples x most_common_freqs[i]` where `most_common_vals[i] = 'APPROVED'`. Run ANALYZE first so both are fresh. |
| 6.4 | Make a nested loop go bad through correlated columns; fix with `CREATE STATISTICS` | medium | `status = 'DRAFT' AND updated_by IS NULL` is perfectly correlated in the seed (pair 7 of the m06 file). Join the result to `customers` and watch the join method change. |
| 6.5 | Five-statement login vs one PL/pgSQL call, 100 iterations each with `\timing` | hard | Load `plpgsql/m08_functions.sql`. Use a shell loop with `psql -c` for honest round trips, or `pgbench -f` with a custom script for both. |
| 6.6 | `random_page_cost = 1.1`: re-run all pairs, argue for or against with `track_io_timing` | hard | Look for plans that switch from Seq Scan / Bitmap to Index Scan. `track_io_timing` adds `I/O Timings` to BUFFERS output. |

## 07. SQL for backend engineers

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 7.1 | LEFT JOIN trap and fix on applications and documents | easy | Section 1 of `code/m07_sql_patterns.sql`. `NULL = 'ID_PROOF'` is NULL, not false, and WHERE drops it. |
| 7.2 | Latest application per customer both ways, no Sort node | easy | Index `(customer_id, submitted_at DESC)` exists in the schema. Mind NULL ordering: DESC puts NULLs first. |
| 7.3 | Keyset pagination over 10 pages, insert in between, no skips or duplicates | medium | Carry the last `(submitted_at, id)` with `\gset` (section 10 of the m07 file). Rows inserted with a newer timestamp land before page 1, never inside your walk. |
| 7.4 | Load 1 M rows three ways: autocommit inserts, multi-row INSERT, `\copy` | medium | Stop the single-row loop at 10k and extrapolate. Use an UNLOGGED staging table to compare client cost without WAL noise, then repeat logged. |
| 7.5 | Berkeley DB style loop as PL/pgSQL FOR loop vs one set-based UPDATE, timed on 100k rows | hard | Do both inside `BEGIN ... ROLLBACK` so you can repeat. The ratio comes from per-row statement overhead and plan reuse vs one hash join. |
| 7.6 | Recursive CTE over `workflow_transitions`: reachable states with shortest path, terminating on a cycle | hard | The seed has the cycle UNDER_REVIEW -> SUBMITTED -> UNDER_REVIEW. Use a depth bound or the 14+ `CYCLE` clause; section 7 of the m07 file shows both. |

## 08. PL/pgSQL stored procedures

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 8.1 | Call `sp_transition_application` for a valid and an invalid transition; see LN403 with `\errverbose` | easy | Load the three `plpgsql/` files in order; the end of `m08_workflow_rules.sql` is a ready walk-through with fixtures. |
| 8.2 | Write an inlinable `LANGUAGE sql STABLE fn_user_has_role` and prove inlining with EXPLAIN | easy | No `SET` clause (it prevents inlining), not STRICT, a single SELECT. Use it in a WHERE over many rows and look for the join instead of a function call in the plan. |
| 8.3 | 100k inserts with an EXCEPTION block per row vs `ON CONFLICT DO NOTHING` | medium | Each block is a subtransaction; with writes each gets an xid. Watch `pg_stat_activity` / `pg_subtrans` effects and the timing. |
| 8.4 | Mark `fn_validate_application` IMMUTABLE and demonstrate a stale answer | medium | Use it in an expression the planner can fold at plan time (a prepared statement with constant arguments), change documents, re-execute. |
| 8.5 | SECURITY DEFINER search_path hijack, then the fix | hard | Create a definer function without `SET search_path` that calls unqualified `lower()`. As a low-privilege role, create `public.lower(text)` and put `public` before `pg_catalog` in that role's search_path. |
| 8.6 | pgTAP tests: permitted, forbidden (role), blocked (validation), audit row | hard | `plpgsql/m08_pgtap_tests.sql` is a 12-test reference; write yours before reading it. Use `\gset` for ids because CALL arguments cannot contain subqueries. |

## 09. Schema design and data modelling

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 9.1 | Load the schema and seed; "FK without index" query returns no rows | easy | If a row appears, check whether an index starts with that column; composite indexes count only for their first column. |
| 9.2 | Violate three constraints; name the constraint in each error | easy | Status not DRAFT without `submitted_at`, a negative amount, an `application_no` that is not `LA-YYYY-NNNNNN`. Constraint names are in the error text. |
| 9.3 | 3NF design for a denormalised Berkeley DB style record | medium | Entities: loan, customer (with phones as a child table), branch, product (with rate history), officer (a user), status (lookup). Every original field must appear exactly once. |
| 9.4 | Per-year gapless `application_no` generator; 2 x 1000 concurrent calls, 2000 distinct | medium | A counter table keyed by year, `UPDATE ... SET n = n + 1 RETURNING n` (the row lock serialises). Never a sequence: sequences have gaps. Test with two `pgbench -f` clients. |
| 9.5 | Range-partition `application_audit` by month, migrate rows, show pruning, drop a month fast | hard | Unique constraints must include the partition key, so the PK becomes `(id, at)`. Create the partitioned table new, `INSERT ... SELECT`, swap names in one transaction. |

## 10. Berkeley DB and embedded KV stores

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 10.1 | Build and run `m10_bdb_cursor_txn.c`; read `db_stat -c` and `-m` | easy | Run from a scratch directory: it creates `bdb_env/` where you run it. `-c` = locks, `-m` = mpool cache hits. |
| 10.2 | `DB_NOOVERWRITE` vs plain put; then `DB_DUPSORT` and walk dups with `DB_NEXT_DUP` | easy | Second run already shows `DB_KEYEXIST`. DUPSORT must be set before the database is first created: delete `bdb_env` first. |
| 10.3 | Two processes with overlapping long transactions: deadlock, retry wrapper; then `DB_MULTIVERSION` + `DB_TXN_SNAPSHOT` | medium | Page locks: keys on the same page conflict even if different. Add a `sleep` inside the transaction to widen the window. MVCC needs the env flag AND the txn/cursor flag. |
| 10.4 | `kill -9` mid-transaction, verify with `db_dump`; then remove `DB_RECOVER` and kill again | medium | Without recovery, the next open may fail (`DB_RUNRECOVERY`) or see an inconsistent environment. `db_recover -h bdb_env -v` repairs it. |
| 10.5 | 5 M random keys into RocksDB vs Berkeley DB BTREE, then 100k point reads | hard | `sudo apt install librocksdb-dev`. Give both the same cache size. Explain with write amplification (LSM writes sequentially) and read amplification (B-tree one path). |

## 11. YugabyteDB and distributed SQL

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 11.1 | Steps 1 to 3 of `m11_yugabyte_demo.sql`; compare `EXPLAIN (ANALYZE, DIST)` | easy | Compare "Storage Read Requests" and "Storage Rows Scanned" before and after the index, not the timings. |
| 11.2 | Show the Sort and fan-out with a hash PK; then `PRIMARY KEY (id ASC)` touches one tablet | easy | Step 4 of the demo file builds `app_users_range` for you. State the price: insert hotspot on the last tablet. |
| 11.3 | 3-node cluster with RF 3; stop a node during point reads; measure the error window | medium | Uncomment `yb2`/`yb3` in `docker-compose.yml`. Run reads in a shell loop with timestamps; the gap is leader election plus client reconnect. |
| 11.4 | 10,000 single-row inserts with three secondary indexes vs one | medium | Each index is a separate distributed table: more tablets per transaction, more intents, a distributed commit instead of a single-shard fast path. |
| 11.5 | Tablet and index design for the full loan schema on 3 nodes, every line justified | hard | Colocate the master tables; hash-shard big tables on the access key; covering hash indexes for point lookups; range indexes only where you range-scan. Check each module 06 query for scatter. |
| 11.6 | Reproduce 40001 on YSQL; libpq retry loop on port 5433, 1000 attempts from two processes | hard | The module 05 C loop works unchanged: same protocol, same SQLSTATEs. Log every retry and reconcile the final balance. |

## 12. Migrating a key-value store to relational

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 12.1 | Build and run `m12_migration_engine.cpp`; explain the parity report and the corrupt records | easy | Every record's hash is computed from the decoded source and recomputed from the stored target row by a separate path. Rejects are version 9, bad BCD, truncation, negative amounts. |
| 12.2 | Build `m12_packed_record_decode.cpp`; flip one field's endianness; explain the failing bytes | easy | Change `rd_be16` to `rd_le16` for `branch_no`: 0x01F4 (500) becomes 0xF401 (62465). |
| 12.3 | Kill switch after batch 3; rerun; prove no duplicates and the right resume key | medium | `main()` already does this with `crash_after_batches`; move the crash to before `commit()` and check the batch is redone, not lost. ON CONFLICT skips should stay 0. |
| 12.4 | Real `LibpqSink` against WSL PostgreSQL; load and check a 1% sample with the SQL parity function | medium | Build with `-DHAVE_LIBPQ -I/usr/include/postgresql ... -lpq`; the sink is already implemented. Then query `migration_parity` joined to `loan_applications` with `fn_parity_hash_loan`. |
| 12.5 | Dual-write function with replay queue and idempotency keys; parity daemon SQL for the last 10 minutes | hard | Write old first (it is still the source of truth), then new; on new-store failure enqueue the key, never the payload. The daemon selects keys changed since its last run from the replay/change log, not by wall clock. |
| 12.6 | Full cutover runbook for the loan migration, presented in 10 minutes | hard | Start from the 14-step skeleton in section 2.8; every step needs an owner, a numeric check and a rollback. Rehearse the rollback flag flip. |

## 13. Caching and Redis

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 13.1 | Start Redis, run `m13_redis_cli_demo.sh`, explain every output | easy | The second `SET NX` returns nil because the key exists; `*-1` after EXEC is the WATCH abort. |
| 13.2 | `m13_cache_aside.py demo`: 1000 reads, hit ratio, one invalidation miss | easy | Compare the script's own counters with `INFO stats` keyspace_hits/misses. The update deletes the key after COMMIT. |
| 13.3 | Stampede: 200 concurrent misses with and without the lock; count DB loads | medium | `python3 m13_cache_aside.py stampede 200` and `... --no-lock`; confirm with `pg_stat_statements.calls` for the branch SELECT. |
| 13.4 | `cache_versions` trigger + a libpq `LISTEN master_changed` reloader; detect missed notifications after restart | medium | Notifications are delivered at COMMIT and lost while you are disconnected; on reconnect compare your cached version numbers with `cache_versions`. Use `PQconsumeInput` + `PQnotifies` in a `poll()` loop. |
| 13.5 | Double-holder problem with `kill -STOP`; then a fencing token checked by PostgreSQL | hard | Take a sequence value with the lock; the UPDATE says `WHERE fence_token < $1` (and sets it). The paused, stale holder's token is lower, so its write affects 0 rows. |
| 13.6 | Redis design for sessions, rate limiting and event fan-out, with failure analysis | hard | Hash + TTL per session; fixed-window INCR+EXPIRE or a sorted-set sliding window; streams with consumer groups and XACK/XPENDING. For each: Redis restart, failover (async replication loses writes), slow consumer. |

## 14. Performance tuning and operations

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 14.1 | Enable `pg_stat_statements`, run the m06 file, read the top three statements | easy | Needs `shared_preload_libraries` and a restart; `SELECT pg_stat_statements_reset()` before the run so only your workload counts. |
| 14.2 | Leave a transaction idle; find it, its `backend_xmin`, its locks; terminate it | easy | Sections 3 to 5 of `code/m14_monitoring_queries.sql`. `pg_terminate_backend(pid)` rolls its transaction back. |
| 14.3 | pgbouncer in transaction mode; pgbench `-c 200` through it and direct; show the SET leak | medium | Use `-S` (select-only) for a fair comparison. For the leak: client A runs `SET search_path = public`, client B's next query on the same server connection sees it; `SHOW SERVERS` shows the pairing. |
| 14.4 | `pg_dump -Fd -j 4` and parallel restore; streaming replica on 5435; lag during seeding; promote | medium | Commands are in section 3. `replay_lag` is NULL when idle; watch `pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn)` instead. After promotion `timeline_id` becomes 2. |
| 14.5 | Make autovacuum fall behind under a pgbench update storm; fix with per-table settings and cost limit | hard | A pgbench script: `\set id random(1, 2000000)` then `UPDATE loans.app_users SET failed_attempts = failed_attempts + 0 WHERE id = :id;`. Sample `n_dead_tup` with `\watch 30`. |
| 14.6 | Logical replication of master tables to `loans_reporting`; rehearse a cutover with counts and parity hashes | hard | Same-cluster subscriptions need a pre-created slot and `create_slot = false`. Sequences are not replicated: `setval` them on the target at cutover. |
