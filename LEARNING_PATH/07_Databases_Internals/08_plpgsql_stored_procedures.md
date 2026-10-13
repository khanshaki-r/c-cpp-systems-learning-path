# 8. PL/pgSQL stored procedures

**Time:** ~3 hours   **Prerequisites:** 05, 06, 07

## 1. In plain words

PL/pgSQL is a small procedural language that runs inside the PostgreSQL server. You write a
function or procedure once, store it in the database, and call it from any client. Inside it you
can run SQL statements, keep local variables, loop, branch, and catch errors, and all of it
happens next to the data with no network round trips between steps.

The analogy: instead of phoning the bank clerk five times ("check the password", "check the
account is active", "count failed attempts", "record the login", "fetch the roles"), you hand
over one form that says "log this user in" and the clerk performs the whole procedure and
returns one answer. The procedure is written once, enforced for every caller, and can be changed
without redeploying the callers.

You already write PL/pgSQL. This module makes you able to defend every design decision in it:
when logic belongs in the database, why `EXCEPTION` blocks cost more than they look, what plan
caching does to your functions, what `SECURITY DEFINER` risks, and how to test and version
procedures like code.

## 2. The real thing

### 2.1 Functions versus procedures

| | `CREATE FUNCTION` | `CREATE PROCEDURE` (PostgreSQL 11+) |
|---|---|---|
| Called with | `SELECT fn(...)`, in expressions, FROM clause | `CALL proc(...)` |
| Returns | a value, a row, a set (`SETOF`, `RETURNS TABLE`), or `void` | nothing (OUT parameters for results) |
| Transaction control | none; runs inside the caller's transaction | `COMMIT`/`ROLLBACK` allowed when called at top level (not inside a `BEGIN` block, not from a function) |
| Usable in queries | yes | no |
| Typical use | computations, validations, row producers, triggers | multi-step workflows, batch jobs committing per batch |

Both are stored in `pg_proc`; both can be written in PL/pgSQL, SQL, C, or other PLs.

### 2.2 Structure and control flow

```sql
CREATE OR REPLACE FUNCTION fn_name(p_arg1 bigint, p_arg2 text DEFAULT 'x')
RETURNS integer
LANGUAGE plpgsql
STABLE                        -- volatility, see 2.6
SECURITY INVOKER              -- default; see 2.5
SET search_path = loans, pg_temp   -- pin the schema resolution for this function
AS $$
DECLARE
    v_count  integer := 0;
    v_row    loan_applications%ROWTYPE;      -- a variable shaped like a table row
    v_status loan_applications.status%TYPE;  -- a variable with a column's type
BEGIN
    SELECT * INTO STRICT v_row FROM loan_applications WHERE id = p_arg1;   -- STRICT: error if 0 or >1 rows

    IF v_row.status = 'DRAFT' THEN
        v_count := 1;
    ELSIF v_row.status IN ('SUBMITTED', 'UNDER_REVIEW') THEN
        v_count := 2;
    ELSE
        v_count := 3;
    END IF;

    v_status := CASE WHEN v_count = 1 THEN 'new' ELSE 'old' END;

    FOR v_row IN SELECT * FROM loan_applications WHERE customer_id = v_row.customer_id LOOP
        CONTINUE WHEN v_row.status = 'WITHDRAWN';
        v_count := v_count + 1;
        EXIT WHEN v_count > 100;
    END LOOP;

    RETURN v_count;
END;
$$;
```

Naming convention that avoids the most common PL/pgSQL bug: prefix parameters `p_` and variables
`v_` so they never collide with column names. A variable named `status` in a function that runs
`WHERE status = status` is ambiguous; the default `#variable_conflict error` makes it fail loudly,
which is the setting you want.

Loops over result sets: `FOR r IN SELECT ... LOOP`, `FOR i IN 1..10 LOOP`, `FOREACH x IN ARRAY
arr LOOP`, `WHILE cond LOOP`, and `FOR r IN EXECUTE dynamic_sql USING ... LOOP`. After any SQL
statement: `FOUND` is true if it affected or returned at least one row; `GET DIAGNOSTICS v_n =
ROW_COUNT;` gives the count.

### 2.3 Returning sets

```sql
-- RETURNS TABLE: the output columns are declared; RETURN NEXT adds a row, RETURN QUERY adds a result set
CREATE OR REPLACE FUNCTION fn_validate_application(p_app_id bigint)
RETURNS TABLE (rule_code text, severity text, message text)
LANGUAGE plpgsql STABLE AS $$
BEGIN
    RETURN QUERY
    SELECT 'DOC_MISSING', 'ERROR', 'At least two supporting documents are required'
    WHERE (SELECT count(*) FROM documents d WHERE d.application_id = p_app_id) < 2;

    RETURN QUERY
    SELECT 'AMOUNT_LIMIT', 'ERROR', format('Requested amount exceeds branch limit of %s', b.max_loan_amount)
    FROM loan_applications a JOIN branches b ON b.code = a.branch_code
    WHERE a.id = p_app_id AND a.requested_amount > b.max_loan_amount;

    RETURN QUERY
    SELECT 'KYC_PENDING', 'WARNING', 'Customer KYC not verified'
    FROM loan_applications a JOIN customers c ON c.id = a.customer_id
    WHERE a.id = p_app_id AND NOT c.kyc_verified;
    -- falling off the end returns the accumulated rows
END;
$$;

SELECT * FROM fn_validate_application(42);
```

`RETURNS SETOF some_table` or `SETOF record` are the older forms. Set-returning functions
materialise their whole output in a tuplestore before the caller sees the first row (PL/pgSQL
cannot stream), so do not build a 10 M row report this way; write a view or plain SQL.

### 2.4 Exceptions

```sql
BEGIN
    INSERT INTO app_sessions (user_id, token) VALUES (p_user_id, p_token);
EXCEPTION
    WHEN unique_violation THEN
        -- the INSERT's effects are rolled back to the start of this BEGIN block; the function continues
        UPDATE app_sessions SET token = p_token, created_at = now() WHERE user_id = p_user_id;
    WHEN OTHERS THEN
        GET STACKED DIAGNOSTICS v_state = RETURNED_SQLSTATE, v_msg = MESSAGE_TEXT, v_ctx = PG_EXCEPTION_CONTEXT;
        INSERT INTO error_log (sqlstate, message, context) VALUES (v_state, v_msg, v_ctx);
        RAISE;                   -- re-raise the original error after logging
END;
```

Every `BEGIN ... EXCEPTION ... END` block is a **subtransaction** (a savepoint): entering it
costs a subtransaction xid allocation on first write, and exiting via an exception rolls back to
the savepoint. This is the single most important performance fact about PL/pgSQL: an exception
block *inside a loop* creates one subtransaction per iteration, and past 64 live subtransactions
per backend the snapshot code falls off the `pg_subtrans` cliff (module 05). Put the loop inside
the exception block, or avoid exceptions for expected cases (`ON CONFLICT`, `IF EXISTS` checks).

Raising your own errors with meaning the application can switch on:

```sql
RAISE EXCEPTION 'Transition % -> % not permitted for role %', v_from, p_to, v_role
    USING ERRCODE = 'LN403',                       -- custom 5-char SQLSTATE; class 'LN' is yours
          HINT = 'Request approval from a branch manager',
          DETAIL = format('application_id=%s actor=%s', p_app_id, p_actor_id);
```

Levels: `DEBUG`, `LOG`, `INFO`, `NOTICE`, `WARNING` (continue), `EXCEPTION` (abort). The client
receives SQLSTATE, message, detail and hint as separate fields (`PQresultErrorField`), so the
application maps `LN403` to HTTP 403 without parsing text.

### 2.5 SECURITY DEFINER

A `SECURITY DEFINER` function runs with the privileges of its *owner*, like setuid. Use: let an
application role call `sp_login` that reads `password_hash`, without granting the role SELECT on
`app_users`. Rules that make it safe:

1. `SET search_path = pg_catalog, loans, pg_temp` on the function, so a caller cannot create a
   temp table or function that shadows one you reference (the classic privilege escalation).
2. `REVOKE EXECUTE ON FUNCTION ... FROM PUBLIC; GRANT EXECUTE ... TO app_role;` because functions
   are executable by PUBLIC by default.
3. Schema-qualify everything inside, validate every input, never build SQL by concatenation
   (`format('%I', ...)` for identifiers, `%L` for literals, or `EXECUTE ... USING`).
4. Own it by a role with the minimum needed privileges, not a superuser.

### 2.6 Volatility, strictness, and plan caching

**Volatility** tells the planner how often it may call the function:

| Category | Promise | Effects |
|---|---|---|
| `VOLATILE` (default) | may do anything, result can differ per call | called once per row, never indexed, gets a fresh snapshot for each statement inside |
| `STABLE` | same inputs + same snapshot -> same result; reads tables but does not modify | may be called once per statement for constant args; usable in index conditions; sees the calling statement's snapshot |
| `IMMUTABLE` | pure function of its inputs, never reads tables | may be constant-folded at plan time; required for expression indexes and generated columns |

Lying is a correctness bug: an `IMMUTABLE` function that reads a table returns stale results
once folded into a plan or an index. Mark every lookup-only function `STABLE`; it is both
faster and honest.

`STRICT` (`RETURNS NULL ON NULL INPUT`): the function is not called at all if any argument is
NULL. `PARALLEL SAFE` lets it run in parallel workers (default `UNSAFE`). `LEAKPROOF` matters
only for row-level security.

**Plan caching.** Each SQL statement inside a PL/pgSQL function is prepared on first execution
in a session and the plan is cached for the life of the session, with the same custom/generic
logic as prepared statements (module 06): five custom plans, then possibly generic. Three
consequences:

- A function statement with a skewed parameter can lock into a bad generic plan. Use
  `SET plan_cache_mode = force_custom_plan` on the function
  (`ALTER FUNCTION f SET plan_cache_mode = force_custom_plan`), or write the SQL so the skew is
  not a parameter.
- `EXECUTE 'dynamic sql' USING ...` is planned every call: flexible but no caching, and the right
  tool when the table name or the WHERE shape changes.
- Cached plans are invalidated when the referenced objects change (DDL); you do not need to
  reconnect after `CREATE INDEX`.

**Inlining.** A `LANGUAGE sql` function that is a single SELECT and is `STABLE`/`IMMUTABLE` and
not `STRICT`-with-nullable-args can be **inlined** into the calling query, letting the planner
see through it. PL/pgSQL functions are never inlined; each call is a black box with a
`cpu_operator_cost`-based default cost (`COST 100`). For small reusable expressions, prefer SQL
functions.

### 2.7 Transaction control in procedures

```sql
CREATE OR REPLACE PROCEDURE sp_archive_withdrawn(p_batch_size int DEFAULT 10000)
LANGUAGE plpgsql AS $$
DECLARE
    v_moved int;
BEGIN
    LOOP
        WITH batch AS (
            DELETE FROM loan_applications
            WHERE id IN (SELECT id FROM loan_applications
                         WHERE status = 'WITHDRAWN' AND updated_at < now() - interval '2 years'
                         ORDER BY id LIMIT p_batch_size
                         FOR UPDATE SKIP LOCKED)
            RETURNING *
        )
        INSERT INTO loan_applications_archive SELECT * FROM batch;
        GET DIAGNOSTICS v_moved = ROW_COUNT;
        COMMIT;                                 -- releases locks, lets VACUUM reclaim, bounds WAL per transaction
        EXIT WHEN v_moved < p_batch_size;
    END LOOP;
END;
$$;
CALL sp_archive_withdrawn(5000);                -- must be called outside an explicit transaction block
```

After `COMMIT` inside a procedure a new transaction starts automatically; `SET TRANSACTION` can
change its isolation. Procedures cannot commit if called from a function, from a trigger, or
inside `BEGIN ... END` from the client (the error is "invalid transaction termination").

### 2.8 Where logic belongs: database or application

Put it in the database when:

- The rule must hold for **every** writer (several applications, batch jobs, DBAs): constraints
  first, then triggers or procedures. "Application status can only change along defined
  transitions" is such a rule.
- It is **set-oriented** and touches many rows: moving it to the application means shipping the
  rows out and back.
- It is a **multi-statement atomic unit** that would otherwise be several round trips in one
  transaction (your login path; a state transition with audit).
- **Security**: the application role should not see raw data (password hashes) but needs a
  derived answer.

Keep it in the application when:

- It is CPU-heavy and scales with users (the database is the hardest tier to scale out; a
  stored procedure that hashes passwords with bcrypt at cost 12 burns the database's CPU).
- It calls external systems (HTTP, queues) or needs libraries the database lacks.
- It changes often and is owned by many teams; database deployments are more coupled.
- Testing and debugging tooling for your application language is far better than for PL/pgSQL,
  and the logic is complex branching rather than data rules.

The honest answer for your banking server: validation rules, authorisation checks against
roles/branches, and workflow transitions with audit went into PL/pgSQL because they are
data-integrity rules enforced for all callers and they collapsed many round trips; password
hashing and session token generation stayed in C.

### 2.9 Testing with pgTAP and versioning

**pgTAP** is a set of SQL assertion functions (`CREATE EXTENSION pgtap`, package
`postgresql-16-pgtap` on Ubuntu) run with `pg_prove`. A test is a transaction that is rolled
back, so it never dirties the database:

```sql
BEGIN;
SELECT plan(5);
SELECT has_function('loans', 'sp_transition_application', ARRAY['bigint','text','bigint','text']);
SELECT lives_ok($$CALL loans.sp_transition_application(1, 'SUBMITTED', 100, 'test')$$, 'DRAFT -> SUBMITTED allowed');
-- the transition exists, but needs CREDIT_OFFICER; user 100 is clerk + branch manager only
SELECT throws_ok($$CALL loans.sp_transition_application(1, 'UNDER_REVIEW', 100, 'test')$$, 'LN403',
                 NULL, 'SUBMITTED -> UNDER_REVIEW blocked without CREDIT_OFFICER');
-- no such edge in the workflow at all: validation failure, checked before the role
SELECT throws_ok($$CALL loans.sp_transition_application(1, 'DISBURSED', 100, 'test')$$, 'LN400',
                 NULL, 'SUBMITTED -> DISBURSED is not a transition');
SELECT is((SELECT status FROM loans.loan_applications WHERE id = 1), 'SUBMITTED', 'status persisted');
SELECT * FROM finish();
ROLLBACK;
```

**Versioning procedures.** Treat them as code: one file per function in version control,
deployed through the same migration tool as schema changes (Flyway, sqitch, dbmate, or your own
numbered SQL scripts with a `schema_migrations` table). Rules:

- `CREATE OR REPLACE FUNCTION` keeps the OID (callers' cached plans are invalidated, not broken)
  but cannot change the return type or parameter names; for those you `DROP` and `CREATE`, inside
  one transaction so there is no window without the function.
- Changing the parameter list creates an **overload**, leaving the old signature in place.
  Clean it up explicitly; ambiguous overloads cause "function is not unique" errors.
- Deploy DDL and procedure changes together in one transaction when the new procedure depends
  on new columns.
- Keep the procedures backward compatible across one release when the application and database
  deploy separately (add parameters with DEFAULTs; add result columns at the end).

## 3. Code

The example rebuilds your "validation, authorisation and workflow rules" layer for the loan
schema. Files: `code/plpgsql/m08_functions.sql`, `m08_procedures.sql`, `m08_workflow_rules.sql`,
`m08_pgtap_tests.sql`. The core procedure:

```sql
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
    v_from_state text;
    v_required_role text;
    v_violations int;
BEGIN
    -- 1. Lock the application row for the duration: two clerks cannot race a transition.
    SELECT status INTO STRICT v_from_state
    FROM loan_applications WHERE id = p_app_id FOR UPDATE;

    -- 2. Is this transition defined at all? (data, not code: the graph lives in a table)
    SELECT required_role INTO v_required_role
    FROM workflow_transitions WHERE from_state = v_from_state AND to_state = p_to_state;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'No transition % -> %', v_from_state, p_to_state USING ERRCODE = 'LN400';
    END IF;

    -- 3. Authorisation: does the actor hold the required role in the application's branch?
    IF NOT fn_user_has_role(p_actor_id, v_required_role,
                            (SELECT branch_code FROM loan_applications WHERE id = p_app_id)) THEN
        RAISE EXCEPTION 'Transition % -> % requires role %', v_from_state, p_to_state, v_required_role
            USING ERRCODE = 'LN403';
    END IF;

    -- 4. Validation rules: any ERROR-severity violation blocks a submit or approve.
    IF p_to_state IN ('SUBMITTED', 'APPROVED') THEN
        SELECT count(*) INTO v_violations
        FROM fn_validate_application(p_app_id) v WHERE v.severity = 'ERROR';
        IF v_violations > 0 THEN
            RAISE EXCEPTION '% validation error(s) block transition to %', v_violations, p_to_state
                USING ERRCODE = 'LN422', HINT = 'Call fn_validate_application for details';
        END IF;
    END IF;

    -- 5. Apply and audit, in the same transaction as the checks.
    UPDATE loan_applications
    SET status = p_to_state, updated_at = now(), updated_by = p_actor_id
    WHERE id = p_app_id;

    INSERT INTO application_audit (application_id, from_state, to_state, actor_id, note)
    VALUES (p_app_id, v_from_state, p_to_state, p_actor_id, p_note);
END;
$$;
REVOKE EXECUTE ON PROCEDURE sp_transition_application FROM PUBLIC;
GRANT  EXECUTE ON PROCEDURE sp_transition_application TO loans_app;
```

And the login function that replaced five round trips, returning one row the C server consumes:

```sql
CREATE OR REPLACE FUNCTION fn_login_check(p_branch_code text, p_user_code text)
RETURNS TABLE (user_id bigint, password_hash text, status text, failed_attempts int, roles text[])
LANGUAGE plpgsql STABLE SECURITY DEFINER SET search_path = pg_catalog, loans, pg_temp
AS $$
BEGIN
    RETURN QUERY
    SELECT u.id, u.password_hash, u.status, u.failed_attempts,
           coalesce(array_agg(r.role_code ORDER BY r.role_code) FILTER (WHERE r.role_code IS NOT NULL), '{}')
    FROM app_users u
    LEFT JOIN user_roles r ON r.user_id = u.id
    WHERE u.branch_code = p_branch_code AND u.user_code = p_user_code      -- hits app_users_login_idx
    GROUP BY u.id;
END;
$$;
-- The password comparison (bcrypt) and session token creation stay in the C server;
-- a second small procedure sp_record_login(p_user_id, p_success) updates failed_attempts and inserts the session.
```

## 4. Under the hood (senior-level)

**How PL/pgSQL executes.** On first call in a session the function body is parsed into an AST
of PL/pgSQL statements (`PLpgSQL_stmt_*`) and cached in the session's function cache. Each
embedded SQL statement becomes an **SPI** (Server Programming Interface) prepared plan on its
first execution: `SPI_prepare` -> `SPI_execute_plan`, with PL/pgSQL variables passed as
parameters (`$1`, `$2` ...). Expressions like `v_count := v_count + 1` are themselves SQL
expressions (`SELECT $1 + 1`) executed through a fast path for simple expressions, which is why
tight arithmetic loops in PL/pgSQL are slow compared to C: each `:=` is an expression evaluation
through the executor, not a CPU add.

**Subtransactions in detail.** Entering a `BEGIN ... EXCEPTION` block calls
`BeginInternalSubTransaction`; a resource owner and memory context are created; on first write
the subtransaction gets its own xid, recorded in `pg_subtrans` and in the backend's ProcArray
cache (64 slots). On error, `RollbackAndReleaseCurrentSubTransaction` discards the
subtransaction's changes (its tuples become invisible: xid aborted) and releases its locks.
Cost per block even without an error: a few microseconds of bookkeeping; with a write, an xid.
Beyond 64 subxids, every snapshot by *any* session that must check one of this backend's subxids
goes to `pg_subtrans` on disk (SLRU). A 100k-iteration loop with an exception block per
iteration is the canonical way to trigger this on a busy system.

**Snapshot behaviour by volatility.** A `VOLATILE` function's statements each take a new
snapshot (READ COMMITTED semantics inside the function), so it sees rows committed by other
sessions during its own execution. A `STABLE` or `IMMUTABLE` function executes with the
*calling statement's* snapshot: everything inside is consistent with the outer query, and it
cannot see its own caller's uncommitted modifications made by the same statement. This is why a
validation function used in a WHERE clause must be `STABLE`: with `VOLATILE` it would be
re-evaluated per row with changing snapshots and cost a snapshot each time.

**`RETURN QUERY` and tuplestores.** Set-returning PL/pgSQL functions append every result row to a
tuplestore (`work_mem` in memory, then temp file), and the caller reads from it only after the
function returns. A function returning 1 M rows therefore holds 1 M rows before the first is
delivered; the planner also cannot push a WHERE from the caller into the function. The planner
estimates set-returning function output at `ROWS 1000` unless you set `ROWS n` on the function,
another source of bad plans in queries that join to functions.

**Function call overhead and the `COST` clause.** Each PL/pgSQL call costs tens of microseconds
of setup (SPI connect, variable allocation). The planner uses `COST` (default 100 for PL
functions, 1 for C/SQL) to decide evaluation order of predicates: cheaper predicates first. Set
it realistically for expensive validation functions so they run after cheap filters.

**Triggers** are functions with a fixed signature (`RETURNS trigger`) called by the executor's
`ModifyTable` node per row or per statement with `NEW`/`OLD`. Per-row triggers that run SQL turn a
set-based UPDATE of 1 M rows into 1 M function calls; statement-level triggers with transition
tables (`REFERENCING NEW TABLE AS newrows`) keep it set-based. Use them for audit and
denormalised counters; keep business workflows in explicit procedures where the caller can see
them.

## 5. In production

- **Error codes are an API.** Define a range of custom SQLSTATEs (`LN4xx` for client errors,
  `LN5xx` for server-side rule failures), document them, and map them in the C server to RPC
  error codes. Never make the client parse `MESSAGE_TEXT`.
- **Exception blocks per row** in a batch procedure is the most common PL/pgSQL performance
  bug. Validate with set-based SQL first; use `ON CONFLICT`; wrap the loop, not the iteration.
- **Lock ordering inside procedures.** `FOR UPDATE` the parent row first, then children,
  consistently across all procedures that touch the same tables; otherwise two procedures
  deadlock each other (module 05) and the user sees a 40P01 from a function three calls deep.
- **Long procedures block VACUUM and hold locks.** A procedure processing 10 M rows in one
  transaction is the long transaction from module 05. Commit per batch (2.7).
- **Deploying a new version of a hot function** under load: `CREATE OR REPLACE FUNCTION` needs
  only a brief lock on the `pg_proc` row but invalidates cached plans in every session; the first
  call after deployment re-plans. If the function's dependent table also changed in the same
  deployment, run both in one transaction.
- **`SECURITY DEFINER` without `search_path`** is a CVE-class mistake. Lint for it:
  `SELECT proname FROM pg_proc WHERE prosecdef AND NOT coalesce(proconfig::text, '') LIKE '%search_path%'`.
- **Debugging**: `RAISE NOTICE` with `client_min_messages = notice` in psql; `auto_explain.log_nested_statements = on`
  to see the plans of statements inside functions; `pg_stat_statements.track = all` to see them
  as separate entries; `plpgsql_check` extension for static analysis (catches typos in column
  names that only fail at runtime).
- **YugabyteDB**: PL/pgSQL runs unchanged on YSQL, but each statement inside may cost a network
  hop to a tablet leader, so collapsing round trips into procedures matters even more there; and
  `COMMIT` inside procedures is supported but each commit is a distributed transaction commit
  (module 11).

## 6. Try it

1. **(easy)** Load `code/plpgsql/m08_workflow_rules.sql` and the schema. Call
   `sp_transition_application` for a valid and an invalid transition from psql; print the
   SQLSTATE with `\errverbose`. Done when you see your custom `LN403` code.
2. **(easy)** Write `fn_user_has_role(p_user_id, p_role, p_branch)` as a `LANGUAGE sql STABLE`
   function and show with EXPLAIN that it is inlined into a calling query (no `Function Scan`,
   the join appears in the plan). Done when the plan shows the inlined join.
3. **(medium)** Write a batch procedure that inserts 100,000 rows with an `EXCEPTION WHEN
   unique_violation` block per row, time it; rewrite with `ON CONFLICT DO NOTHING` and no exception
   block, time it. Done when you can explain the difference in terms of subtransactions.
4. **(medium)** Mark `fn_validate_application` as `IMMUTABLE`, use it in a query, change the
   underlying data, and demonstrate a wrong (stale) answer. Done when you can state why `STABLE`
   is the correct category.
5. **(hard)** Create a `SECURITY DEFINER` function without `SET search_path` that calls an
   unqualified `lower(text)`. As a low-privilege role, create `public.lower(text)` that logs its
   argument to a table you control, call the definer function, and show the capture. Then fix the
   function. Done when the attack works before the fix and fails after.
6. **(hard)** Write pgTAP tests for the workflow procedure covering: a permitted transition, a
   forbidden transition (role), a blocked transition (validation), and the audit row. Run with
   `pg_prove -d loans code/plpgsql/m08_pgtap_tests.sql`. Done when all tests pass and the
   database is unchanged after the run.

## 7. Interview questions

**Q: Function versus procedure in PostgreSQL?**
A: A function returns a value or set and is used inside SQL statements, running within the
caller's transaction with no transaction control. A procedure is invoked with CALL, returns
nothing except OUT parameters, and can COMMIT and ROLLBACK when called at top level, which
makes it the right tool for batch jobs that commit per chunk.

**Q: What does an EXCEPTION block cost?**
A: It creates a subtransaction (a savepoint) on entry so that the block's changes can be rolled
back independently. That costs bookkeeping and, if the block writes, a subtransaction xid. Inside
a loop it creates one per iteration, and beyond 64 live subtransactions per backend snapshot
checks fall back to the on-disk pg_subtrans, which can slow the whole server. Wrap the loop, not
the iteration, and use ON CONFLICT instead of catching unique violations.

**Q: What do STABLE and IMMUTABLE mean and why does lying about them matter?**
A: STABLE promises the same result for the same inputs within one statement and no writes, so the
planner may evaluate it once per statement, use it in index conditions, and run it under the
calling statement's snapshot. IMMUTABLE promises a pure function of its inputs, so it can be
constant-folded and used in expression indexes. An IMMUTABLE function that reads tables returns
stale results from folded plans and indexes.

**Q: How does plan caching work inside PL/pgSQL?**
A: Each SQL statement in a function is prepared through SPI on first execution per session and
the plan is cached, following the prepared-statement rule of five custom plans then possibly a
generic plan. Skewed parameters can lock in a bad generic plan; force_custom_plan on the
function or dynamic EXECUTE avoids it. Plans are invalidated automatically by DDL.

**Q: When do you put logic in the database rather than the application?**
A: When the rule must hold for every writer, when it is set-oriented over many rows, when it is a
multi-statement atomic unit that would otherwise be several round trips, or when the application
role should not see the raw data. Keep CPU-heavy work, external calls, and frequently changing
team-owned logic in the application. In my banking server, validation, authorisation and workflow
transitions went into PL/pgSQL; password hashing stayed in C.

**Q: What are the risks of SECURITY DEFINER?**
A: The function runs with the owner's privileges, so an attacker who can influence name
resolution can hijack it: without a fixed search_path, a caller's temp schema or public schema
function can shadow a referenced function or operator. Mitigate with SET search_path on the
function, schema-qualified references, REVOKE EXECUTE FROM PUBLIC, parameterised dynamic SQL, and
a non-superuser owner.

**Q: How did your login procedure change the latency?**
A: Five statements in the C server became one function call returning one row with the user's
hash, status, attempt count and roles, cutting the round trips from five to one, and the
function's statements hit the covering natural-key index, so the whole check became a single
index-only read. The password comparison remained in the application.

**Q: How do you test and version stored procedures?**
A: One file per routine under version control, deployed with the same migration tool as schema
changes, in one transaction when the routine depends on new DDL. pgTAP tests run inside a
transaction that is rolled back, asserting results, raised SQLSTATEs and side effects such as
audit rows, and execute in CI via pg_prove against a database built from the migrations.

## 8. Recap

- Functions return values and run in the caller's transaction; procedures use CALL and can COMMIT per batch.
- Prefix parameters `p_`, variables `v_`; `#variable_conflict error` is your friend.
- `RETURNS TABLE` + `RETURN QUERY` for set results; output is materialised, not streamed.
- `EXCEPTION` blocks are subtransactions; never one per loop iteration; use `ON CONFLICT`.
- `RAISE ... USING ERRCODE = 'LNxxx'` makes errors a stable API for the C server.
- `SECURITY DEFINER` needs `SET search_path`, `REVOKE FROM PUBLIC`, qualified names, `format('%I','%L')`.
- Volatility: VOLATILE per row, STABLE per statement with the caller's snapshot, IMMUTABLE foldable; never lie.
- Statements inside functions are cached prepared plans: generic-plan traps apply; EXECUTE replans.
- Logic in the DB for integrity rules, set operations, atomic multi-step units, data hiding; not for CPU-heavy or external calls.
- pgTAP in a rolled-back transaction; version routines as code; CREATE OR REPLACE keeps the OID, overloads linger.
