# 07. Databases from the storage engine up: PostgreSQL, YugabyteDB, Berkeley DB, Redis

## What this course covers

You have used databases from the application side for 2.5 years: you wrote a Berkeley DB to
YugabyteDB migration engine in C, you write PL/pgSQL, you have fixed a slow login path on a
multi-node cluster. This course goes one level down. By the end you can explain what the
database engine does with every statement you send it: how a page looks on disk, how a B-tree
index is walked, how MVCC decides which row version you see, how WAL survives a power cut, how
the planner picks a plan and why it sometimes picks the wrong one, and what changes when the
"database" is actually a Raft-replicated cluster of RocksDB instances pretending to be PostgreSQL.

The hands-on database is **PostgreSQL 16 on WSL Ubuntu 24.04** (installed with `apt`). The
distributed parts use the **YugabyteDB docker image**. Caching uses **Redis** in docker.
Berkeley DB examples use `libdb-dev` from apt and compile with `gcc ... -ldb`.

## Why it is on the resume

Resume lines this course backs up (quoted exactly):

- "PostgreSQL, YugabyteDB, Berkeley DB, Oracle, SQL Server, Redis; database internals, storage
  engines, key-value stores, indexing, query optimisation, transactions, caching"
- "PL/pgSQL"
- Migration-engine bullets: B-tree cursor traversal, record-to-relational mapping, data-parity
  regression suite, zero-downtime cutover
- Stored-procedure bullets: PL/pgSQL layer, login-latency bottleneck from full-table scans,
  execution plans, composite and covering indexes on a multi-node cluster

An interviewer who sees "database internals" on a resume will ask you to explain MVCC, read an
EXPLAIN plan, design an index, and walk through a migration. Each of those gets a full module here.

## Prerequisites

- C at a working level (you have it). C++17 basics for module 12's engine skeleton
  (see 02_Modern_CPP if you need a refresher on RAII, `std::vector`, `std::optional`).
- Comfortable in a Linux shell on WSL.
- Basic SQL (SELECT, JOIN, GROUP BY). Module 07 fills in the rest.

## Setup (do this once, ~30 minutes)

```bash
# PostgreSQL 16 on WSL Ubuntu 24.04 (24.04 ships 16 in the default repos)
sudo apt update
sudo apt install -y postgresql-16 postgresql-contrib-16 postgresql-client-16 libpq-dev
sudo service postgresql start
sudo -u postgres createuser --superuser $USER
createdb loans
psql loans -c "select version();"

# Extensions used in the modules
psql loans -c "create extension if not exists pageinspect;"
psql loans -c "create extension if not exists pg_stat_statements;"   # needs shared_preload_libraries, see module 14
psql loans -c "create extension if not exists pg_buffercache;"
psql loans -c "create extension if not exists pgstattuple;"

# Berkeley DB (C library) and build tools
sudo apt install -y libdb-dev build-essential g++

# Docker (inside WSL) for YugabyteDB and Redis
# then: cd code && docker compose up -d
```

Load the example schema and data:

```bash
psql loans -f code/m09_schema.sql
psql loans -f code/m09_seed.sql
```

## Modules

| #  | File | Topic | Hours |
|----|------|-------|-------|
| 01 | `01_what_a_database_actually_does.md` | The layers of a DBMS; a statement's path from text to disk; library vs server | 2 |
| 02 | `02_storage_engines_pages_and_heap_files.md` | 8 KB pages, tuples, heap files, TOAST, FSM, VM, buffer pool, fsync | 3 |
| 03 | `03_btree_indexes_in_depth.md` | B+tree on disk, splits, covering and composite indexes, selectivity, HOT, bloat | 3 |
| 04 | `04_wal_and_crash_recovery.md` | WAL, LSN, checkpoints, redo, full-page writes, replication, PITR; Berkeley DB's log | 3 |
| 05 | `05_transactions_acid_mvcc.md` | ACID, isolation anomalies, xmin/xmax visibility, VACUUM, SSI, locks, deadlocks | 3 |
| 06 | `06_query_planning_and_explain.md` | Statistics, cost model, join methods, reading EXPLAIN, the login-query case study | 3 |
| 07 | `07_sql_for_backend_engineers.md` | Joins, CTEs, windows, upserts, set-based thinking, COPY, pagination, anti-patterns | 3 |
| 08 | `08_plpgsql_stored_procedures.md` | Functions vs procedures, exceptions, plan caching, SECURITY DEFINER, pgTAP, versioning | 3 |
| 09 | `09_schema_design_and_data_modelling.md` | 3NF, keys, constraints, loan-origination model, audit, soft delete, time zones | 2 |
| 10 | `10_berkeley_db_and_embedded_kv_stores.md` | Berkeley DB architecture and C API, LSM trees, RocksDB, where embedded stores fit | 3 |
| 11 | `11_yugabytedb_and_distributed_sql.md` | YSQL over DocDB over RocksDB, tablets, Raft, what changes vs PostgreSQL, cluster indexes | 3 |
| 12 | `12_migrating_a_kv_store_to_relational.md` | Full methodology: decode, map, stream, verify, cut over with zero downtime; C++17 skeleton | 4 |
| 13 | `13_caching_and_redis.md` | Cache patterns, stampede, invalidation, Redis structures, persistence, locks | 2 |
| 14 | `14_performance_tuning_and_operations.md` | pgbouncer, config, monitoring, vacuum tuning, backups, replication, Oracle/SQL Server table | 3 |

