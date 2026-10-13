-- m05_deadlock_a.sql : produce a deadlock, read the log, fix it by lock ordering. SESSION A (module 05).
--
-- Two terminals, `psql loans` in each. Paste steps in label order: A1, B1, A2, B2.
-- The other half is m05_deadlock_b.sql. Server log on Ubuntu:
--     sudo tail -f /var/log/postgresql/postgresql-16-main.log

-- RESET
UPDATE accounts SET balance = CASE id WHEN 1 THEN 1000 WHEN 2 THEN 500 ELSE 2000 END;

-- ===========================================================================================
-- Part 1: the deadlock. A transfers 1 -> 2, B transfers 2 -> 1, each locking "from" first.
-- ===========================================================================================
-- A1
BEGIN;
UPDATE accounts SET balance = balance - 100 WHERE id = 1;   -- A locks row 1
-- (now B1: B locks row 2)
-- A2
UPDATE accounts SET balance = balance + 100 WHERE id = 2;   -- A waits for B (row 2) ...
-- (now B2: B waits for A (row 1): a cycle)
-- After deadlock_timeout (1 s by default) one backend runs the detector, finds the cycle and
-- aborts ITSELF (the one that ran the check, i.e. the session that waited long enough):
--   ERROR:  deadlock detected
--   DETAIL: Process 4242 waits for ShareLock on transaction 991; blocked by process 4243.
--           Process 4243 waits for ShareLock on transaction 990; blocked by process 4242.
--   HINT:  See server log for query details.
--   CONTEXT: while updating tuple (0,2) in relation "accounts"
-- The survivor's UPDATE then completes. Finish whichever session is still alive:
COMMIT;   -- (in a session that got the error, this just ends the aborted transaction: ROLLBACK)

-- What the log says (log_lock_waits = on also logs any wait longer than deadlock_timeout):
--   LOG:  process 4242 still waiting for ShareLock on transaction 991 after 1000.123 ms
--   ERROR:  deadlock detected ... STATEMENT:  UPDATE accounts SET balance = balance + 100 WHERE id = 2;
-- "ShareLock on transaction": a row lock is waited on via the holder's transaction id.

-- ===========================================================================================
-- Part 2: the fix. Every transfer locks the two rows in a GLOBAL order (lowest id first),
-- regardless of direction. Then no cycle can form: whoever gets row 1 first wins, the other waits.
-- ===========================================================================================
-- RESET, then:
-- A3
BEGIN;
SELECT id FROM accounts WHERE id IN (1, 2) ORDER BY id FOR UPDATE;   -- locks 1 then 2
-- (now B3: B's identical SELECT blocks on row 1, holding nothing)
-- A4
UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;
COMMIT;                                                              -- B proceeds; no deadlock

-- Same idea in one statement (the UPDATE visits rows in an order you do not control, so the
-- explicit ordered FOR UPDATE above is the reliable form):
--   UPDATE accounts SET balance = balance + CASE id WHEN 1 THEN -100 ELSE 100 END WHERE id IN (1, 2);
