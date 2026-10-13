-- m05_deadlock_b.sql : produce a deadlock, read the log, fix it by lock ordering. SESSION B (module 05).
--
-- Paste steps in label order together with m05_deadlock_a.sql: A1, B1, A2, B2, then A3, B3, A4.

-- ===========================================================================================
-- Part 1: the deadlock. B transfers 2 -> 1.
-- ===========================================================================================
-- B1 (after A1)
BEGIN;
UPDATE accounts SET balance = balance - 50 WHERE id = 2;    -- B locks row 2
-- (now A2: A blocks on row 2)
-- B2
UPDATE accounts SET balance = balance + 50 WHERE id = 1;    -- B blocks on row 1, held by A: cycle
-- One of the two sessions gets, after about 1 second:
--   ERROR:  deadlock detected   (SQLSTATE 40P01)
-- The application treats 40P01 like 40001: roll back and retry the whole transaction.
COMMIT;   -- or ROLLBACK; in the aborted session either one just ends the transaction

-- ===========================================================================================
-- Part 2: ordered locking
-- ===========================================================================================
-- B3 (after A3)
BEGIN;
SELECT id FROM accounts WHERE id IN (1, 2) ORDER BY id FOR UPDATE;   -- blocks on row 1 until A commits
UPDATE accounts SET balance = balance - 50 WHERE id = 2;
UPDATE accounts SET balance = balance + 50 WHERE id = 1;
COMMIT;
SELECT id, balance FROM accounts WHERE id IN (1, 2) ORDER BY id;    -- 950, 550: both transfers applied
