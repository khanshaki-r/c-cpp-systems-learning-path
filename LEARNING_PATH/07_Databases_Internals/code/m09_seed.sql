-- m09_seed.sql : realistic volume for the loan-origination schema, generated with generate_series.
--
-- Run after m09_schema.sql:   psql loans -f m09_seed.sql
-- Volume (measured: about 15 minutes under WSL 2 on a laptop; most of it is FK checks and index
-- maintenance on the already-created indexes):
--     500 branches, 6 products, 200,000 customers, 2,000,000 users (+ roles),
--     1,000,000 applications, ~1.9 M documents, ~2.2 M payments, ~1.9 M audit rows, ~2.2 M user_roles.
--
-- Deterministic: setseed() makes random() repeatable, so everyone gets the same data and the
-- row counts quoted in the modules match. Specific rows the modules query:
--     application_no 'LA-2024-000123' exists           (module 01)
--     app_users ('BR0456', 'U00123456') exists          (modules 03, 06, 08, 11)
--     customer with email 'anna.k@example.com' exists  (module 06)
--     loan_applications id 1 is SUBMITTED              (modules 02, 04 update it)
--     accounts 1 = 1000.00, 2 = 500.00; on_call alice and bob on night duty (module 05)

\set ON_ERROR_STOP on
SET search_path = loans, public;
SET synchronous_commit = off;          -- a crash during seeding loses at most the tail; just re-run
SET maintenance_work_mem = '256MB';
\timing on

TRUNCATE branches, roles, workflow_states, workflow_transitions, document_kinds, loan_products,
         product_rates, customers, app_users, user_roles, app_sessions, loan_applications,
         documents, payments, application_audit, loan_applications_archive,
         application_audit_archive, error_log, accounts, on_call, cache_versions,
         migration_checkpoints, migration_rejects, migration_parity, loan_applications_staging
         RESTART IDENTITY CASCADE;

SELECT setseed(0.42);

-- --------------------------------------------------------------------------------------------
-- 1. Master data (the business codes)
-- --------------------------------------------------------------------------------------------

INSERT INTO roles (code, name) VALUES
    ('CLERK',          'Branch clerk'),
    ('CREDIT_OFFICER', 'Credit officer'),
    ('BRANCH_MANAGER', 'Branch manager'),
    ('AUDITOR',        'Internal auditor');

INSERT INTO workflow_states (code, name, sort_order, is_terminal) VALUES
    ('DRAFT',            'Draft',                 10, false),
    ('SUBMITTED',        'Submitted',             20, false),
    ('READY_FOR_REVIEW', 'Ready for review',      30, false),
    ('UNDER_REVIEW',     'Under credit review',   40, false),
    ('APPROVED',         'Approved',              50, false),
    ('REJECTED',         'Rejected',              60, true),
    ('WITHDRAWN',        'Withdrawn by customer', 70, true),
    ('DISBURSED',        'Disbursed',             80, false),
    ('CLOSED',           'Closed (repaid)',       90, true);

-- The state machine as data. Note the UNDER_REVIEW -> SUBMITTED edge ("send back"): a cycle,
-- which the recursive CTE exercise in module 07 must terminate on.
-- There is no DRAFT -> WITHDRAWN edge: a withdrawn application must have been submitted
-- (constraint la_submitted_needs_time).
INSERT INTO workflow_transitions (from_state, to_state, required_role) VALUES
    ('DRAFT',            'SUBMITTED',        'CLERK'),
    ('SUBMITTED',        'READY_FOR_REVIEW', 'CLERK'),
    ('SUBMITTED',        'WITHDRAWN',        'CLERK'),
    ('SUBMITTED',        'UNDER_REVIEW',     'CREDIT_OFFICER'),
    ('READY_FOR_REVIEW', 'UNDER_REVIEW',     'CREDIT_OFFICER'),
    ('UNDER_REVIEW',     'SUBMITTED',        'CREDIT_OFFICER'),
    ('UNDER_REVIEW',     'REJECTED',         'CREDIT_OFFICER'),
    ('UNDER_REVIEW',     'APPROVED',         'BRANCH_MANAGER'),
    ('APPROVED',         'WITHDRAWN',        'CLERK'),
    ('APPROVED',         'DISBURSED',        'BRANCH_MANAGER'),
    ('DISBURSED',        'CLOSED',           'BRANCH_MANAGER');

