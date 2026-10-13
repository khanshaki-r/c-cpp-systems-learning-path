# code/ : runnable examples for 07_Databases_Internals

Everything here targets **WSL Ubuntu 24.04** with the apt packages for PostgreSQL 16, GCC 13 and
Python 3.12. Paths below are relative to this `code/` folder. On WSL the folder is reachable as
`/mnt/d/Cpp_DSA/LEARNING_PATH/07_Databases_Internals/code`.

## 1. One-time setup (about 30 minutes)

```bash
# PostgreSQL 16 server + client + headers (24.04 ships 16 in its default repos)
sudo apt update
sudo apt install -y postgresql-16 postgresql-contrib-16 postgresql-client-16 libpq-dev
sudo service postgresql start
sudo -u postgres createuser --superuser "$USER"
createdb loans

# pg_stat_statements and auto_explain must be preloaded (module 14 explains why):
sudo sed -i "s/^#\?shared_preload_libraries.*/shared_preload_libraries = 'pg_stat_statements,auto_explain'/" \
    /etc/postgresql/16/main/postgresql.conf
echo "track_io_timing = on" | sudo tee -a /etc/postgresql/16/main/postgresql.conf
sudo service postgresql restart

# C and C++ toolchain, Berkeley DB headers, pgTAP
sudo apt install -y build-essential g++ libdb-dev postgresql-16-pgtap libtap-parser-sourcehandler-pgtap-perl

# Docker inside WSL for YugabyteDB and Redis (the compose plugin is a separate package)
sudo apt install -y docker.io docker-compose-v2 redis-tools
sudo usermod -aG docker "$USER"        # log out and back in to WSL once
sudo service docker start

# Python for module 13
sudo apt install -y python3-venv
python3 -m venv ~/venv-db && . ~/venv-db/bin/activate
pip install "redis>=5" "psycopg[binary]>=3.1"
```

## 2. Load the schema and data (every module needs this)

```bash
psql loans -f m09_schema.sql      # seconds; drops and recreates schema "loans"
psql loans -f m09_seed.sql        # 5 to 15 minutes; ~10 M rows across all tables
```

`m09_schema.sql` sets `search_path = loans, public` as the database default, so `psql loans` then
`SELECT count(*) FROM loan_applications;` works without a schema prefix.

The seed is deterministic (`setseed`), so the rows the modules quote exist for everyone:
`LA-2024-000123`, user `('BR0456', 'U00123456')`, customer email `anna.k@example.com`,
application id 1 in status `SUBMITTED`, accounts 1 and 2 with 1000 and 500.

Name map, if you look for the generic names: `schema.sql` = `m09_schema.sql`,
`seed.sql` = `m09_seed.sql`, the index demo = `m03_index_demos.sql`, the login EXPLAIN case
study = `m06_explain_login_query.sql`.

## 3. File by file

| File | Module | How to run | What you should see |
|------|--------|------------|---------------------|
| `m09_schema.sql` | 09, all | `psql loans -f m09_schema.sql` | `tables_created` = 25 |
| `m09_seed.sql` | 09, all | `psql loans -f m09_seed.sql` | row counts per table, status distribution |
| `m01_libpq_hello.c` | 01 | `gcc -Wall -Wextra m01_libpq_hello.c -o hello -I/usr/include/postgresql -lpq && ./hello` | one row, its plan, SQLSTATE 42P01 |
| `m02_page_inspect.sql` | 02 | `psql loans -f m02_page_inspect.sql` | page header, line pointers, TOAST chunks, padding sizes, VM bits |
| `m03_index_demos.sql` | 03 | `psql loans -f m03_index_demos.sql` (own schema `m03`) | Seq Scan -> Index Only Scan, tree height, HOT counts, REINDEX shrink |
| `m04_wal_inspect.sh` | 04 | `bash m04_wal_inspect.sh` (asks for sudo for pg_waldump) | WAL bytes with and without FPI, pg_waldump records |
| `m04_wal_inspect.sh --crash` | 04 | dev machine only | kill -9 of the server, redo in the log, uncommitted rows absent |
| `m05_isolation_session_a.sql` + `_b.sql` | 05 | two `psql loans` windows, paste step by step | non-repeatable read, 40001, lost update, write skew |
| `m05_deadlock_a.sql` + `_b.sql` | 05 | two windows | `deadlock detected` (40P01), then the ordered-locking fix |
| `m06_slow_vs_fast_queries.sql` | 06 | `psql loans -f ... > plans.txt` | 8 slow/fast plan pairs with comments |
| `m06_explain_login_query.sql` | 06 | `psql loans -f ...` (own schema `m06`) | 26,000 buffers -> 5 -> 4, Heap Fetches: 0 |
| `m07_sql_patterns.sql` | 07 | `psql loans -f ...` | joins, LATERAL, windows, recursive CTE, upserts, keyset pages |
| `plpgsql/m08_functions.sql` | 08 | `psql loans -f plpgsql/m08_functions.sql` | validation, authorisation, login functions |
| `plpgsql/m08_procedures.sql` | 08 | after the functions | `sp_record_login`, `sp_archive_withdrawn` |
| `plpgsql/m08_workflow_rules.sql` | 08 | after the procedures | `sp_transition_application`, LN403 / LN400 errors in a walk-through |
| `plpgsql/m08_pgtap_tests.sql` | 08 | `psql loans -c "create extension pgtap"`; `pg_prove -d loans plpgsql/m08_pgtap_tests.sql` | `ok 1` .. `ok 12`, database unchanged |
| `m10_bdb_cursor_txn.c` | 10 | `gcc -Wall -Wextra -O2 m10_bdb_cursor_txn.c -o bdbdemo -ldb && ./bdbdemo` | inserts, cursor scan, secondary lookup; run twice for DB_KEYEXIST |
| `m11_yugabyte_demo.sql` | 11 | `docker compose up -d yugabyte`, then `docker exec -it yb bin/ysqlsh -h yb -f /code/m11_yugabyte_demo.sql` | Storage Read Requests per tablet, before and after the index |
| `m12_migration_engine.cpp` | 12 | `g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_migration_engine.cpp -o migrate && ./migrate` | crash after batch 3, resume, injected 40001 retry, `PARITY OK` |
| same, `--float-bug` | 12 | `./migrate --float-bug` | parity catches money converted through float |
| same, real sink | 12 | `g++ ... -DHAVE_LIBPQ -I/usr/include/postgresql m12_migration_engine.cpp -o migrate_pg -lpq && ./migrate_pg --libpq "dbname=loans"` | 23,456 records loaded with COPY, checkpoints in `migration_checkpoints` |
| `m12_packed_record_decode.cpp` | 12 | `g++ -std=c++17 -Wall -Wextra -Werror m12_packed_record_decode.cpp -o decode && ./decode` | `19/19 tests passed` |
| `m13_cache_aside.py` | 13 | `docker compose up -d redis`; `python3 m13_cache_aside.py demo` / `stampede 200` / `stampede 200 --no-lock` | hit stats; 1 DB load with the lock vs ~200 without |
| `m13_redis_cli_demo.sh` | 13 | `bash m13_redis_cli_demo.sh` | SET NX, owner-checked release, pipe, SCAN, WATCH abort |
| `m14_monitoring_queries.sql` | 14 | `psql loans -f ...` | settings, top statements, blockers, vacuum, wraparound, replication |
| `docker-compose.yml` | 11, 13, 14 | `docker compose up -d` | `pg16` on 5434, `yb` on 5433, `redis` on 6379 |

