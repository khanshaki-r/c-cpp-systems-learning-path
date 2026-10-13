-- m02_page_inspect.sql : pages, tuples, TOAST, alignment, buffer pool, FSM and VM (module 02).
--
-- Run:  psql loans -f m02_page_inspect.sql        (after m09_schema.sql and m09_seed.sql)
-- Better: open psql and paste one numbered step at a time, reading each output before moving on.
-- Extensions used (created by m09_schema.sql): pageinspect, pg_buffercache, pg_visibility.

\set ON_ERROR_STOP on
\pset pager off
SET search_path = loans, public;

\echo '--- 1. Where is the table on disk, and how many 8 KB pages in each fork?'
SELECT pg_relation_filepath('loan_applications')                 AS file,
       pg_relation_size('loan_applications') / 8192               AS main_pages,
       pg_relation_size('loan_applications', 'fsm') / 8192        AS fsm_pages,
       pg_relation_size('loan_applications', 'vm')  / 8192        AS vm_pages;
-- file is relative to the data directory: base/<database oid>/<relfilenode>. Files over 1 GB
-- continue as <relfilenode>.1, .2 ...

\echo '--- 2. The 24-byte page header of page 0'
SELECT * FROM page_header(get_raw_page('loan_applications', 0));
-- lower = end of the line pointer array, upper = start of tuple data.
-- Free bytes in the middle of the page = upper - lower.
-- lsn = WAL position of the last change to this page (module 04).

\echo '--- 3. Line pointers and tuple headers on page 0 (first 10)'
SELECT lp, lp_off, lp_len, lp_flags, t_xmin, t_xmax, t_ctid, t_hoff,
       (t_infomask & 1)      <> 0 AS has_null,        -- HEAP_HASNULL: a null bitmap follows the header
       (t_infomask & 16384)  <> 0 AS hot_updated,     -- HEAP_HOT_UPDATED (0x4000)
       (t_infomask2 & 32768) <> 0 AS heap_only        -- HEAP_ONLY_TUPLE (0x8000)
FROM heap_page_items(get_raw_page('loan_applications', 0))
ORDER BY lp
LIMIT 10;
-- lp_flags: 1 = LP_NORMAL, 2 = LP_REDIRECT, 3 = LP_DEAD, 0 = LP_UNUSED.
-- t_hoff = header length incl. null bitmap, padded to 8 bytes (24 without nulls).

-- A note on step 3's output: t_xmax is often NON-zero on live rows of a freshly seeded table.
-- That is a lock-only xmax (infomask bit 0x80, HEAP_XMAX_LOCK_ONLY) left by the FOR KEY SHARE
-- row locks the foreign-key checks took when documents, payments and audit rows were inserted
-- (module 09, "FKs are system triggers"). A lock is recorded in the tuple, not in a lock table.
SELECT lp, t_xmax, (t_infomask & 128) <> 0 AS xmax_lock_only
FROM heap_page_items(get_raw_page('loan_applications', 0))
ORDER BY lp
LIMIT 3;

\echo '--- 4. An UPDATE creates a new tuple version (row id 1 is on page 0)'
-- Inside a transaction, so the old version cannot be pruned away before you look at it,
-- and so ROLLBACK leaves the data as the other modules expect.
BEGIN;
SELECT ctid, id, status FROM loan_applications WHERE id = 1;
UPDATE loan_applications SET status = 'UNDER_REVIEW' WHERE id = 1;
SELECT ctid, id, status, pg_current_xact_id() AS my_xid FROM loan_applications WHERE id = 1;
SELECT lp, t_xmin, t_xmax, t_ctid,
       (t_infomask & 16384)  <> 0 AS hot_updated,
       (t_infomask2 & 32768) <> 0 AS heap_only
FROM heap_page_items(get_raw_page('loan_applications', 0))
WHERE lp <= 3
ORDER BY lp;
ROLLBACK;
-- Expect: lp=1 has t_xmax = my_xid and t_ctid pointing at the new version's (page, slot).
-- status is in an index (la_status_submitted_idx), so this update cannot be HOT: hot_updated
-- is false and every index on the table got a new entry. Page 0 is full after the seed, so the
-- new version lands on another page (often the last one, found through the FSM).
-- Outside a transaction the dead old version is pruned by the next reader of the page
-- ("opportunistic pruning"), and lp=1 shows as an empty slot: try it and see.