INSERT INTO document_kinds (code, name, mandatory) VALUES
    ('ID_PROOF',       'Identity proof',  true),
    ('ADDRESS_PROOF',  'Address proof',   true),
    ('INCOME_PROOF',   'Income proof',    false),
    ('BANK_STATEMENT', 'Bank statement',  false),
    ('PROPERTY_DEED',  'Property deed',   false);

INSERT INTO loan_products (code, name, max_amount, min_term, max_term) VALUES
    ('HOME',     'Home loan',        5000000.00, 60, 360),
    ('AUTO',     'Vehicle loan',     2000000.00, 12,  84),
    ('PERSONAL', 'Personal loan',    1500000.00,  6,  60),
    ('GOLD',     'Gold loan',        1000000.00,  6,  36),
    ('EDU',      'Education loan',   3000000.00, 12, 180),
    ('BIZ',      'Business loan',    4000000.00, 12, 120);

-- Two non-overlapping rate periods per product; a third overlapping one would be refused by
-- the exclusion constraint (try it: module 09 exercise).
INSERT INTO product_rates (product_id, annual_rate, valid_during)
SELECT p.id, 8.0 + p.id * 0.75, daterange('2020-01-01', '2024-04-01')
FROM loan_products p
UNION ALL
SELECT p.id, 8.5 + p.id * 0.75, daterange('2024-04-01', NULL)
FROM loan_products p;

-- 500 branches BR0000 .. BR0499. Limits vary so that some applications exceed them
-- (the AMOUNT_LIMIT validation rule in module 08 needs failures to find).
INSERT INTO branches (code, name, city, max_loan_amount, time_zone)
SELECT 'BR' || lpad(g::text, 4, '0'),
       'Branch ' || g,
       (ARRAY['Chennai', 'Bengaluru', 'Mumbai', 'Pune', 'Hyderabad', 'Kolkata', 'Delhi', 'Kochi'])[1 + g % 8],
       (1 + g % 5) * 1000000.00,
       'Asia/Kolkata'
FROM generate_series(0, 499) g;

-- --------------------------------------------------------------------------------------------
-- 2. Customers: 200,000
-- --------------------------------------------------------------------------------------------

INSERT INTO customers (national_id, full_name, email, phone, date_of_birth, kyc_verified, home_branch, created_at)
SELECT 'NID' || lpad(g::text, 10, '0'),                                    -- natural key, unique
       'Customer ' || g,
       CASE WHEN g = 777       THEN 'anna.k@example.com'                    -- module 06 looks her up
            WHEN g % 20 = 0    THEN NULL                                    -- 5% have no email
            ELSE 'cust' || g || '@example.com' END,
       '+91' || lpad((9000000000 + g)::text, 10, '0'),
       DATE '1950-01-01' + (random() * 18000)::int,                         -- born 1950 .. ~1999
       random() < 0.9,                                                      -- 90% KYC verified
       'BR' || lpad((g % 500)::text, 4, '0'),
       TIMESTAMPTZ '2019-01-01 00:00+05:30' + g * interval '15 minutes'
FROM generate_series(1, 200000) g
ORDER BY g;

-- --------------------------------------------------------------------------------------------
-- 3. Users: 2,000,000 (the module 03 login table), plus their roles
-- --------------------------------------------------------------------------------------------
-- user g sits in branch g % 500, so ('BR0456', 'U00123456') exists (123456 % 500 = 456).
-- Every 50th user (g % 50 = 7) is LOCKED: a 2% value for the partial-index demos. Not g % 50 = 0,
-- because then every manager (id % 100 = 0) would be locked and hold no effective roles.