### Checking the parity functions against the engine

After `./migrate_pg --libpq "dbname=loans"`, the SQL implementation of the canonical form and
FNV-1a hash must agree with the C++ one, record by record:

```sql
SELECT count(*) AS checked,
       count(*) FILTER (WHERE fn_parity_hash_loan(a.id) <> p.hash) AS mismatches
FROM migration_parity p JOIN loan_applications a USING (application_no)
WHERE random() < 0.05;            -- fn_fnv1a64 is numeric arithmetic: sample, do not scan 23k rows
```

Expected: `mismatches = 0`. Rejects (corrupt fake records) are in `migration_rejects` with their raw
bytes; the latest checkpoint is `SELECT * FROM migration_checkpoints ORDER BY records_done DESC LIMIT 1`.
The fake loan numbers (`LA-YYYY-9NNNNN`, product `BIZ`) never collide with seeded rows.

### Resetting

- Everything: `psql loans -f m09_schema.sql && psql loans -f m09_seed.sql`.
- Demo leftovers: `DROP SCHEMA m03 CASCADE; DROP SCHEMA m06 CASCADE;`
- Berkeley DB: `rm -rf bdb_env` in the folder you ran `bdbdemo` from (run it from a scratch
  directory, e.g. `mkdir -p ~/bdb && cd ~/bdb && /path/to/bdbdemo`, to keep this folder clean).
- Docker: `docker compose down -v`.

## 4. What was verified, and how

Verified on WSL Ubuntu 24.04, PostgreSQL 16.15, GCC 13 (2026-10-12):

- `m09_schema.sql` + `m09_seed.sql` loaded into a fresh cluster (about 14 minutes under WSL 2).
- `m01`, `m02`, `m03`, `m04` (without `--crash`), `m06` (both), `m07`, `m14` run end to end.
- `plpgsql/*` loaded; `m08_pgtap_tests.sql` passes 12/12 against pgTAP 1.3.2.
- `m10_bdb_cursor_txn.c` built against libdb 5.3 with `-Wall -Wextra`, no warnings, run twice.
- `m12_migration_engine.cpp` and `m12_packed_record_decode.cpp` build with
  `-std=c++17 -Wall -Wextra -Werror -pthread`, with and without `-DHAVE_LIBPQ`; the libpq sink
  loaded 23,456 records and a SQL parity sample showed 0 mismatches.

Written to the same standard but not executed at the time of writing (no Docker daemon): the
two-session `m05` scripts (paste-by-hand by design), `m11_yugabyte_demo.sql`,
`m13_cache_aside.py` (syntax-checked), `m13_redis_cli_demo.sh` (`bash -n` checked),
`docker-compose.yml` (YAML parsed; image tags confirmed to exist on Docker Hub). If a YugabyteDB
version prints EXPLAIN (DIST) lines slightly differently, compare the request counts, not the text.
