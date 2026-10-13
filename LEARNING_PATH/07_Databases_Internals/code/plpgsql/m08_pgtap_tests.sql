-- plpgsql/m08_pgtap_tests.sql : pgTAP tests for the workflow layer (module 08, exercise 6).
--
-- Setup once:
--     sudo apt install -y postgresql-16-pgtap libtap-parser-sourcehandler-pgtap-perl
--     psql loans -c "CREATE EXTENSION IF NOT EXISTS pgtap"
-- Run:
--     pg_prove -d loans code/plpgsql/m08_pgtap_tests.sql
--     (or: psql loans -f code/plpgsql/m08_pgtap_tests.sql  and read the "ok n" lines)
--
-- The whole file is one transaction that is rolled back: the database is unchanged afterwards.
-- Fixtures are created with fixed natural keys and their ids captured with \gset, because CALL
-- arguments cannot contain subqueries.

\set ON_ERROR_STOP on
\set QUIET on
SET client_min_messages = warning;
BEGIN;
SET LOCAL search_path = loans, public;

-- ---------------------------------------------------------------------- fixtures
INSERT INTO customers (national_id, full_name, date_of_birth, kyc_verified, home_branch)
VALUES ('NID9100000001', 'Test Customer', DATE '1985-06-01', true, 'BR0001')
RETURNING id AS cust \gset

-- app_ok: two documents including an ID proof -> passes validation
INSERT INTO loan_applications (requested_amount, customer_id, product_id, term_months, application_no, branch_code)
VALUES (250000, :cust, (SELECT id FROM loan_products WHERE code = 'BIZ'), 36, 'LA-2098-000001', 'BR0001')
RETURNING id AS app_ok \gset
INSERT INTO documents (application_id, kind, storage_key, sha256, size_bytes)
SELECT :app_ok, k, 's3://test/' || k, sha256(convert_to(k, 'UTF8')), 1000
FROM unnest(ARRAY['ID_PROOF', 'ADDRESS_PROOF']) k;

-- app_bad: one document only -> DOC_MISSING blocks the submit
INSERT INTO loan_applications (requested_amount, customer_id, product_id, term_months, application_no, branch_code)
VALUES (250000, :cust, (SELECT id FROM loan_products WHERE code = 'GOLD'), 12, 'LA-2098-000002', 'BR0001')
RETURNING id AS app_bad \gset
INSERT INTO documents (application_id, kind, storage_key, sha256, size_bytes)
VALUES (:app_bad, 'ID_PROOF', 's3://test/only', sha256('\x00'::bytea), 1000);

INSERT INTO app_users (branch_code, user_code, password_hash, full_name)
VALUES ('BR0001', 'U91000001', 'x', 'Test clerk'), ('BR0001', 'U91000002', 'x', 'Test officer');
SELECT id AS clerk   FROM app_users WHERE branch_code = 'BR0001' AND user_code = 'U91000001' \gset
SELECT id AS officer FROM app_users WHERE branch_code = 'BR0001' AND user_code = 'U91000002' \gset
INSERT INTO user_roles (user_id, role_code, branch_code)
VALUES (:clerk, 'CLERK', 'BR0001'), (:officer, 'CREDIT_OFFICER', 'BR0001');

-- ---------------------------------------------------------------------- tests
SELECT plan(12);

-- 1-2. the objects exist with the expected signatures
SELECT ok(EXISTS (SELECT 1 FROM pg_proc
                  WHERE proname = 'sp_transition_application' AND prokind = 'p'
                    AND pronamespace = 'loans'::regnamespace),
          'sp_transition_application is a procedure in schema loans');
SELECT has_function('loans', 'fn_validate_application', ARRAY['bigint'], 'fn_validate_application(bigint) exists');

-- 3. validation passes for the good application
SELECT is_empty(format('SELECT * FROM loans.fn_validate_application(%s) WHERE severity = %L', :app_ok, 'ERROR'),
                'good application has no ERROR violations');

-- 4. a permitted transition
SELECT lives_ok(format('CALL loans.sp_transition_application(%s, %L, %s, %L)', :app_ok, 'SUBMITTED', :clerk, 'test'),
                'DRAFT -> SUBMITTED allowed for a clerk');

-- 5-6. its effects: status and submitted_at persisted, one audit row with the actor
SELECT is((SELECT status FROM loans.loan_applications WHERE id = :app_ok), 'SUBMITTED', 'status persisted');
SELECT results_eq(format('SELECT from_state, to_state, actor_id FROM loans.application_audit WHERE application_id = %s', :app_ok),
                  format('VALUES (%L::text, %L::text, %s::bigint)', 'DRAFT', 'SUBMITTED', :clerk),
                  'exactly one audit row, with from/to state and actor');

-- 7. a transition that exists but needs a role the clerk lacks
SELECT throws_ok(format('CALL loans.sp_transition_application(%s, %L, %s, NULL)', :app_ok, 'UNDER_REVIEW', :clerk),
                 'LN403', NULL, 'SUBMITTED -> UNDER_REVIEW refused for a clerk (role)');

-- 8. a transition that does not exist in the graph
SELECT throws_ok(format('CALL loans.sp_transition_application(%s, %L, %s, NULL)', :app_ok, 'DISBURSED', :officer),
                 'LN400', NULL, 'SUBMITTED -> DISBURSED is not a transition');

-- 9. validation blocks a submit
SELECT throws_ok(format('CALL loans.sp_transition_application(%s, %L, %s, NULL)', :app_bad, 'SUBMITTED', :clerk),
                 'LN422', NULL, 'submit blocked by DOC_MISSING');

-- 10. unknown application
SELECT throws_ok(format('CALL loans.sp_transition_application(%s, %L, %s, NULL)', -1, 'SUBMITTED', :clerk),
                 'LN404', NULL, 'unknown application id');

-- 11-12. the officer may continue, and the audit trail grows by one
SELECT lives_ok(format('CALL loans.sp_transition_application(%s, %L, %s, %L)', :app_ok, 'UNDER_REVIEW', :officer, 'picked up'),
                'SUBMITTED -> UNDER_REVIEW allowed for a credit officer');
SELECT is((SELECT count(*)::int FROM loans.application_audit WHERE application_id = :app_ok), 2, 'two audit rows');

SELECT * FROM finish();
ROLLBACK;
