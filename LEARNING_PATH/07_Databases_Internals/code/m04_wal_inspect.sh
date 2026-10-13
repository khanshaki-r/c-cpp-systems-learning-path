#!/usr/bin/env bash
# m04_wal_inspect.sh : watch the write-ahead log from the shell on WSL (module 04).
#
# Usage:
#   bash m04_wal_inspect.sh            # steps 1 to 7: WAL position, bytes per txn, FPIs, pg_waldump
#   bash m04_wal_inspect.sh --crash    # also step 8: kill -9 the server mid-transaction, watch redo
#
# Assumes the Ubuntu packaged PostgreSQL 16 ("main" cluster) and that your OS user is a database
# superuser (README setup: createuser --superuser $USER). Override with PGHOST/PGPORT/PGUSER and
# DB=... if your setup differs. pg_waldump and kill need sudo because pg_wal and the postmaster
# belong to the postgres OS user.

set -euo pipefail

DB="${DB:-loans}"
PSQL=(psql -X -q -v ON_ERROR_STOP=1 -d "$DB")
PGBIN="${PGBIN:-/usr/lib/postgresql/16/bin}"
PGDATA_DIR="$("${PSQL[@]}" -Atc 'SHOW data_directory')"
LOG="${LOG:-/var/log/postgresql/postgresql-16-main.log}"

echo "data directory: $PGDATA_DIR"

echo
echo "=== 1. Where is the end of WAL, and which segment file holds it?"
"${PSQL[@]}" -c "SELECT pg_current_wal_lsn(), pg_walfile_name(pg_current_wal_lsn());"
# An LSN like 1/32217F28 is a 64-bit byte position in the WAL stream: high 32 bits / low 32 bits.

echo
echo "=== 2. How many WAL bytes does one small UPDATE generate? (run twice)"
"${PSQL[@]}" <<'SQL'
SET search_path = loans, public;
-- accounts: one heap page, balance not indexed, so the update is HOT and touches ONE page.
CHECKPOINT;                                         -- the next change to each page writes a full-page image
SELECT pg_current_wal_lsn() AS before \gset
UPDATE accounts SET balance = balance + 1 WHERE id = 3;
SELECT pg_current_wal_lsn() - :'before'::pg_lsn AS wal_bytes_first_change_after_checkpoint;
SELECT pg_current_wal_lsn() AS before \gset
UPDATE accounts SET balance = balance - 1 WHERE id = 3;
SELECT pg_current_wal_lsn() - :'before'::pg_lsn AS wal_bytes_second_change;

-- The same on loan_applications, whose status column is in an index and which has 8 indexes:
CHECKPOINT;
SELECT pg_current_wal_lsn() AS before \gset
UPDATE loan_applications SET status = 'UNDER_REVIEW' WHERE id = 1;
SELECT pg_current_wal_lsn() - :'before'::pg_lsn AS wal_bytes_indexed_update_after_checkpoint;
SELECT pg_current_wal_lsn() AS before \gset
UPDATE loan_applications SET status = 'SUBMITTED' WHERE id = 1;
SELECT pg_current_wal_lsn() - :'before'::pg_lsn AS wal_bytes_indexed_update_again;
SQL
# accounts: the first number is ~8 KB (a full-page image of the heap page), the second ~100 bytes.
# loan_applications: tens of KB, then still several KB. A non-HOT update writes a new entry into
# EVERY index, and each index page touched for the first time since the checkpoint gets its own
# full-page image. The second update touches different index pages (a new status value lives in
# a different part of la_status_submitted_idx), so it pays FPIs again. Indexes cost WAL.

echo
echo "=== 3. Checkpoint, then count full-page images (FPI) caused by the next write"
"${PSQL[@]}" <<'SQL'
SET search_path = loans, public;
CHECKPOINT;
SELECT pg_stat_force_next_flush();                  -- backends flush WAL statistics lazily
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_wal;
UPDATE accounts SET balance = balance + 1 WHERE id = 3;
SELECT pg_stat_force_next_flush();
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_wal;
UPDATE accounts SET balance = balance - 1 WHERE id = 3;
SELECT pg_stat_force_next_flush();
SELECT wal_records, wal_fpi, wal_bytes FROM pg_stat_wal;
SQL
# wal_fpi rises by one after the first UPDATE (torn-page protection), not after the second.
# (The counters are server-wide: background activity can add a little.)

