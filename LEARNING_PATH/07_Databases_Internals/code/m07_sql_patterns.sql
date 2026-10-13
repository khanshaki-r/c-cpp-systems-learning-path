-- m07_sql_patterns.sql : SQL for backend engineers on the loan schema (module 07).
--
-- Run:  psql loans -f m07_sql_patterns.sql > m07_out.txt     or paste section by section.
-- Everything that modifies data runs inside BEGIN ... ROLLBACK, so the seed stays as it was.
-- Expected plan shapes are in the comments; EXPLAIN without ANALYZE where the run would be slow.

\set ON_ERROR_STOP on
\pset pager off
SET search_path = loans, public;

-- ===========================================================================================
\echo '=== 1. Joins: the LEFT JOIN trap'
-- ===========================================================================================
-- WRONG: the WHERE on the right table turns the LEFT JOIN into an INNER JOIN.
SELECT count(*) AS wrong_count
FROM loan_applications a
LEFT JOIN documents d ON d.application_id = a.id
WHERE d.kind = 'ID_PROOF' AND a.branch_code = 'BR0001';
-- RIGHT: the condition belongs in ON; applications without an ID proof are kept with NULLs.
SELECT count(*) AS right_count, count(d.id) AS with_id_proof
FROM loan_applications a
LEFT JOIN documents d ON d.application_id = a.id AND d.kind = 'ID_PROOF'
WHERE a.branch_code = 'BR0001';
-- Anti-join form: applications with NO ID proof.
SELECT count(*) AS missing_id_proof
FROM loan_applications a
WHERE a.branch_code = 'BR0001'
  AND NOT EXISTS (SELECT 1 FROM documents d WHERE d.application_id = a.id AND d.kind = 'ID_PROOF');

