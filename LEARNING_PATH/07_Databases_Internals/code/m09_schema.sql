-- m09_schema.sql : the loan-origination schema used by every module of 07_Databases_Internals.
--
-- Run:   psql loans -f m09_schema.sql      (then m09_seed.sql)
-- Safe to re-run: it drops and recreates schema "loans".
--
-- Every design decision is commented with WHY. Module 09 explains the rules; this file applies them:
--   * surrogate bigint identity PKs, natural keys as UNIQUE NOT NULL constraints
--   * master (lookup) tables keyed by short business codes: branches, roles, workflow_states,
--     document_kinds, loan_products.code
--   * every FK column indexed (the "FK without index" query in module 09 returns no rows)
--   * money as numeric(18,2), instants as timestamptz, constraints named
--   * columns ordered wide-to-narrow to minimise alignment padding (module 02)

\set ON_ERROR_STOP on

-- Extensions live in "public" so every schema can use them.
CREATE EXTENSION IF NOT EXISTS btree_gist;      -- needed for the EXCLUDE constraint on product_rates
CREATE EXTENSION IF NOT EXISTS pageinspect;     -- modules 02, 03, 05
CREATE EXTENSION IF NOT EXISTS pg_buffercache;  -- modules 01, 02
CREATE EXTENSION IF NOT EXISTS pgstattuple;     -- module 03 (bloat)
CREATE EXTENSION IF NOT EXISTS pg_visibility;   -- module 02 (visibility map bits)
CREATE EXTENSION IF NOT EXISTS pg_freespacemap; -- module 02 (free space map)

DROP SCHEMA IF EXISTS loans CASCADE;
CREATE SCHEMA loans;

-- Make the schema the default for every new session on this database, so the modules can
-- write "FROM loan_applications" without qualifying. :"DBNAME" is a psql variable.
ALTER DATABASE :"DBNAME" SET search_path = loans, public;
SET search_path = loans, public;

-- The application role. NOLOGIN: real deployments log in as a member role or via the pooler.
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'loans_app') THEN
        CREATE ROLE loans_app NOLOGIN;
    END IF;
END $$;

-- ============================================================================================
-- 1. Master tables keyed by business codes
-- ============================================================================================

-- Branch codes are short, stable and printed on every document: the one deliberate natural PK.
CREATE TABLE branches (
    max_loan_amount  numeric(18,2) NOT NULL CONSTRAINT branches_max_amount_positive CHECK (max_loan_amount > 0),
    code             text PRIMARY KEY CONSTRAINT branches_code_format CHECK (code ~ '^BR[0-9]{4}$'),
    name             text NOT NULL,
    city             text NOT NULL,
    time_zone        text NOT NULL DEFAULT 'Asia/Kolkata'      -- IANA zone, never a fixed offset
);

CREATE TABLE roles (
    code  text PRIMARY KEY CONSTRAINT roles_code_upper CHECK (code = upper(code)),
    name  text NOT NULL
);

-- The workflow is DATA: adding a state is an INSERT, and sp_transition_application reads it.
CREATE TABLE workflow_states (
    code         text PRIMARY KEY CONSTRAINT ws_code_upper CHECK (code = upper(code)),
    name         text NOT NULL,
    sort_order   int  NOT NULL,
    is_terminal  boolean NOT NULL DEFAULT false
);

CREATE TABLE workflow_transitions (
    id             int GENERATED ALWAYS AS IDENTITY UNIQUE,     -- handy handle for tooling and the module 07 CTE
    from_state     text NOT NULL REFERENCES workflow_states(code),
    to_state       text NOT NULL REFERENCES workflow_states(code),
    required_role  text NOT NULL REFERENCES roles(code),
    CONSTRAINT workflow_transitions_pkey PRIMARY KEY (from_state, to_state),
    CONSTRAINT wt_no_self_loop CHECK (from_state <> to_state)
);
CREATE INDEX wt_to_state_idx      ON workflow_transitions (to_state);        -- FK index
CREATE INDEX wt_required_role_idx ON workflow_transitions (required_role);   -- FK index

CREATE TABLE document_kinds (
    code        text PRIMARY KEY CONSTRAINT dk_code_upper CHECK (code = upper(code)),
    name        text NOT NULL,
    mandatory   boolean NOT NULL DEFAULT false
);

