-- plpgsql/m08_procedures.sql : procedures with exception handling and transaction control (module 08).
--
-- Load after m08_functions.sql:   psql loans -f plpgsql/m08_procedures.sql

\set ON_ERROR_STOP on
SET search_path = loans, public;

-- ===========================================================================================
-- sp_record_login: the write half of the login path. Called by the C server AFTER it has
-- compared the bcrypt hash returned by fn_login_check.
--   success -> reset failed_attempts, stamp last_login_at, upsert the session row
--   failure -> count the attempt, lock the user at 5
-- One round trip, one transaction (the caller's), no exception blocks: ON CONFLICT handles the
-- "session already exists" case without a subtransaction.
-- ===========================================================================================
CREATE OR REPLACE PROCEDURE sp_record_login(
    p_user_id  bigint,
    p_success  boolean,
    p_token    text DEFAULT NULL,
    INOUT p_status text DEFAULT NULL)               -- OUT-style result: the user's status afterwards
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, loans, pg_temp
AS $$
BEGIN
    IF p_success THEN
        -- last_login_at is not indexed and app_users has fillfactor 90: this UPDATE is HOT.
        UPDATE loans.app_users
        SET last_login_at = now(), failed_attempts = 0
        WHERE id = p_user_id
        RETURNING status INTO p_status;

        INSERT INTO loans.app_sessions (user_id, token)
        VALUES (p_user_id, p_token)
        ON CONFLICT (user_id) DO UPDATE SET token = EXCLUDED.token, created_at = now();
    ELSE
        UPDATE loans.app_users
        SET failed_attempts = failed_attempts + 1,
            status = CASE WHEN failed_attempts + 1 >= 5 THEN 'LOCKED' ELSE status END
        WHERE id = p_user_id
        RETURNING status INTO p_status;
    END IF;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'User % does not exist', p_user_id USING ERRCODE = 'LN404';
    END IF;
END;
$$;
REVOKE EXECUTE ON PROCEDURE sp_record_login(bigint, boolean, text, text) FROM PUBLIC;
GRANT  EXECUTE ON PROCEDURE sp_record_login(bigint, boolean, text, text) TO loans_app;

-- ===========================================================================================
-- fn_open_session_legacy: the EXCEPTION-block version from module 08 section 2.4, kept to show
-- the pattern and its cost. Each call enters a subtransaction; a unique_violation rolls back to
-- the block's savepoint and the handler runs. Correct, but ON CONFLICT (above) is cheaper.
-- ===========================================================================================
CREATE OR REPLACE FUNCTION fn_open_session_legacy(p_user_id bigint, p_token text)
RETURNS text
LANGUAGE plpgsql
SET search_path = pg_catalog, loans, pg_temp
AS $$
DECLARE
    v_state text;
    v_msg   text;
    v_ctx   text;
BEGIN
    BEGIN
        INSERT INTO loans.app_sessions (user_id, token) VALUES (p_user_id, p_token);
        RETURN 'inserted';
    EXCEPTION
        WHEN unique_violation THEN
            -- the INSERT's effects are rolled back to the start of this block; we continue
            UPDATE loans.app_sessions SET token = p_token, created_at = now() WHERE user_id = p_user_id;
            RETURN 'updated';
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS v_state = RETURNED_SQLSTATE, v_msg = MESSAGE_TEXT, v_ctx = PG_EXCEPTION_CONTEXT;
            INSERT INTO loans.error_log (sqlstate, message, context) VALUES (v_state, v_msg, v_ctx);
            RAISE;                   -- re-raise the original error after logging (the log row rolls back with it!)
    END;
END;
$$;

-- ===========================================================================================
-- sp_archive_withdrawn: a batch job that COMMITs per batch (procedures only).
-- Moves withdrawn applications older than p_age (and their audit rows) to the archive tables.
-- Documents go with them through ON DELETE CASCADE. Must be CALLed outside BEGIN ... END.
-- ===========================================================================================
CREATE OR REPLACE PROCEDURE sp_archive_withdrawn(
    p_batch_size int DEFAULT 10000,
    p_age interval DEFAULT interval '2 years')
LANGUAGE plpgsql
SET search_path = pg_catalog, loans, pg_temp
AS $$
DECLARE
    v_ids   bigint[];
    v_total bigint := 0;
BEGIN
    LOOP
        -- Pick a batch and lock it. SKIP LOCKED: rows a clerk is editing right now are left for
        -- the next run instead of making the batch job wait.
        SELECT array_agg(id) INTO v_ids
        FROM (SELECT a.id FROM loans.loan_applications a
              WHERE a.status = 'WITHDRAWN' AND a.updated_at < now() - p_age
                AND NOT EXISTS (SELECT 1 FROM loans.payments p WHERE p.application_id = a.id)
              ORDER BY a.id
              LIMIT p_batch_size
              FOR UPDATE SKIP LOCKED) b;
        EXIT WHEN v_ids IS NULL;

        -- Children first (the FK from application_audit would refuse the parent delete).
        WITH moved AS (DELETE FROM loans.application_audit WHERE application_id = ANY (v_ids) RETURNING *)
        INSERT INTO loans.application_audit_archive SELECT * FROM moved;

        WITH moved AS (DELETE FROM loans.loan_applications WHERE id = ANY (v_ids) RETURNING *)
        INSERT INTO loans.loan_applications_archive SELECT * FROM moved;

        v_total := v_total + cardinality(v_ids);
        RAISE NOTICE 'archived % so far', v_total;
        COMMIT;   -- releases locks, lets VACUUM reclaim, bounds WAL and lock memory per transaction
        EXIT WHEN cardinality(v_ids) < p_batch_size;
    END LOOP;
END;
$$;
-- Usage (outside an explicit transaction):
--   CALL loans.sp_archive_withdrawn(5000);
-- Inside BEGIN ... the first COMMIT fails with "invalid transaction termination".
-- The DELETE of audit rows needs privileges the application role does not have; the owner
-- runs this job (or make it SECURITY DEFINER with the same hardening as the others).

-- Smoke test (inside a transaction, so nothing changes)
BEGIN;
CALL sp_record_login(123456, true, 'tok-demo');
SELECT user_id, token FROM app_sessions WHERE user_id = 123456;
SELECT fn_open_session_legacy(123456, 'tok-demo-2') AS legacy_path;     -- 'updated': the session existed
ROLLBACK;