Total: about 40 hours. At 4 hours per week alongside other courses this is a 10-week course;
the modules are ordered so the first six give the highest interview return.

## Code folder

| File | Used by | Build / run |
|------|---------|-------------|
| `code/docker-compose.yml` | 11, 13 | `docker compose up -d` |
| `code/m01_libpq_hello.c` | 01 | `gcc m01_libpq_hello.c -o hello -lpq` |
| `code/m02_page_inspect.sql` | 02 | `psql loans -f ...` |
| `code/m03_index_demos.sql` | 03 | `psql loans -f ...` |
| `code/m04_wal_inspect.sh` | 04 | `bash m04_wal_inspect.sh` |
| `code/m05_isolation_session_a.sql`, `m05_isolation_session_b.sql` | 05 | two `psql` windows |
| `code/m05_deadlock_a.sql`, `m05_deadlock_b.sql` | 05 | two `psql` windows |
| `code/m06_slow_vs_fast_queries.sql` | 06 | `psql loans -f ...` |
| `code/m06_explain_login_query.sql` | 06 | `psql loans -f ...` (the login case study) |
| `code/m07_sql_patterns.sql` | 07 | `psql loans -f ...` |
| `code/plpgsql/m08_functions.sql`, `m08_procedures.sql`, `m08_workflow_rules.sql`, `m08_pgtap_tests.sql` | 08 | `psql loans -f ...` |
| `code/m09_schema.sql`, `m09_seed.sql` | 09 and all | `psql loans -f ...` |
| `code/m10_bdb_cursor_txn.c` | 10 | `gcc -Wall -Wextra m10_bdb_cursor_txn.c -o bdbdemo -ldb` |
| `code/m11_yugabyte_demo.sql` | 11 | `ysqlsh -f ...` (inside the container) |
| `code/m12_migration_engine.cpp` | 12 | `g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_migration_engine.cpp -o migrate` |
| `code/m12_packed_record_decode.cpp` | 12 | `g++ -std=c++17 -Wall -Wextra -Werror m12_packed_record_decode.cpp -o decode` |
| `code/m13_cache_aside.py`, `m13_redis_cli_demo.sh` | 13 | `python3 ...` / `bash ...` |
| `code/m14_monitoring_queries.sql` | 14 | `psql loans -f ...` |

## You are done when you can...

- [ ] Draw the layers of PostgreSQL from the wire protocol to the disk and say what each one owns.
- [ ] Describe an 8 KB heap page byte by byte: header, line pointers, tuples, special space.
- [ ] Explain why a B+tree index with 3 levels can find one row in 400 million with 4 page reads.
- [ ] Choose the column order of a composite index from a WHERE clause and justify it.
- [ ] Explain what happens between `COMMIT` returning and the data being on disk, including WAL, fsync, checkpoints and full-page writes.
- [ ] Given a row's `xmin`, `xmax` and a snapshot, say whether a transaction sees it and why.
- [ ] Name the five classic anomalies and which isolation level in PostgreSQL stops each.
- [ ] Read an `EXPLAIN (ANALYZE, BUFFERS)` plan top to bottom and point at the problem node.
- [ ] Write a PL/pgSQL procedure with exception handling, and say when logic belongs in the database.
- [ ] Write the Berkeley DB C code to open an environment, insert, iterate with a cursor, and commit a transaction, from memory.
- [ ] Explain how YugabyteDB turns a PostgreSQL row into DocDB keys and why hash sharding changes index design.
- [ ] Present a zero-downtime KV-to-relational migration with parity verification, and defend each decision.
- [ ] Explain cache-aside, the stampede problem, and why a Redis lock is not a correctness guarantee.
- [ ] Say honestly what differs between PostgreSQL, Oracle and SQL Server on isolation defaults and MVCC.

## How to study

Read section 1 and 2 of a module, then type (not paste) the SQL in section 3 into `psql` and
watch the output. Section 4 is the interview material: read it twice. Do at least the easy and
medium exercises. Finish with the module's interview questions out loud.