CREATE TABLE loan_products (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    max_amount  numeric(18,2) NOT NULL CONSTRAINT lp_max_amount_positive CHECK (max_amount > 0),
    min_term    int  NOT NULL,
    max_term    int  NOT NULL,
    code        text NOT NULL,
    name        text NOT NULL,
    CONSTRAINT loan_products_code_key UNIQUE (code),                         -- the business key
    CONSTRAINT lp_term_range CHECK (min_term BETWEEN 1 AND max_term)
);

-- Interest rates over time. The exclusion constraint makes overlapping validity periods for one
-- product impossible: a rule the database enforces for every writer (module 09, 2.3).
CREATE TABLE product_rates (
    id            bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    product_id    bigint NOT NULL REFERENCES loan_products(id),
    annual_rate   numeric(6,3) NOT NULL CONSTRAINT pr_rate_range CHECK (annual_rate > 0 AND annual_rate < 50),
    valid_during  daterange NOT NULL,
    CONSTRAINT rate_no_overlap EXCLUDE USING gist (product_id WITH =, valid_during WITH &&)
);
-- product_id is the first column of the gist index behind rate_no_overlap, so the FK is indexed.

-- ============================================================================================
-- 2. People: customers and users
-- ============================================================================================

CREATE TABLE customers (
    id            bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    created_at    timestamptz NOT NULL DEFAULT now(),
    updated_at    timestamptz NOT NULL DEFAULT now(),
    deleted_at    timestamptz,                                            -- soft delete
    date_of_birth date NOT NULL CONSTRAINT customers_dob_sane CHECK (date_of_birth > DATE '1900-01-01'),
    kyc_verified  boolean NOT NULL DEFAULT false,
    national_id   text NOT NULL,
    full_name     text NOT NULL,
    email         text,
    phone         text,
    home_branch   text NOT NULL REFERENCES branches(code),
    CONSTRAINT customers_national_id_key UNIQUE (national_id)
);
-- Email unique only among live customers: a soft-deleted customer must not block a new sign-up.
CREATE UNIQUE INDEX customers_email_live_key ON customers (lower(email)) WHERE deleted_at IS NULL;
CREATE INDEX customers_home_branch_idx ON customers (home_branch);           -- FK index

-- The application sees live customers only through this view (module 09, 2.6).
CREATE VIEW customers_live AS
    SELECT * FROM customers WHERE deleted_at IS NULL;

-- Login table. The natural key (branch_code, user_code) is a UNIQUE constraint WITH the INCLUDE
-- columns the login needs: this is the covering index from the module 03/06 case study, created
-- by the constraint itself so nobody can forget it again.
-- fillfactor 90: last_login_at is updated on every login; free space on the page keeps those
-- updates HOT (no index writes), module 03, 2.9.
CREATE TABLE app_users (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    last_login_at    timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    failed_attempts  int  NOT NULL DEFAULT 0 CONSTRAINT au_failed_nonneg CHECK (failed_attempts >= 0),
    branch_code      text NOT NULL REFERENCES branches(code),
    user_code        text NOT NULL CONSTRAINT au_user_code_format CHECK (user_code ~ '^U[0-9]{8}$'),
    password_hash    text NOT NULL,
    status           text NOT NULL DEFAULT 'ACTIVE'
                     CONSTRAINT au_status_valid CHECK (status IN ('ACTIVE', 'LOCKED', 'DISABLED')),
    full_name        text,
    CONSTRAINT app_users_login_key UNIQUE (branch_code, user_code)
        INCLUDE (password_hash, status, failed_attempts)
) WITH (fillfactor = 90);

-- Who may do what, where. Composite natural PK: a pure link table nobody references.
CREATE TABLE user_roles (
    user_id      bigint NOT NULL REFERENCES app_users(id) ON DELETE CASCADE,
    role_code    text   NOT NULL REFERENCES roles(code),
    branch_code  text   NOT NULL REFERENCES branches(code),
    granted_at   timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT user_roles_pkey PRIMARY KEY (user_id, role_code, branch_code)
);
CREATE INDEX user_roles_role_idx   ON user_roles (role_code, branch_code);   -- FK index + "who are the managers of BR0007"
CREATE INDEX user_roles_branch_idx ON user_roles (branch_code);              -- FK index