INSERT INTO app_users (branch_code, user_code, password_hash, status, failed_attempts, full_name, created_at)
SELECT 'BR' || lpad((g % 500)::text, 4, '0'),
       'U'  || lpad(g::text, 8, '0'),
       md5(g::text),                                                        -- stand-in; real systems store bcrypt/argon2
       CASE WHEN g % 50 = 7 THEN 'LOCKED' ELSE 'ACTIVE' END,
       CASE WHEN g % 50 = 7 THEN 5 ELSE 0 END,
       'User ' || g,
       TIMESTAMPTZ '2018-01-01 00:00+05:30' + g * interval '1 minute'
FROM generate_series(1, 2000000) g
ORDER BY g;

-- Everyone is a clerk in their branch; every 10th also a credit officer; every 100th a manager;
-- every 1000th an auditor. Identity ids equal g because the insert above was in g order.
INSERT INTO user_roles (user_id, role_code, branch_code)
SELECT u.id, r.role_code, u.branch_code
FROM app_users u
CROSS JOIN LATERAL (
    SELECT 'CLERK' AS role_code
    UNION ALL SELECT 'CREDIT_OFFICER' WHERE u.id % 10 = 0
    UNION ALL SELECT 'BRANCH_MANAGER' WHERE u.id % 100 = 0
    UNION ALL SELECT 'AUDITOR'        WHERE u.id % 1000 = 0
) r;

-- --------------------------------------------------------------------------------------------
-- 4. Loan applications: 1,000,000
-- --------------------------------------------------------------------------------------------
-- Layout of the generator:
--   year      = 2020 + (g-1) / 150000           -> 2020 .. 2026, numbers restart each year
--   seq       = (g-1) % 150000 + 1              -> application_no LA-YYYY-NNNNNN
--   customer  = (g-1) % 200000 + 1, product = (g-1) / 200000 + 1
--               -> each customer has at most one application per product, so the partial
--                  unique index la_one_active_key can never be violated by the seed
--   created_at increases with g within a year   -> physical order correlates with time (BRIN, keyset)
--   status    = skewed distribution, older years mostly closed
-- LA-2024-000123 is g = 4 * 150000 + 123 = 600123.

INSERT INTO loan_applications (requested_amount, customer_id, product_id, updated_by, submitted_at,
                               created_at, updated_at, term_months, application_no, branch_code, status)
SELECT s.amount,
       s.customer_id,
       s.product_id,
       CASE WHEN s.status = 'DRAFT' THEN NULL
            ELSE 500 * (1 + s.g % 3999) + s.customer_id % 500 END,           -- a user of the same branch
       CASE WHEN s.status = 'DRAFT' THEN NULL
            ELSE s.created_at + s.r2 * interval '3 days' END,
       s.created_at,
       s.created_at + interval '5 days',
       s.term,
       'LA-' || s.yr || '-' || lpad(s.seq::text, 6, '0'),
       'BR' || lpad((s.customer_id % 500)::text, 4, '0'),
       s.status
FROM (
    SELECT g, r2, yr, seq, customer_id, product_id,
           make_timestamptz(yr, 1, 1, 9, 0, 0, 'Asia/Kolkata')
               + (seq::numeric / 150000) * interval '364 days'
               + r2 * interval '8 hours'                                    AS created_at,
           round((50000 + r3 * 2950000)::numeric, -3)                       AS amount,
           (ARRAY[12, 24, 36, 48, 60, 84, 120, 180, 240])[1 + g % 9]        AS term,
           CASE WHEN g = 1 THEN 'SUBMITTED'                                 -- modules 02/04 update row 1
                WHEN r1 < 0.03 THEN 'DRAFT'
                WHEN r1 < 0.10 THEN 'SUBMITTED'
                WHEN r1 < 0.12 THEN 'READY_FOR_REVIEW'
                WHEN r1 < 0.20 THEN 'UNDER_REVIEW'
                WHEN r1 < 0.25 THEN 'APPROVED'
                WHEN r1 < 0.40 THEN 'REJECTED'
                WHEN r1 < 0.45 THEN 'WITHDRAWN'
                WHEN r1 < 0.60 THEN 'DISBURSED'
                ELSE 'CLOSED' END                                           AS status
    FROM (
        SELECT g, random() AS r1, random() AS r2, random() AS r3,
               2020 + (g - 1) / 150000       AS yr,
               (g - 1) % 150000 + 1          AS seq,
               (g - 1) % 200000 + 1          AS customer_id,
               (g - 1) / 200000 + 1          AS product_id
        FROM generate_series(1, 1000000) g
    ) raw
) s
ORDER BY s.g;