echo
echo "=== 4. Every page carries the LSN of its last change"
"${PSQL[@]}" <<'SQL'
SET search_path = loans, public;
SELECT (SELECT ctid FROM loan_applications WHERE id = 1) AS row_1_location;
SELECT lsn AS page_lsn, pg_current_wal_lsn() AS wal_end
FROM page_header(get_raw_page('loans.loan_applications',
                              ((SELECT ctid FROM loans.loan_applications WHERE id = 1)::text::point)[0]::int));
SQL
# During recovery, redo of a record is skipped when page_lsn >= record LSN: that comparison is
# what makes replay idempotent.

echo
echo "=== 5. Checkpoint state and settings"
"${PSQL[@]}" <<'SQL'
SELECT checkpoint_lsn, redo_lsn, timeline_id, checkpoint_time FROM pg_control_checkpoint();
SELECT name, setting, unit FROM pg_settings
WHERE name IN ('checkpoint_timeout', 'max_wal_size', 'min_wal_size', 'checkpoint_completion_target',
               'wal_level', 'full_page_writes', 'synchronous_commit', 'wal_compression', 'archive_mode');
SQL

echo
echo "=== 6. Checkpoint activity since the last stats reset"
"${PSQL[@]}" <<'SQL'
-- PostgreSQL 16: pg_stat_bgwriter. In 17 these moved to pg_stat_checkpointer.
SELECT checkpoints_timed, checkpoints_req, buffers_checkpoint, buffers_clean, buffers_backend
FROM pg_stat_bgwriter;
SQL
# checkpoints_req much larger than checkpoints_timed means max_wal_size is too small for the
# write rate: checkpoints are forced by WAL volume instead of happening on schedule.

echo
echo "=== 7. Decode the WAL records of the last transactions with pg_waldump (needs sudo)"
SEG="$("${PSQL[@]}" -Atc "SELECT pg_walfile_name(pg_current_wal_lsn())")"
echo "segment: $SEG"
sudo "$PGBIN/pg_waldump" -p "$PGDATA_DIR/pg_wal" "$SEG" 2>/dev/null | tail -20 || true
# Expect lines like:
#   rmgr: Heap  len (rec/tot): 54/8266, tx: 812, lsn: 0/01A2B3C8, prev 0/01A2B390,
#     desc: UPDATE off 3 xmax 812 ... ; new off 9 xmax 0, blkref #0: rel 1663/16384/16432 blk 0 FPW
#   rmgr: Btree ... desc: INSERT_LEAF off 41, blkref #0: rel 1663/16384/16440 blk 2201
#   rmgr: Transaction len (rec/tot): 34/34, ... desc: COMMIT 2026-10-03 ...
# "FPW" marks a full-page image. The pg_waldump error about an invalid record length at the end
# is normal: it is where the WAL currently ends.

if [[ "${1:-}" != "--crash" ]]; then
    echo
    echo "Skipping step 8 (crash test). Re-run with --crash on a DEV machine to see redo."
    exit 0
fi

echo
echo "=== 8. Crash test: kill -9 the postmaster mid-transaction, then watch redo in the log"
COUNT_SQL="SELECT count(*) FROM loans.loan_applications WHERE id < 100 AND status = 'UNDER_REVIEW'"
echo "UNDER_REVIEW rows with id < 100 before the crash: $("${PSQL[@]}" -Atc "$COUNT_SQL")"
# A valid status is used so the UPDATE runs (an invalid one fails the foreign key immediately).
psql -X -d "$DB" -c "BEGIN; UPDATE loans.loan_applications SET status = 'UNDER_REVIEW' WHERE id < 100; SELECT pg_sleep(30); COMMIT;" &
sleep 2
sudo kill -9 "$(sudo head -1 "$PGDATA_DIR/postmaster.pid")"
wait || true
sleep 1
sudo service postgresql start
sleep 3
sudo tail -30 "$LOG"
# Expect: "database system was interrupted; last known up at ...", "redo starts at 0/...",
# "redo done at 0/...", "database system is ready to accept connections".
echo "UNDER_REVIEW rows with id < 100 after recovery:  $("${PSQL[@]}" -Atc "$COUNT_SQL")   (same as before)"
# The uncommitted UPDATE is simply not there. Redo replayed its records, but its xid has no
# COMMIT record, so MVCC treats those tuple versions as aborted: no undo pass is needed.