\echo '--- 5. TOAST: big values move out of line'
-- Give 20 documents a large, poorly compressible OCR text (~13 KB each).
UPDATE documents
SET ocr_text = (SELECT string_agg(md5(random()::text || g::text), '') FROM generate_series(1, 400) g)
WHERE id <= 20;
SELECT reltoastrelid::regclass AS toast_table FROM pg_class WHERE oid = 'documents'::regclass \gset
\echo 'TOAST table is' :toast_table
SELECT chunk_id, count(*) AS chunks, sum(length(chunk_data)) AS bytes
FROM :toast_table
GROUP BY chunk_id
ORDER BY chunk_id
LIMIT 5;
-- Each value is cut into chunks of about 2 KB (TOAST_MAX_CHUNK_SIZE = 1996 bytes on 8 KB pages);
-- the main tuple keeps an 18-byte pointer. pg_column_size shows the stored (compressed) size:
SELECT id, pg_column_size(ocr_text) AS stored_bytes, length(ocr_text) AS chars
FROM documents WHERE id <= 3 ORDER BY id;

\echo '--- 6. Alignment padding: same data, two column orders'
DROP TABLE IF EXISTS pad_bad, pad_good;
CREATE TABLE pad_bad  (a int8, b bool, c int8, d bool, e int8);
CREATE TABLE pad_good (a int8, c int8, e int8, b bool, d bool);
INSERT INTO pad_bad  SELECT g, true, g, false, g FROM generate_series(1, 100000) g;
INSERT INTO pad_good SELECT g, g, g, true, false FROM generate_series(1, 100000) g;
SELECT pg_size_pretty(pg_relation_size('pad_bad'))  AS bad,
       pg_size_pretty(pg_relation_size('pad_good')) AS good,
       (SELECT avg(pg_column_size(t.*)) FROM pad_bad t)  AS bad_row_bytes,
       (SELECT avg(pg_column_size(t.*)) FROM pad_good t) AS good_row_bytes;
-- bad: each bool is followed by 7 padding bytes so the next int8 starts on an 8-byte boundary.

\echo '--- 7. Buffer pool: what is cached, how dirty, how hot'
SELECT c.relname, count(*) AS buffers,
       pg_size_pretty(count(*) * 8192) AS cached,
       sum(CASE WHEN b.isdirty THEN 1 ELSE 0 END) AS dirty,
       round(avg(b.usagecount), 2) AS avg_usage
FROM pg_buffercache b
JOIN pg_class c ON b.relfilenode = pg_relation_filenode(c.oid)
WHERE b.reldatabase = (SELECT oid FROM pg_database WHERE datname = current_database())
  AND c.relname IN ('loan_applications', 'customers', 'app_users', 'app_users_login_key')
GROUP BY c.relname
ORDER BY buffers DESC;
-- usagecount (0..5) is the clock-sweep counter: bumped on access, decremented by the sweep.

\echo '--- 8. Free space map: how much room each page advertises'
SELECT blkno, avail FROM pg_freespace('loan_applications') ORDER BY blkno LIMIT 10;
-- FSM values are approximate (1/256 of a page granularity) and updated by VACUUM, not by every insert.

\echo '--- 9. Visibility map bits'
SELECT blkno, all_visible, all_frozen, pd_all_visible
FROM pg_visibility('loan_applications')
ORDER BY blkno
LIMIT 10;
-- Page 0 lost all_visible in step 4 (an UPDATE touched it, even though it rolled back).
-- A plain VACUUM may NOT set it again: when only a handful of dead items exist, PostgreSQL 14+
-- skips the index pass (INDEX_CLEANUP auto, "bypass" below ~2% of pages), so the dead line
-- pointer (lp_flags = 3, LP_DEAD) stays and the page is not all-visible. Watch:
VACUUM loan_applications;
SELECT blkno, all_visible, all_frozen FROM pg_visibility('loan_applications') WHERE blkno = 0;
SELECT lp, lp_flags FROM heap_page_items(get_raw_page('loan_applications', 0)) WHERE lp_flags <> 1;
-- Forcing the index pass removes the LP_DEAD item and sets the bits (and, here, freezes the page):
VACUUM (INDEX_CLEANUP ON) loan_applications;
SELECT blkno, all_visible, all_frozen FROM pg_visibility('loan_applications') WHERE blkno = 0;

DROP TABLE pad_bad, pad_good;