-- One live session per user (module 08's exception-handling example upserts into it).
CREATE TABLE app_sessions (
    user_id     bigint PRIMARY KEY REFERENCES app_users(id) ON DELETE CASCADE,
    created_at  timestamptz NOT NULL DEFAULT now(),
    token       text NOT NULL
);

-- ============================================================================================
-- 3. The business: applications, documents, payments, audit
-- ============================================================================================

CREATE TABLE loan_applications (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    requested_amount numeric(18,2) NOT NULL CONSTRAINT la_amount_positive CHECK (requested_amount > 0),
    customer_id      bigint NOT NULL REFERENCES customers(id),
    product_id       bigint NOT NULL REFERENCES loan_products(id),
    updated_by       bigint REFERENCES app_users(id),
    submitted_at     timestamptz,
    created_at       timestamptz NOT NULL DEFAULT now(),
    updated_at       timestamptz NOT NULL DEFAULT now(),
    term_months      int NOT NULL CONSTRAINT la_term_range CHECK (term_months BETWEEN 6 AND 360),
    application_no   text NOT NULL,
    branch_code      text NOT NULL REFERENCES branches(code),
    status           text NOT NULL DEFAULT 'DRAFT' REFERENCES workflow_states(code),
    CONSTRAINT la_application_no_key UNIQUE (application_no),
    CONSTRAINT la_application_no_format CHECK (application_no ~ '^LA-[0-9]{4}-[0-9]{6}$'),
    CONSTRAINT la_submitted_needs_time CHECK (status = 'DRAFT' OR submitted_at IS NOT NULL)
);
CREATE INDEX la_customer_submitted_idx ON loan_applications (customer_id, submitted_at DESC);  -- latest per customer; FK
CREATE INDEX la_status_submitted_idx   ON loan_applications (status, submitted_at);           -- work queues; FK
CREATE INDEX la_submitted_id_idx       ON loan_applications (submitted_at, id);               -- keyset pagination
CREATE INDEX la_branch_status_idx      ON loan_applications (branch_code, status);            -- FK + branch dashboards
CREATE INDEX la_product_idx            ON loan_applications (product_id);                     -- FK
CREATE INDEX la_updated_by_idx         ON loan_applications (updated_by);                     -- FK
-- One active application per customer per product: a partial unique index.
CREATE UNIQUE INDEX la_one_active_key  ON loan_applications (customer_id, product_id)
    WHERE status NOT IN ('REJECTED', 'WITHDRAWN', 'CLOSED');

-- Content lives in object storage; the row holds the reference and the checksum.
CREATE TABLE documents (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    application_id  bigint NOT NULL REFERENCES loan_applications(id) ON DELETE CASCADE,  -- true child
    size_bytes      bigint NOT NULL CONSTRAINT doc_size_positive CHECK (size_bytes > 0),
    uploaded_at     timestamptz NOT NULL DEFAULT now(),
    kind            text  NOT NULL REFERENCES document_kinds(code),
    storage_key     text  NOT NULL,
    sha256          bytea NOT NULL CONSTRAINT doc_sha256_len CHECK (length(sha256) = 32),
    ocr_text        text                                              -- optional, large: lives in TOAST (module 02)
);
CREATE INDEX documents_application_idx ON documents (application_id);       -- FK, and the per-application count
CREATE INDEX documents_kind_idx        ON documents (kind);                 -- FK (low cardinality, exists for the FK check)

CREATE TABLE payments (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    application_id  bigint NOT NULL REFERENCES loan_applications(id),
    amount          numeric(18,2) NOT NULL CONSTRAINT pay_amount_positive CHECK (amount > 0),
    posted_at       timestamptz NOT NULL DEFAULT now(),
    kind            text NOT NULL CONSTRAINT pay_kind_valid CHECK (kind IN ('DISBURSEMENT', 'REPAYMENT'))
);
CREATE INDEX payments_application_idx ON payments (application_id, posted_at);   -- FK + statement order

-- Insert-only event log of every workflow transition. Written in the same transaction as the change.
CREATE TABLE application_audit (
    id             bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    application_id bigint NOT NULL REFERENCES loan_applications(id),
    actor_id       bigint REFERENCES app_users(id),
    at             timestamptz NOT NULL DEFAULT now(),                -- transaction time, not clock time
    from_state     text NOT NULL,
    to_state       text NOT NULL,
    note           text
);
CREATE INDEX application_audit_app_idx   ON application_audit (application_id, at);
CREATE INDEX application_audit_actor_idx ON application_audit (actor_id);    -- FK
REVOKE UPDATE, DELETE, TRUNCATE ON application_audit FROM PUBLIC;

-- Archive tables for sp_archive_withdrawn (module 08). LIKE copies columns and NOT NULL,
-- not identity, not FKs: archived rows must not hold locks on live parents.
CREATE TABLE loan_applications_archive (LIKE loan_applications);
CREATE TABLE application_audit_archive (LIKE application_audit);

-- Errors captured by PL/pgSQL exception handlers before re-raising (module 08, 2.4).
CREATE TABLE error_log (
    id       bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    at       timestamptz NOT NULL DEFAULT clock_timestamp(),        -- when the error happened, not when the transaction began
    sqlstate text NOT NULL,
    message  text NOT NULL,
    context  text
);
-- Note: an error_log row written inside a transaction that later rolls back disappears with it.
-- Durable error logging needs a separate connection (dblink) or the server log; this table is for
-- handled-and-continued errors.

-- ============================================================================================
-- 4. Tiny tables for the isolation and deadlock demos (module 05)
-- ============================================================================================

CREATE TABLE accounts (
    id       int PRIMARY KEY,
    balance  numeric(18,2) NOT NULL
);

CREATE TABLE on_call (
    doctor   text    NOT NULL,
    shift    text    NOT NULL,
    on_duty  boolean NOT NULL,
    CONSTRAINT on_call_pkey PRIMARY KEY (doctor, shift)
);

-- ============================================================================================
-- 5. Cache invalidation signal (module 13)
-- ============================================================================================

CREATE TABLE cache_versions (
    table_name text PRIMARY KEY,
    version    bigint NOT NULL DEFAULT 1,
    bumped_at  timestamptz NOT NULL DEFAULT now()
);

CREATE OR REPLACE FUNCTION trg_bump_cache_version() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO cache_versions (table_name) VALUES (TG_TABLE_NAME)
    ON CONFLICT (table_name) DO UPDATE SET version = cache_versions.version + 1, bumped_at = now();
    PERFORM pg_notify('master_changed', TG_TABLE_NAME);   -- delivered at COMMIT to LISTENing sessions
    RETURN NULL;
END $$;

-- Statement-level: one bump per statement, not one per row.
CREATE TRIGGER branches_bump AFTER INSERT OR UPDATE OR DELETE ON branches
    FOR EACH STATEMENT EXECUTE FUNCTION trg_bump_cache_version();
CREATE TRIGGER loan_products_bump AFTER INSERT OR UPDATE OR DELETE ON loan_products
    FOR EACH STATEMENT EXECUTE FUNCTION trg_bump_cache_version();

-- ============================================================================================
-- 6. Triggers on the business tables
-- ============================================================================================

CREATE OR REPLACE FUNCTION trg_touch_updated_at() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END $$;

-- WHEN clause: a no-op UPDATE does not even call the function.
CREATE TRIGGER la_touch BEFORE UPDATE ON loan_applications
    FOR EACH ROW WHEN (OLD.* IS DISTINCT FROM NEW.*) EXECUTE FUNCTION trg_touch_updated_at();

-- ============================================================================================
-- 7. Migration bookkeeping and parity (module 12)
-- ============================================================================================

-- One row per committed batch, written in the SAME transaction as the batch's rows.
CREATE TABLE migration_checkpoints (
    run_id        text   NOT NULL,
    records_done  bigint NOT NULL,
    committed_at  timestamptz NOT NULL DEFAULT now(),
    batch_hash    bigint NOT NULL,
    source_db     text   NOT NULL,
    last_key      bytea  NOT NULL,
    CONSTRAINT migration_checkpoints_pkey PRIMARY KEY (run_id, records_done)
);

-- Records the engine could not decode or map: kept with their raw bytes, never silently dropped.
CREATE TABLE migration_rejects (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    at          timestamptz NOT NULL DEFAULT now(),
    run_id      text  NOT NULL,
    source_db   text  NOT NULL,
    source_key  bytea NOT NULL,
    reason      text  NOT NULL,
    raw         bytea NOT NULL
);
CREATE INDEX migration_rejects_run_idx ON migration_rejects (run_id);

-- The per-record hash computed by the engine from the DECODED SOURCE record.
CREATE TABLE migration_parity (
    hash           bigint NOT NULL,
    migrated_at    timestamptz NOT NULL DEFAULT now(),
    source_db      text  NOT NULL,
    source_key     bytea NOT NULL,
    application_no text  NOT NULL,                                   -- natural key: finds the target row
    CONSTRAINT migration_parity_pkey PRIMARY KEY (source_db, source_key)
);
CREATE INDEX migration_parity_app_no_idx ON migration_parity (application_no);

-- Staging table for COPY. UNLOGGED: no WAL, truncated after a crash, which is fine because the
-- engine redoes any batch whose checkpoint did not commit.
CREATE UNLOGGED TABLE loan_applications_staging (
    requested_amount numeric(18,2),
    submitted_at     timestamptz,
    term_months      int,
    application_no   text,
    national_id      text,
    product_code     text,
    branch_code      text,
    status           text
);

-- FNV-1a 64-bit over bytes, identical to the C++ engine's fnv1a64(). bigint cannot multiply
-- modulo 2^64 without overflow errors, so the arithmetic is done in numeric: slow (use it on
-- samples, not on 50 M rows) but exact. Result returned as a signed bigint (two's complement),
-- which is how the engine prints int64_t.
CREATE OR REPLACE FUNCTION fn_fnv1a64(p_data bytea) RETURNS bigint
LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $$
DECLARE
    c_two64  CONSTANT numeric := 18446744073709551616;
    c_prime  CONSTANT numeric := 1099511628211;
    v_hash   numeric := 14695981039346656037;                       -- FNV offset basis
    v_low    int;
BEGIN
    FOR i IN 0 .. length(p_data) - 1 LOOP
        v_low  := (v_hash % 256)::int;                                -- low byte of the hash
        v_hash := v_hash - v_low + (v_low # get_byte(p_data, i));    -- hash ^= byte  (only the low byte changes)
        v_hash := (v_hash * c_prime) % c_two64;                      -- hash *= prime (mod 2^64)
    END LOOP;
    IF v_hash >= 9223372036854775808 THEN
        v_hash := v_hash - c_two64;                                  -- reinterpret as signed int64
    END IF;
    RETURN v_hash::bigint;
END $$;

-- Canonical form of one application, the SAME definition as canonicalise() in
-- m12_migration_engine.cpp: fields in fixed order, '|' separated, amount in integer cents,
-- date as YYYYMMDD in UTC, '~' as the NULL sentinel (concat_ws would silently skip a NULL,
-- which would make "A|NULL|B" and "A|B" hash equal).
CREATE OR REPLACE FUNCTION fn_parity_canonical_loan(p_id bigint) RETURNS text
LANGUAGE sql STABLE AS $$
    SELECT concat_ws('|',
                     a.application_no,
                     c.national_id,
                     (a.requested_amount * 100)::bigint,
                     a.status,
                     coalesce(to_char(a.submitted_at AT TIME ZONE 'UTC', 'YYYYMMDD'), '~'),
                     a.branch_code)
    FROM loan_applications a JOIN customers c ON c.id = a.customer_id
    WHERE a.id = p_id
$$;

CREATE OR REPLACE FUNCTION fn_parity_hash_loan(p_id bigint) RETURNS bigint
LANGUAGE sql STABLE AS $$
    SELECT fn_fnv1a64(convert_to(fn_parity_canonical_loan(p_id), 'UTF8'))
$$;

-- ============================================================================================
-- 8. Privileges: the application role gets what it needs, nothing more
-- ============================================================================================

GRANT USAGE ON SCHEMA loans TO loans_app;
GRANT SELECT ON branches, roles, workflow_states, workflow_transitions, document_kinds,
                loan_products, product_rates, customers_live TO loans_app;
GRANT SELECT, INSERT, UPDATE ON loan_applications, documents, payments TO loans_app;
GRANT SELECT, INSERT ON application_audit TO loans_app;
-- No SELECT on app_users: password hashes are reached only through SECURITY DEFINER functions (module 08).

-- Sanity check printed at the end of the run.
SELECT count(*) AS tables_created
FROM information_schema.tables
WHERE table_schema = 'loans' AND table_type = 'BASE TABLE';
