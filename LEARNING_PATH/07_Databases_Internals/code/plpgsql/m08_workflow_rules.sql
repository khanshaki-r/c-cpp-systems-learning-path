-- plpgsql/m08_workflow_rules.sql : the workflow state machine enforced in the database (module 08).
--
-- Load after m08_functions.sql and m08_procedures.sql:
--     psql loans -f plpgsql/m08_workflow_rules.sql
--
-- The graph lives in workflow_transitions (data, seeded by m09_seed.sql):
--     DRAFT -> SUBMITTED (CLERK)          SUBMITTED -> READY_FOR_REVIEW (CLERK)
--     SUBMITTED -> WITHDRAWN (CLERK)      SUBMITTED -> UNDER_REVIEW (CREDIT_OFFICER)
--     READY_FOR_REVIEW -> UNDER_REVIEW    UNDER_REVIEW -> SUBMITTED  (send back, CREDIT_OFFICER)
--     UNDER_REVIEW -> REJECTED (CREDIT_OFFICER)    UNDER_REVIEW -> APPROVED (BRANCH_MANAGER)
--     APPROVED -> DISBURSED (BRANCH_MANAGER)        APPROVED -> WITHDRAWN (CLERK)
--     DISBURSED -> CLOSED (BRANCH_MANAGER)
--
-- Error codes (the API the C server switches on, never the message text):
--     LN400 no such transition      LN401 actor not active      LN403 role missing
--     LN404 application not found   LN409 direct status edit    LN422 validation failed

\set ON_ERROR_STOP on
SET search_path = loans, public;

CREATE OR REPLACE PROCEDURE sp_transition_application(
    p_app_id   bigint,
    p_to_state text,
    p_actor_id bigint,
    p_note     text DEFAULT NULL)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, loans, pg_temp
AS $$
DECLARE
    v_from_state    text;
    v_branch        text;
    v_required_role text;
    v_violations    int;
BEGIN
    -- 1. Lock the application row for the rest of the transaction: two clerks cannot race a
    --    transition. Parent row first, always: the lock-ordering rule that prevents deadlocks.
    SELECT a.status, a.branch_code INTO v_from_state, v_branch
    FROM loans.loan_applications a WHERE a.id = p_app_id
    FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Application % not found', p_app_id USING ERRCODE = 'LN404';
    END IF;

    -- 2. Is this transition defined at all? (data, not code)
    SELECT t.required_role INTO v_required_role
    FROM loans.workflow_transitions t
    WHERE t.from_state = v_from_state AND t.to_state = p_to_state;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'No transition % -> %', v_from_state, p_to_state
            USING ERRCODE = 'LN400',
                  DETAIL = format('application_id=%s', p_app_id);
    END IF;

    -- 3. Authorisation: the actor must be active and hold the role in the application's branch.
    IF NOT EXISTS (SELECT 1 FROM loans.app_users u WHERE u.id = p_actor_id AND u.status = 'ACTIVE') THEN
        RAISE EXCEPTION 'Actor % is not an active user', p_actor_id USING ERRCODE = 'LN401';
    END IF;
    IF NOT loans.fn_user_has_role(p_actor_id, v_required_role, v_branch) THEN
        RAISE EXCEPTION 'Transition % -> % requires role % in branch %',
                        v_from_state, p_to_state, v_required_role, v_branch
            USING ERRCODE = 'LN403',
                  HINT = 'Request the action from a user holding that role',
                  DETAIL = format('application_id=%s actor=%s', p_app_id, p_actor_id);
    END IF;

    -- 4. Validation rules: any ERROR-severity violation blocks a submit or an approval.
    IF p_to_state IN ('SUBMITTED', 'APPROVED') THEN
        SELECT count(*) INTO v_violations
        FROM loans.fn_validate_application(p_app_id) v
        WHERE v.severity = 'ERROR';
        IF v_violations > 0 THEN
            RAISE EXCEPTION '% validation error(s) block transition to %', v_violations, p_to_state
                USING ERRCODE = 'LN422',
                      HINT = 'SELECT * FROM loans.fn_validate_application(' || p_app_id || ')';
        END IF;
    END IF;

    -- 5. Apply and audit in the same transaction as the checks. The flag tells the guard trigger
    --    (if enabled) that this status change came through the workflow.
    PERFORM set_config('loans.via_workflow', 'on', true);          -- true = transaction-local
    UPDATE loans.loan_applications
    SET status       = p_to_state,
        submitted_at = CASE WHEN p_to_state = 'SUBMITTED' THEN coalesce(submitted_at, now())
                            ELSE submitted_at END,               -- la_submitted_needs_time
        updated_at   = now(),
        updated_by   = p_actor_id
    WHERE id = p_app_id;
    PERFORM set_config('loans.via_workflow', 'off', true);

    INSERT INTO loans.application_audit (application_id, from_state, to_state, actor_id, note)
    VALUES (p_app_id, v_from_state, p_to_state, p_actor_id, p_note);
