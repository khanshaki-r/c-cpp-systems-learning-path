-- m05_isolation_session_b.sql : two-terminal isolation demos, SESSION B (module 05).
--
-- Paste one step at a time, interleaved with m05_isolation_session_a.sql in the order of the step
-- labels (A1, B1, A2 ...). Statements here run in autocommit unless a BEGIN is shown.

-- ===========================================================================================
-- Demo 1: READ COMMITTED non-repeatable read
-- ===========================================================================================
-- B1 (after A1)
UPDATE accounts SET balance = balance - 100 WHERE id = 1;   -- autocommit: 1000 -> 900, committed now
-- (now A2)

-- ===========================================================================================
-- Demo 2: REPEATABLE READ and the concurrent-update error
-- ===========================================================================================
-- B1 (after A1)
UPDATE accounts SET balance = balance - 100 WHERE id = 1;   -- 900 -> 800, committed
-- (now A2: A still sees 900 and its UPDATE fails with 40001)

-- ===========================================================================================
-- Demo 3: lost update
-- ===========================================================================================
-- B1 (after A1)
BEGIN;
SELECT balance FROM accounts WHERE id = 2;          -- 500.00; the app computes 500 - 400 = 100
UPDATE accounts SET balance = 100 WHERE id = 2;
COMMIT;
-- (now A2: A overwrites with 200 and B's withdrawal disappears)

-- Fix version:
-- B2 (after A3)
BEGIN;
SELECT balance FROM accounts WHERE id = 2 FOR UPDATE;   -- BLOCKS until A commits ...
-- (now A4) ... then returns 200.00, the committed value, not the stale 500
-- B3
-- The app sees 200 < 400 and refuses the withdrawal:
ROLLBACK;

-- ===========================================================================================
-- Demo 4: write skew
-- ===========================================================================================
-- B1 (after A1)
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM on_call WHERE shift = 'night' AND on_duty;   -- 2, so "I can leave" too
-- (now A2)
-- B2
UPDATE on_call SET on_duty = false WHERE doctor = 'bob' AND shift = 'night';
COMMIT;
-- ERROR:  could not serialize access due to read/write dependencies among transactions
-- DETAIL: Reason code: Canceled on identification as a pivot, during commit attempt.
-- HINT:  The transaction might succeed if retried.
-- A retry re-reads count = 1 and correctly refuses to leave.
-- (Depending on timing the error can also come at the UPDATE instead of the COMMIT.)

-- ===========================================================================================
-- Demo 5: phantoms
-- ===========================================================================================
-- B1 (after A1)
INSERT INTO accounts (id, balance) VALUES (4, 5000);    -- autocommit
-- (now A2: A's count is still 3)

-- ===========================================================================================
-- Demo 6: blocked writer
-- ===========================================================================================
-- B1 (after A1)
UPDATE accounts SET balance = balance + 1 WHERE id = 3;   -- BLOCKS: A holds the row
-- (A2 inspects pg_stat_activity and pg_locks, then A rolls back; this UPDATE completes)
-- Repeat demo 6 with a bound on the wait, which every production session should have.
-- B2 (before repeating A1)
SET lock_timeout = '2s';
-- B3 (after A1 again)
UPDATE accounts SET balance = balance + 1 WHERE id = 3;
-- ERROR:  canceling statement due to lock timeout   (SQLSTATE 55P03) after 2 seconds
RESET lock_timeout;
