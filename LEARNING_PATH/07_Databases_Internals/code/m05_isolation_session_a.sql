-- m05_isolation_session_a.sql : two-terminal isolation demos, SESSION A (module 05).
--
-- How to use: open two terminals, run `psql loans` in each. This file is terminal A,
-- m05_isolation_session_b.sql is terminal B. Do NOT run either file with -f: paste one step at a
-- time, in the global order given by the step labels (A1, then B1, then A2 ...). The expected
-- output is written next to every statement. Tip: \set PROMPT1 'A %/%R%# ' in this window and
-- 'B ...' in the other so you never type into the wrong one.
--
-- Tables (from m09_schema.sql / m09_seed.sql):
--   accounts(id, balance)            1 = 1000.00, 2 = 500.00, 3 = 2000.00
--   on_call(doctor, shift, on_duty)  alice and bob on night duty

-- ===========================================================================================
-- RESET: run this in A before each demo to get back to the starting balances
-- ===========================================================================================
UPDATE accounts SET balance = CASE id WHEN 1 THEN 1000 WHEN 2 THEN 500 ELSE 2000 END;
UPDATE on_call SET on_duty = true;

-- ===========================================================================================
-- Demo 1: READ COMMITTED (the default) -> non-repeatable read
-- Each STATEMENT takes a new snapshot, so two reads in one transaction can disagree.
-- ===========================================================================================
-- A1
BEGIN;                                              -- READ COMMITTED
SELECT balance FROM accounts WHERE id = 1;          -- 1000.00
-- (now B1)
-- A2
SELECT balance FROM accounts WHERE id = 1;          -- 900.00: a new statement saw B's commit
COMMIT;

-- ===========================================================================================
-- Demo 2: REPEATABLE READ keeps one snapshot, and refuses to overwrite a newer version
-- ===========================================================================================
-- A1
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT balance FROM accounts WHERE id = 1;          -- 900.00 (the snapshot is taken HERE, at the first statement)
-- (now B1)
-- A2
SELECT balance FROM accounts WHERE id = 1;          -- still 900.00: same snapshot
UPDATE accounts SET balance = balance - 50 WHERE id = 1;
-- ERROR:  could not serialize access due to concurrent update      (SQLSTATE 40001)
ROLLBACK;                                           -- the application must retry the whole transaction

-- ===========================================================================================
-- Demo 3: lost update at READ COMMITTED, and the FOR UPDATE fix
-- The application reads, computes in its own memory, writes the computed value back.
-- ===========================================================================================
-- RESET first (see top). Account 2 starts at 500.
-- A1
BEGIN;
SELECT balance FROM accounts WHERE id = 2;          -- 500.00; the app computes 500 - 300 = 200
-- (now B1: B reads 500, writes 100, commits)
-- A2
UPDATE accounts SET balance = 200 WHERE id = 2;     -- no error at READ COMMITTED
COMMIT;
SELECT balance FROM accounts WHERE id = 2;          -- 200.00: B's withdrawal of 400 is LOST

-- Fix version. RESET first.
-- A3
BEGIN;
SELECT balance FROM accounts WHERE id = 2 FOR UPDATE;   -- 500.00, row now locked by A
-- (now B2: B's SELECT ... FOR UPDATE blocks)
-- A4
UPDATE accounts SET balance = 200 WHERE id = 2;
COMMIT;                                             -- B unblocks and reads 200.00 (READ COMMITTED re-reads the latest version)
-- (B3 finishes: 200 - 400 would go negative, so B's application refuses: correct)

-- Also correct, and simpler: let the database do the arithmetic.
-- UPDATE accounts SET balance = balance - 300 WHERE id = 2 AND balance >= 300;
-- At READ COMMITTED a blocked UPDATE re-evaluates its WHERE on the newest version (EvalPlanQual).

-- ===========================================================================================
-- Demo 4: write skew. Only SERIALIZABLE catches it.
-- Invariant: at least one doctor on call per shift. Each transaction checks, then goes off duty.
-- ===========================================================================================
-- RESET first.
-- A1
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM on_call WHERE shift = 'night' AND on_duty;   -- 2, so "I can leave"
-- (now B1)
-- A2
UPDATE on_call SET on_duty = false WHERE doctor = 'alice' AND shift = 'night';
COMMIT;                                             -- ok
-- (now B2: B's COMMIT fails with 40001 "read/write dependencies among transactions")

-- Repeat with REPEATABLE READ in both sessions: both commit, and nobody is on call:
-- SELECT * FROM on_call WHERE shift = 'night';     -- alice f, bob f   (invariant broken)

-- ===========================================================================================
-- Demo 5: phantoms. PostgreSQL's REPEATABLE READ does not show them (stronger than the SQL standard).
-- ===========================================================================================
-- RESET first.
-- A1
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM accounts WHERE balance > 100;  -- 3
-- (now B1: B inserts account 4 with balance 5000 and commits)
-- A2
SELECT count(*) FROM accounts WHERE balance > 100;  -- still 3: no phantom
COMMIT;
SELECT count(*) FROM accounts WHERE balance > 100;  -- 4: new transaction, new snapshot
DELETE FROM accounts WHERE id = 4;                  -- clean up

-- ===========================================================================================
-- Demo 6: what a waiting session looks like from the outside (run while B is blocked)
-- ===========================================================================================
-- A1
BEGIN;
UPDATE accounts SET balance = balance + 1 WHERE id = 3;          -- A holds the row lock
-- (now B1: B tries to update the same row and blocks)
-- A2: who is blocking whom?
SELECT a.pid, a.state, a.wait_event_type, a.wait_event,
       pg_blocking_pids(a.pid) AS blocked_by,
       age(now(), a.xact_start) AS xact_age,
       left(a.query, 60) AS query
FROM pg_stat_activity a
WHERE a.datname = current_database() AND a.pid <> pg_backend_pid()
ORDER BY a.xact_start;
-- B shows wait_event_type = Lock, wait_event = transactionid, blocked_by = {A's pid}.
-- Row locks wait on the HOLDER'S TRANSACTION ID, because the lock itself lives in the tuple's xmax.
SELECT locktype, relation::regclass, transactionid, mode, granted, pid
FROM pg_locks
WHERE NOT granted OR locktype = 'transactionid'
ORDER BY pid;
ROLLBACK;                                           -- B proceeds