END;
$$;
REVOKE EXECUTE ON PROCEDURE sp_transition_application(bigint, text, bigint, text) FROM PUBLIC;
GRANT  EXECUTE ON PROCEDURE sp_transition_application(bigint, text, bigint, text) TO loans_app;

-- The guard: refuse status edits that bypass the procedure. Created DISABLED so the demo
-- scripts of modules 02 and 04 (which flip status directly) keep working. To see it work:
--   ALTER TABLE loans.loan_applications ENABLE TRIGGER la_guard_status;
--   UPDATE loans.loan_applications SET status = 'APPROVED' WHERE id = 1;   -- ERROR LN409
DROP TRIGGER IF EXISTS la_guard_status ON loan_applications;
CREATE TRIGGER la_guard_status BEFORE UPDATE OF status ON loan_applications
    FOR EACH ROW EXECUTE FUNCTION trg_guard_status_change();
ALTER TABLE loan_applications DISABLE TRIGGER la_guard_status;

-- Lint from module 08 section 5: SECURITY DEFINER routines without a pinned search_path.
SELECT proname AS definer_without_search_path
FROM pg_proc
WHERE prosecdef AND pronamespace = 'loans'::regnamespace
  AND NOT coalesce(proconfig::text, '') LIKE '%search_path%';   -- expect zero rows

-- ===========================================================================================
-- Walk-through (module 08 exercise 1). Run inside a transaction so the seed is untouched.
-- In psql, after an error, \errverbose prints SQLSTATE, DETAIL and HINT.
-- ===========================================================================================
BEGIN;
-- A fresh customer with a DRAFT application in BR0001 and two documents, plus a clerk and a
-- credit officer of that branch.
INSERT INTO customers (national_id, full_name, date_of_birth, kyc_verified, home_branch)
VALUES ('NID9000000001', 'Demo Customer', DATE '1985-06-01', true, 'BR0001')
RETURNING id AS demo_customer \gset
INSERT INTO loan_applications (requested_amount, customer_id, product_id, term_months, application_no, branch_code)
VALUES (250000, :demo_customer, (SELECT id FROM loan_products WHERE code = 'BIZ'), 36, 'LA-2099-000001', 'BR0001')
RETURNING id AS demo_app \gset
INSERT INTO documents (application_id, kind, storage_key, sha256, size_bytes)
SELECT :demo_app, k, 's3://demo/' || k, sha256(convert_to(k, 'UTF8')), 1000
FROM unnest(ARRAY['ID_PROOF', 'ADDRESS_PROOF']) k;
INSERT INTO app_users (branch_code, user_code, password_hash, full_name)
VALUES ('BR0001', 'U90000001', 'x', 'Demo clerk'), ('BR0001', 'U90000002', 'x', 'Demo officer');
SELECT id AS demo_clerk   FROM app_users WHERE branch_code = 'BR0001' AND user_code = 'U90000001' \gset
SELECT id AS demo_officer FROM app_users WHERE branch_code = 'BR0001' AND user_code = 'U90000002' \gset
INSERT INTO user_roles (user_id, role_code, branch_code)
VALUES (:demo_clerk, 'CLERK', 'BR0001'), (:demo_officer, 'CREDIT_OFFICER', 'BR0001');

SELECT * FROM fn_validate_application(:demo_app);              -- no ERROR rows
CALL sp_transition_application(:demo_app, 'SUBMITTED', :demo_clerk, 'customer signed');
SAVEPOINT before_forbidden;
\set ON_ERROR_STOP off
\set VERBOSITY verbose
CALL sp_transition_application(:demo_app, 'UNDER_REVIEW', :demo_clerk, 'clerk tries to review');
-- ERROR:  LN403: Transition SUBMITTED -> UNDER_REVIEW requires role CREDIT_OFFICER in branch BR0001
ROLLBACK TO before_forbidden;
CALL sp_transition_application(:demo_app, 'DISBURSED', :demo_officer, 'skip the queue');
-- ERROR:  LN400: No transition SUBMITTED -> DISBURSED
ROLLBACK TO before_forbidden;
\set VERBOSITY default
\set ON_ERROR_STOP on
CALL sp_transition_application(:demo_app, 'UNDER_REVIEW', :demo_officer, 'officer picks it up');
SELECT from_state, to_state, actor_id, note FROM application_audit WHERE application_id = :demo_app ORDER BY id;
ROLLBACK;