-- ===========================================================================================
\echo '=== 2. Fan-out trap and its fix'
-- ===========================================================================================
-- WRONG: documents x payments multiplies rows, so sum(p.amount) is multiplied by the doc count.
SELECT a.id, count(d.id) AS docs, sum(p.amount) AS paid
FROM loan_applications a
LEFT JOIN documents d ON d.application_id = a.id
LEFT JOIN payments  p ON p.application_id = a.id
WHERE a.id BETWEEN 1 AND 20 AND a.status = 'CLOSED'
GROUP BY a.id ORDER BY a.id LIMIT 5;
-- RIGHT: aggregate each child separately, then join the aggregates.
SELECT a.id, coalesce(d.docs, 0) AS docs, coalesce(p.paid, 0) AS paid
FROM loan_applications a
LEFT JOIN (SELECT application_id, count(*) AS docs FROM documents GROUP BY application_id) d ON d.application_id = a.id
LEFT JOIN (SELECT application_id, sum(amount) AS paid FROM payments GROUP BY application_id) p ON p.application_id = a.id
WHERE a.id BETWEEN 1 AND 20 AND a.status = 'CLOSED'
ORDER BY a.id LIMIT 5;
-- (The planner pushes a.id BETWEEN ... only into the outer scan; for a big report write the
-- children's aggregates with the same filter, or use LATERAL as in section 3.)

-- ===========================================================================================
\echo '=== 3. LATERAL: latest 3 applications for each of 5 customers'
-- ===========================================================================================
EXPLAIN (COSTS OFF)
SELECT c.id, c.full_name, la.application_no, la.submitted_at
FROM customers c
CROSS JOIN LATERAL (
    SELECT application_no, submitted_at
    FROM loan_applications a
    WHERE a.customer_id = c.id
    ORDER BY submitted_at DESC
    LIMIT 3
) la
WHERE c.id <= 5;
-- Nested Loop -> Index Scan on customers_pkey, then per customer a Limit over an Index Scan
-- using la_customer_submitted_idx (customer_id, submitted_at DESC): no Sort.

-- ===========================================================================================
\echo '=== 4. Latest application per customer, two idioms'
-- ===========================================================================================
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (customer_id) customer_id, application_no, submitted_at
FROM loan_applications
ORDER BY customer_id, submitted_at DESC;
EXPLAIN (COSTS OFF)
SELECT customer_id, application_no, submitted_at
FROM (SELECT customer_id, application_no, submitted_at,
             row_number() OVER (PARTITION BY customer_id ORDER BY submitted_at DESC) AS rn
      FROM loan_applications) x
WHERE rn = 1;
-- Both: Index Scan using la_customer_submitted_idx feeding Unique / WindowAgg, no Sort node,
-- because the index order (customer_id, submitted_at DESC) is exactly the required order.
-- (NULL submitted_at sorts FIRST in DESC order: drafts count as "latest". Add NULLS LAST to the
-- query AND the index if that is wrong for the business.)

-- ===========================================================================================
\echo '=== 5. Window functions: running total, previous application, difference from average'
-- ===========================================================================================
SELECT a.customer_id, a.application_no, a.submitted_at::date AS submitted, a.requested_amount,
       row_number() OVER w                                       AS nth_for_customer,
       sum(a.requested_amount) OVER (w ROWS UNBOUNDED PRECEDING) AS running_total,
       lag(a.submitted_at::date) OVER w                          AS previous_submitted,
       round(a.requested_amount - avg(a.requested_amount) OVER (PARTITION BY a.customer_id), 2) AS vs_customer_avg
FROM loan_applications a
WHERE a.customer_id IN (42, 43)
WINDOW w AS (PARTITION BY a.customer_id ORDER BY a.submitted_at)
ORDER BY a.customer_id, a.submitted_at;

-- ===========================================================================================
\echo '=== 6. CTEs: inlined vs materialised'
-- ===========================================================================================
EXPLAIN (COSTS OFF)
WITH recent AS (
    SELECT customer_id, count(*) AS n
    FROM loan_applications
    WHERE submitted_at > TIMESTAMPTZ '2026-08-01 00:00+00'
    GROUP BY customer_id
)
SELECT c.full_name, r.n FROM recent r JOIN customers c ON c.id = r.customer_id WHERE r.n >= 1;
-- Inlined (referenced once, no side effects): the range predicate reaches la_submitted_id_idx.
EXPLAIN (COSTS OFF)
WITH apps AS MATERIALIZED (SELECT * FROM loan_applications)
SELECT * FROM apps WHERE id = 42;
-- MATERIALIZED is a fence: "CTE Scan on apps, Filter: (id = 42)" over a full Seq Scan. The
-- primary key cannot be used through the fence. This is what every CTE did before PostgreSQL 12.

-- ===========================================================================================
\echo '=== 7. Recursive CTE over the workflow graph, with a cycle in it'
-- ===========================================================================================
-- UNDER_REVIEW -> SUBMITTED -> UNDER_REVIEW is a cycle. Two ways to stop:
-- (a) the depth bound from the module text
WITH RECURSIVE chain AS (
    SELECT id, from_state, to_state, 1 AS depth FROM workflow_transitions WHERE from_state = 'DRAFT'
    UNION ALL
    SELECT t.id, t.from_state, t.to_state, chain.depth + 1
    FROM workflow_transitions t JOIN chain ON t.from_state = chain.to_state
    WHERE chain.depth < 6                      -- always bound recursion
)
SELECT to_state, min(depth) AS shortest_path_length, count(*) AS paths_seen
FROM chain GROUP BY to_state ORDER BY 2, 1;
-- (b) PostgreSQL 14+ CYCLE clause: stop when a state repeats on the current path
WITH RECURSIVE reach (state, depth) AS (
    SELECT 'DRAFT'::text, 0
    UNION ALL
    SELECT t.to_state, r.depth + 1
    FROM workflow_transitions t JOIN reach r ON t.from_state = r.state
) CYCLE state SET is_cycle USING path
SELECT state, min(depth) AS shortest FROM reach WHERE NOT is_cycle GROUP BY state ORDER BY 2, 1;

-- ===========================================================================================
\echo '=== 8. Upserts'
-- ===========================================================================================
BEGIN;
INSERT INTO customers (national_id, full_name, email, date_of_birth, home_branch)
VALUES ('NID0000000042', 'Customer 42 (renamed)', 'cust42.new@example.com', DATE '1980-01-01', 'BR0042')
ON CONFLICT (national_id) DO UPDATE
    SET full_name  = EXCLUDED.full_name,
        email      = EXCLUDED.email,
        updated_at = now()
    WHERE customers.email IS DISTINCT FROM EXCLUDED.email      -- skip no-op updates (saves WAL and HOT chains)
RETURNING id, (xmax = 0) AS inserted;                           -- false: it was an update
INSERT INTO customers (national_id, full_name, email, date_of_birth, home_branch)
VALUES ('NID9999999999', 'Brand New', 'brand.new@example.com', DATE '1990-05-05', 'BR0001')
ON CONFLICT (national_id) DO NOTHING
RETURNING id, (xmax = 0) AS inserted;                           -- true: a fresh row
ROLLBACK;

-- ===========================================================================================
\echo '=== 9. Batch fetch instead of N+1'
-- ===========================================================================================
PREPARE fetch_apps(bigint[]) AS
SELECT id, application_no, status FROM loan_applications WHERE id = ANY($1);
EXECUTE fetch_apps(ARRAY[1, 2, 3, 4, 5]);
EXPLAIN (COSTS OFF) EXECUTE fetch_apps(ARRAY[1, 2, 3, 4, 5]);
-- Index Scan using loan_applications_pkey, Index Cond: (id = ANY ('{1,2,3,4,5}'::bigint[]))
-- One round trip, one plan, for any number of ids. From libpq: bind the text '{1,2,3}'.
DEALLOCATE fetch_apps;

-- ===========================================================================================
\echo '=== 10. Keyset pagination: three pages, cursor carried in psql variables'
-- ===========================================================================================
SELECT id, submitted_at FROM loan_applications
WHERE submitted_at IS NOT NULL
ORDER BY submitted_at DESC, id DESC LIMIT 3;
SELECT submitted_at AS c_ts, id AS c_id FROM (
    SELECT id, submitted_at FROM loan_applications WHERE submitted_at IS NOT NULL
    ORDER BY submitted_at DESC, id DESC LIMIT 3) p
ORDER BY submitted_at, id LIMIT 1 \gset
\echo 'next page after' :c_ts :c_id
SELECT id, submitted_at FROM loan_applications
WHERE (submitted_at, id) < (:'c_ts'::timestamptz, :c_id)
ORDER BY submitted_at DESC, id DESC LIMIT 3;
EXPLAIN (COSTS OFF)
SELECT id, submitted_at FROM loan_applications
WHERE (submitted_at, id) < (TIMESTAMPTZ '2026-09-01 00:00+00', 500000)
ORDER BY submitted_at DESC, id DESC LIMIT 20;
-- Limit -> Index Only Scan Backward using la_submitted_id_idx,
--          Index Cond: (ROW(submitted_at, id) < ROW(...))

-- ===========================================================================================
\echo '=== 11. Set-based UPDATE replacing a cursor loop (module 07 section 2.5)'
-- ===========================================================================================
BEGIN;
EXPLAIN (ANALYZE, COSTS OFF)
UPDATE loan_applications a
SET status = 'READY_FOR_REVIEW', updated_at = now()
FROM customers c
WHERE c.id = a.customer_id AND a.branch_code = 'BR0001' AND a.status = 'SUBMITTED'
  AND c.kyc_verified
  AND (SELECT count(*) FROM documents d WHERE d.application_id = a.id) >= 2;
-- One statement: Update on loan_applications -> Nested Loop (index on branch_code, status, then
-- customers_pkey) with a SubPlan per candidate row counting documents through
-- documents_application_idx. Compare with 3 round trips per row from a C loop.
ROLLBACK;

-- ===========================================================================================
\echo '=== 12. Data-modifying CTE: move rows atomically (inside a rolled-back transaction)'
-- ===========================================================================================
BEGIN;
WITH moved AS (
    DELETE FROM loan_applications a
    WHERE a.status = 'WITHDRAWN' AND a.updated_at < TIMESTAMPTZ '2020-03-01 00:00+00'
      AND NOT EXISTS (SELECT 1 FROM application_audit x WHERE x.application_id = a.id)
      AND NOT EXISTS (SELECT 1 FROM payments p WHERE p.application_id = a.id)
    RETURNING *
)
INSERT INTO loan_applications_archive SELECT * FROM moved;
-- Seeded withdrawn rows all have audit rows, so this moves 0 rows: the FK from
-- application_audit would refuse the delete otherwise. sp_archive_withdrawn (module 08) moves
-- the audit rows too. Row count 0 here is the point: read your FKs before deleting.
ROLLBACK;

-- ===========================================================================================
\echo '=== 13. Bulk load: multi-row INSERT vs COPY into an unlogged staging table'
-- ===========================================================================================
DROP TABLE IF EXISTS stage_apps;
CREATE UNLOGGED TABLE stage_apps (application_no text, customer_id bigint, requested_amount numeric(18,2), status text);
\timing on
INSERT INTO stage_apps
SELECT 'LA-2030-' || lpad(g::text, 6, '0'), 1 + g % 200000, 100000, 'DRAFT' FROM generate_series(1, 100000) g;
\copy (SELECT * FROM stage_apps) TO '/tmp/m07_stage_apps.csv' CSV HEADER
TRUNCATE stage_apps;
\copy stage_apps FROM '/tmp/m07_stage_apps.csv' CSV HEADER
\timing off
SELECT count(*) FROM stage_apps;
DROP TABLE stage_apps;

-- ===========================================================================================
\echo '=== 14. Anti-patterns, measured'
-- ===========================================================================================
-- count(*) to test existence vs EXISTS
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) > 0 FROM loan_applications WHERE status = 'CLOSED';
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT EXISTS (SELECT 1 FROM loan_applications WHERE status = 'CLOSED');
-- The first counts ~400k rows; the second stops at the first one (actual rows=1).

-- OR across two columns vs UNION ALL
EXPLAIN (COSTS OFF)
SELECT id FROM loan_applications WHERE customer_id = 42 OR application_no = 'LA-2024-000123';
-- PostgreSQL can often do BitmapOr of two index scans; other engines (and older planners) seq scan.
-- The UNION form is always index-driven (UNION, not UNION ALL, removes the overlap):
EXPLAIN (COSTS OFF)
SELECT id FROM loan_applications WHERE customer_id = 42
UNION
SELECT id FROM loan_applications WHERE application_no = 'LA-2024-000123';

-- Leading wildcard: no B-tree can help
EXPLAIN (COSTS OFF) SELECT id FROM customers WHERE full_name LIKE '%4242';
-- Seq Scan. Fix: CREATE EXTENSION pg_trgm; CREATE INDEX ... USING gin (full_name gin_trgm_ops);
