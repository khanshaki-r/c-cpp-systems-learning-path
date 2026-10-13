-- deploy/initdb/001_schema.sql
-- Runs once when the postgres container initialises an empty data directory
-- (mounted at /docker-entrypoint-initdb.d). Gives 09_Databases a table to persist tcpkv into.

CREATE TABLE IF NOT EXISTS kv (
    key         TEXT PRIMARY KEY,
    value       BYTEA NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Audit log of every write, for the write-ahead / replication exercises.
CREATE TABLE IF NOT EXISTS kv_log (
    id          BIGSERIAL PRIMARY KEY,
    op          CHAR(3) NOT NULL CHECK (op IN ('SET', 'DEL')),
    key         TEXT NOT NULL,
    value       BYTEA,
    logged_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS kv_log_key_idx ON kv_log (key, id);