-- --------------------------------------------------------------------------------------------
-- 5. Documents: 0 to 4 per non-draft application (id % 5), so about 40% have fewer than 2
--    and fail the DOC_MISSING validation rule.
-- --------------------------------------------------------------------------------------------

INSERT INTO documents (application_id, kind, storage_key, sha256, size_bytes, uploaded_at)
SELECT a.id,
       (ARRAY['ID_PROOF', 'ADDRESS_PROOF', 'INCOME_PROOF', 'BANK_STATEMENT'])[k],
       's3://loan-docs/' || a.application_no || '/' || k || '.pdf',
       sha256(convert_to(a.application_no || ':' || k, 'UTF8')),
       50000 + (a.id * k) % 2000000,
       coalesce(a.submitted_at, a.created_at) - interval '1 hour'
FROM loan_applications a
CROSS JOIN LATERAL generate_series(1, (a.id % 5)::int) k
WHERE a.status <> 'DRAFT';

-- --------------------------------------------------------------------------------------------
-- 6. Payments: one disbursement and three repayments per disbursed or closed application
-- --------------------------------------------------------------------------------------------

INSERT INTO payments (application_id, kind, amount, posted_at)
SELECT a.id, 'DISBURSEMENT', a.requested_amount, a.submitted_at + interval '10 days'
FROM loan_applications a
WHERE a.status IN ('DISBURSED', 'CLOSED')
UNION ALL
SELECT a.id, 'REPAYMENT', round(a.requested_amount / a.term_months, 2),
       a.submitted_at + interval '10 days' + m * interval '1 month'
FROM loan_applications a
CROSS JOIN generate_series(1, 3) m
WHERE a.status IN ('DISBURSED', 'CLOSED');

-- --------------------------------------------------------------------------------------------
-- 7. Audit trail: DRAFT -> SUBMITTED for every submitted application, then -> current status
-- --------------------------------------------------------------------------------------------

INSERT INTO application_audit (application_id, actor_id, at, from_state, to_state, note)
SELECT a.id, a.updated_by, a.submitted_at, 'DRAFT', 'SUBMITTED', 'seed'
FROM loan_applications a
WHERE a.status <> 'DRAFT'
UNION ALL
SELECT a.id, a.updated_by, a.submitted_at + interval '2 days', 'SUBMITTED', a.status, 'seed (collapsed history)'
FROM loan_applications a
WHERE a.status NOT IN ('DRAFT', 'SUBMITTED');

-- --------------------------------------------------------------------------------------------
-- 8. Demo tables for module 05
-- --------------------------------------------------------------------------------------------

INSERT INTO accounts (id, balance) VALUES (1, 1000.00), (2, 500.00), (3, 2000.00);
INSERT INTO on_call (doctor, shift, on_duty) VALUES
    ('alice', 'night', true),
    ('bob',   'night', true),
    ('carol', 'day',   true);

INSERT INTO cache_versions (table_name) VALUES ('branches'), ('loan_products')
ON CONFLICT (table_name) DO NOTHING;

-- --------------------------------------------------------------------------------------------
-- 9. Statistics and visibility map. Without VACUUM the first index-only scans do heap fetches
--    for every row (module 03, "Index-only scans need VACUUM").
-- --------------------------------------------------------------------------------------------

VACUUM (ANALYZE);

SELECT 'branches' AS t, count(*) FROM branches
UNION ALL SELECT 'customers', count(*) FROM customers
UNION ALL SELECT 'app_users', count(*) FROM app_users
UNION ALL SELECT 'user_roles', count(*) FROM user_roles
UNION ALL SELECT 'loan_applications', count(*) FROM loan_applications
UNION ALL SELECT 'documents', count(*) FROM documents
UNION ALL SELECT 'payments', count(*) FROM payments
UNION ALL SELECT 'application_audit', count(*) FROM application_audit;

SELECT status, count(*) FROM loan_applications GROUP BY status ORDER BY count(*) DESC;
