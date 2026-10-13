-- plpgsql/m08_functions.sql : validation, authorisation and login functions (module 08).
--
-- Load order:  psql loans -f plpgsql/m08_functions.sql
--              psql loans -f plpgsql/m08_procedures.sql
--              psql loans -f plpgsql/m08_workflow_rules.sql
--              pg_prove -d loans plpgsql/m08_pgtap_tests.sql      (needs postgresql-16-pgtap)
--
-- Conventions: parameters p_, variables v_; every function pins search_path; lookup-only
-- functions are STABLE; custom SQLSTATEs in class LN (LN4xx = the caller's fault).
-- In production these would be owned by a dedicated non-superuser role (e.g. loans_owner) that
-- owns the tables; in this dev setup they are owned by whoever loads them.

\set ON_ERROR_STOP on
SET search_path = loans, public;

-- ===========================================================================================
-- Authorisation: does the user hold the role in the branch?
-- LANGUAGE sql, STABLE, a single SELECT with no FROM at the top level: the planner can INLINE it
-- into the calling query (module 08, 2.6), so it costs a join, not a function call.
-- ===========================================================================================
CREATE OR REPLACE FUNCTION fn_user_has_role(p_user_id bigint, p_role text, p_branch text)
RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, loans, pg_temp
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM loans.user_roles r
        JOIN loans.app_users u ON u.id = r.user_id
        WHERE r.user_id = p_user_id
          AND r.role_code = p_role
          AND r.branch_code = p_branch
          AND u.status = 'ACTIVE'                 -- a locked user holds no effective roles
    )
$$;
-- Note: a SET clause on a SQL function prevents inlining. Inlining matters for set-based use
-- (WHERE fn_user_has_role(...) over many rows); for a single call it does not. Exercise 2 in
-- module 08 asks you to write the inlinable version (drop the SET, qualify every name) and prove
-- the inlining with EXPLAIN.

-- ===========================================================================================
-- Validation rules: one row per violation. STABLE: reads tables, writes nothing, and runs with
-- the calling statement's snapshot, so every rule sees the same data.
-- ===========================================================================================
CREATE OR REPLACE FUNCTION fn_validate_application(p_app_id bigint)
RETURNS TABLE (rule_code text, severity text, message text)
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, loans, pg_temp
AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM loans.loan_applications a WHERE a.id = p_app_id) THEN
        RAISE EXCEPTION 'Application % does not exist', p_app_id USING ERRCODE = 'LN404';
    END IF;

    RETURN QUERY
    SELECT 'DOC_MISSING'::text, 'ERROR'::text, 'At least two supporting documents are required'::text
    WHERE (SELECT count(*) FROM loans.documents d WHERE d.application_id = p_app_id) < 2;

    RETURN QUERY
    SELECT 'ID_PROOF_MISSING'::text, 'ERROR'::text, 'An identity proof document is required'::text
    WHERE NOT EXISTS (SELECT 1 FROM loans.documents d
                      WHERE d.application_id = p_app_id AND d.kind = 'ID_PROOF');

    RETURN QUERY
    SELECT 'AMOUNT_LIMIT'::text, 'ERROR'::text,
           format('Requested amount %s exceeds branch limit of %s', a.requested_amount, b.max_loan_amount)
    FROM loans.loan_applications a JOIN loans.branches b ON b.code = a.branch_code
    WHERE a.id = p_app_id AND a.requested_amount > b.max_loan_amount;

    RETURN QUERY
    SELECT 'PRODUCT_LIMIT'::text, 'ERROR'::text,
           format('Requested amount %s exceeds product %s maximum of %s', a.requested_amount, p.code, p.max_amount)
    FROM loans.loan_applications a JOIN loans.loan_products p ON p.id = a.product_id
    WHERE a.id = p_app_id AND a.requested_amount > p.max_amount;

    RETURN QUERY
    SELECT 'TERM_RANGE'::text, 'ERROR'::text,
           format('Term %s months outside %s..%s for product %s', a.term_months, p.min_term, p.max_term, p.code)
    FROM loans.loan_applications a JOIN loans.loan_products p ON p.id = a.product_id
    WHERE a.id = p_app_id AND a.term_months NOT BETWEEN p.min_term AND p.max_term;

    RETURN QUERY
    SELECT 'KYC_PENDING'::text, 'WARNING'::text, 'Customer KYC not verified'::text
    FROM loans.loan_applications a JOIN loans.customers c ON c.id = a.customer_id
    WHERE a.id = p_app_id AND NOT c.kyc_verified;
    -- falling off the end returns the accumulated rows
END;
$$;

-- ===========================================================================================
-- Login check: the five-statement login path collapsed into one call returning one row.
-- SECURITY DEFINER: the application role may call it but cannot SELECT app_users itself.
-- The password comparison (bcrypt) and session token creation stay in the C server.
-- ===========================================================================================
CREATE OR REPLACE FUNCTION fn_login_check(p_branch_code text, p_user_code text)
RETURNS TABLE (user_id bigint, password_hash text, status text, failed_attempts int, roles text[])
LANGUAGE plpgsql STABLE
SECURITY DEFINER
SET search_path = pg_catalog, loans, pg_temp
AS $$
BEGIN
    RETURN QUERY
    SELECT u.id, u.password_hash, u.status, u.failed_attempts,
           coalesce(array_agg(DISTINCT r.role_code ORDER BY r.role_code)
                        FILTER (WHERE r.role_code IS NOT NULL), '{}')
    FROM loans.app_users u
    LEFT JOIN loans.user_roles r ON r.user_id = u.id
    WHERE u.branch_code = p_branch_code AND u.user_code = p_user_code      -- hits app_users_login_key
    GROUP BY u.id;
END;
$$;
REVOKE EXECUTE ON FUNCTION fn_login_check(text, text) FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION fn_login_check(text, text) TO loans_app;

-- ===========================================================================================
-- A trigger function used by the workflow layer: forbid direct status edits that skip the
-- procedure. The procedure sets a transaction-local flag; anyone else changing status is refused.
-- ===========================================================================================
CREATE OR REPLACE FUNCTION trg_guard_status_change() RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NEW.status IS DISTINCT FROM OLD.status
       AND coalesce(current_setting('loans.via_workflow', true), '') <> 'on'
       AND coalesce(current_setting('loans.allow_direct_status', true), '') <> 'on' THEN
        RAISE EXCEPTION 'status changes must go through sp_transition_application'
            USING ERRCODE = 'LN409',
                  HINT = 'CALL loans.sp_transition_application(id, to_state, actor_id, note)';
    END IF;
    RETURN NEW;
END;
$$;
-- The trigger itself is created by m08_workflow_rules.sql, OFF by default (the other modules'
-- demo scripts update status directly). Enable it to see the guard work.

-- Smoke test
SELECT * FROM fn_validate_application(42);
SELECT user_id, status, failed_attempts, roles FROM fn_login_check('BR0456', 'U00123456');
SELECT fn_user_has_role(100, 'BRANCH_MANAGER', 'BR0100') AS user_100_is_manager_of_br0100;
