# 12. Migrating a key-value store to a relational database

**Time:** ~4 hours   **Prerequisites:** 07, 09, 10, 11

## 1. In plain words

You have years of business data in a key-value store: packed C structs stored as opaque bytes
under packed keys, read and written by one application. You need the same data in relational
tables, with a schema, constraints and SQL, served by a cluster, and the business must not stop
while you do it. That is a migration, and it is one of the hardest things a backend engineer does,
because every mistake is silent: a wrong byte offset turns 3% of balances into garbage and nobody
notices until month end.

The analogy is moving a working hospital to a new building. You cannot close for a month. You
inventory every ward (the KV layouts), label every item (decode the records), design the new
floor plan (the schema), move things in trucks while patients keep arriving (streaming,
batching, checkpoints), check every item arrived (parity verification), run both buildings for a
while with every new patient registered in both (dual write), and only then lock the old doors
(cutover) with a plan to reopen them if the new building floods (rollback).

This module is the methodology, written so you can present your flagship project at senior level:
every decision, why, and what you would do differently. The C++17 skeleton in `code/` is the
engine's shape with a fake source so it compiles and runs without Berkeley DB or libpq.

## 2. The real thing

### 2.1 Phase 0: inventory the key-value layouts

Before any code: a spreadsheet (really) of every database in the environment, and for each:

- Access method, key layout (fields, widths, byte order, sort order), value layout (struct,
  version byte, variable parts), duplicates allowed, secondary databases and their extractors.
- Record counts and total bytes (`db_stat -d`), growth rate, and which records are hot.
- Every writer and reader in the code base (grep for the DB handle). You are about to make
  every one of them a dual writer or a reader of the new store.
- Invariants that were enforced in C: "status is one of these 5 chars", "amount is cents, never
  negative", "customer must exist" (you will turn each into a constraint).
- Known dirt: records written by old versions, orphans (child records whose parent was deleted),
  test data in production, padding bytes with garbage. Sample 10,000 records per database and
  decode them *before* designing the schema; the dirt decides half the design.

Output: a **mapping document** per record type: source field -> target table.column, type
conversion rule, nullability rule, what to do with invalid values (reject, default, quarantine
table). This document is also the specification of the parity checker.

### 2.2 Phase 1: decoding packed records correctly

The records were written by a C compiler with `#pragma pack(1)` (or without, with padding!), on
a specific architecture, with specific conventions. Reading them back in a different program is
where byte-level bugs live.

```c
/* The historical layout. If the original had NO pack pragma, the compiler inserted padding:
 * sizeof may be 40, not 35, and the field offsets depend on the original compiler and ABI. */
#pragma pack(push, 1)
struct loan_rec_v2 {
    uint8_t  version;           /* 2 */
    char     loan_no[12];       /* space-padded, not NUL-terminated */
    uint32_t customer_id;       /* little-endian on x86 */
    int64_t  amount_cents;      /* little-endian */
    char     status;            /* 'D','S','A','R','W' */
    uint8_t  bcd_date[4];       /* YYYYMMDD as packed BCD: 0x20 0x24 0x03 0x15 */
    uint16_t branch_no;         /* big-endian! written by a network-order helper */
    char     officer[3];        /* code */
};                              /* 35 bytes */
#pragma pack(pop)
```

Rules for the decoder (all of them are in `code/m12_packed_record_decode.cpp`):

1. **Never cast the buffer to the struct pointer** (`(struct loan_rec_v2 *)dbt.data`). It is
   undefined behaviour if the buffer is not suitably aligned (Berkeley DB returns arbitrary
   offsets into a page), and it silently depends on the current compiler's layout matching the
   original. Read fields with `memcpy` at explicit offsets, or `std::memcpy` into a local
   properly-aligned variable.
2. **Byte order per field.** x86 wrote little-endian integers unless the code used `htonl`-style
   helpers, in which case big-endian. Decode each with an explicit function: `rd_le32`, `rd_be16`.
   Mixed layouts like the one above happened in real code bases.
3. **Fixed-width strings**: not NUL-terminated; trim trailing spaces (and sometimes NULs);
   decide the character encoding (likely a single-byte code page, convert to UTF-8 explicitly;
   invalid bytes are a parity-check finding, not a crash).
4. **Dates and amounts**: packed BCD, Julian day numbers, days since 1900, cents as integers.
   Map to `date` and `numeric(18,2)`; never through `double`.
5. **Versioned records**: read the version byte first, dispatch to the layout for that version,
   and handle every version ever written. Records with an unknown version go to a quarantine
   table with the raw bytes (`bytea`) for later analysis.
6. **Alignment of the output struct is irrelevant**: decode into a clean C++ struct with natural
   types (`std::string`, `int64_t`, `std::optional<Date>`), which is what the mapping and the
   hash operate on.
7. **Test the decoder against golden records**: pick 50 records across versions, hand-verify
   with `db_dump -p` and a hex editor, and make them unit tests.

### 2.3 Phase 2: design the relational schema

Module 09 covers the design rules. Migration-specific points:

- Keep the **natural key** as a `UNIQUE` constraint so the parity checker can find each target
  row from the source key. Add the surrogate `bigint` id for the future.
- Add a **`source_hash bytea`** (or `bigint`) column, or a side table `migration_parity(source_db,
  source_key bytea, hash bigint, migrated_at)`, so parity can be re-verified without re-decoding.
- Every invariant found in phase 0 becomes a constraint, but create them `NOT VALID` during
  the load and `VALIDATE` after, or load into staging tables first, so a single dirty record
  does not abort a 10 M row batch.
- Decide the fate of denormalised copies: the loan record carried `cust_name`; the target has
  it only on `customers`. The parity rule becomes "loan.cust_name == customers.full_name joined
  by customer_id", and mismatches are findings about the *source* data (stale copies), which the
  business must adjudicate before cutover.
- For the cluster (module 11): choose sharding and colocation now; the engine's write pattern
  (batched inserts by source key order) interacts with range-sharded keys (hotspots on the last
  tablet). Hash sharding on the natural key spreads a bulk load across all tablets.

### 2.4 Phase 3: the migration engine

```
 +-----------+     +-----------+     +-----------+     +-------------+     +--------------+
 | KV source | --> | decoder   | --> | mapper    | --> | batcher     | --> | relational   |
 | cursor    |     | bytes ->  |     | record -> |     | arena,      |     | sink: COPY / |
 | SET_RANGE |     | typed rec |     | row(s)    |     | N rows/txn  |     | INSERT ...   |
 | NEXT      |     |           |     |           |     |             |     | ON CONFLICT  |
 +-----------+     +-----------+     +-----------+     +------+------+     +------+-------+
       ^                                                      |                   |
       |                 +------------------------------------+                   |
       |                 v                                                        v
       |          +-------------+   after each committed batch              +-----------+
       +--------- | checkpoint  | <----------------------------------------- | parity    |
   resume from    | store       |   (last_key, rows, hash, time)             | hashes    |
   last key       +-------------+                                            +-----------+
```

**Streaming cursor, not a load into memory.** The source is read with `DB_SET_RANGE` from the
last checkpointed key and `DB_NEXT` forward; the engine never holds more than one batch. On the
source, use short read transactions (or snapshot reads) per batch to avoid holding page locks
against the live application (module 10); reposition by key at each batch.

**Batching.** A batch of 1,000 to 10,000 source records becomes one target transaction: decoded,
mapped, written with `COPY` into staging or multi-row `INSERT ... ON CONFLICT`, committed. Batch
size trades throughput (per-transaction overhead, fsyncs, on the cluster one distributed commit)
against lock duration, WAL/intents per transaction, and the cost of redoing a failed batch.
Measure; 5,000 was typical.

**Idempotency.** Any batch may be retried after a crash, so every write must be safe to repeat:
`INSERT ... ON CONFLICT (natural_key) DO NOTHING` for an initial load, or `DO UPDATE SET ...
WHERE target.source_hash <> EXCLUDED.source_hash` when re-running over changed data. Child rows
(documents of a loan) are deleted and re-inserted per parent within the batch transaction, or
also keyed by a natural key. Never `INSERT` without a conflict clause in a restartable engine.

**Checkpoints and restartability.** After each batch commits, write a checkpoint row in the
*target* database, in the same transaction as the batch:
`(run_id, source_db, last_key bytea, records_done, batch_hash, committed_at)`. The checkpoint
and the data commit atomically, so after a crash the engine reads the last checkpoint and
`DB_SET_RANGE` to `last_key`, skipping the first record (already done). Also record failed
records in a `migration_rejects(source_key, reason, raw bytea)` table rather than stopping.

**Arena allocation per batch.** Decoding 5,000 records produces thousands of small strings and
rows. Allocating and freeing them individually is slow and fragments the heap over a 12-hour
run. Allocate from a per-batch arena (a bump allocator over a few MB) and reset it after the
batch commits: O(1) free, no fragmentation, cache-friendly. PostgreSQL does exactly this with
memory contexts (module 01). In C++17 this is a `std::pmr::monotonic_buffer_resource` with
`std::pmr::vector`/`std::pmr::string`, or a hand-written bump allocator as in the skeleton.

**Parallelism.** Partition the key space into ranges (one per worker, by key prefix or by
branch), run one engine instance per range with its own checkpoint row. On a hash-sharded
target the writes spread; on PostgreSQL, 4 to 8 parallel COPY streams usually saturate the disk.

**Throughput controls.** `synchronous_commit = off` for the load session (crash loses at most
the last batch, which the checkpoint logic redoes anyway, because checkpoint and data are in
the same transaction); indexes created after the load; `maintenance_work_mem` high for the index
builds; `ANALYZE` at the end; on YugabyteDB, the bulk load path with large batches, and run
`ANALYZE` explicitly.

### 2.5 Phase 4: data-parity verification

Parity is not "row counts match". It is a **regression suite** that proves, for every record, the
relational form equals the source form under the mapping rules, and keeps proving it while both
systems run.

1. **Canonical serialisation and hash per record.** Define, in the mapping document, a canonical
   byte string for each entity: fields in a fixed order, normalised (trimmed, UTF-8, cents as
   integers, dates as `YYYYMMDD`, NULL as a sentinel). Hash it (64-bit FNV-1a or xxHash is enough
   for parity; SHA-256 if someone demands collision resistance). Compute it from the *decoded
   source record* in the engine, and recompute it from the *target row* with a SQL function
   (`fn_parity_hash_loan(application_id)`) that applies the same canonicalisation. Store both.
   Mismatch = a mapping bug or a decoder bug, found per record.
2. **Row counts** per entity and per partition (per branch, per status), source versus target,
   including rejects: `source_count = target_count + rejects`.
3. **Aggregates** that the business already watches: sum of outstanding balances per branch,
   count of applications per status. If the migration changes the month-end report by one rupee,
   the business finds it before you do, so find it first.
4. **Sampled deep diff**: random sample (and a stratified sample across versions, branches, and
   edge cases: empty strings, max amounts, oldest dates), decode the source, read the target rows
   via SQL, compare field by field with a report of every difference. Run it on 0.1% then on 100%
   if the hardware allows (a full diff of 50 M records at 50k/s is 17 minutes).
5. **Continuous diff during dual write** (next section): a daemon that re-hashes records changed
   in the last N minutes on both sides and alerts on mismatch. This is what makes the dual-write
   period safe rather than hopeful.
6. Keep all findings in a table with a status (`open`, `source_bug`, `mapping_fixed`,
   `accepted`); the sign-off for cutover is "zero open findings and all accepted ones documented".

The suite runs on every engine change, like unit tests. That is why "regression suite" is the
right phrase on your resume.

### 2.6 Phase 5: cutover strategies

| Strategy | How | Downtime | Risk | When |
|---|---|---|---|---|
| **Big bang** | stop writes, run final load, verify, switch readers and writers, start | hours | one shot; rollback = switch back if the old store is untouched | small data, tolerable maintenance window, simple dependencies |
| **Dual write** | application writes to both stores (old first, then new) for weeks; reads from old; parity daemon compares; then flip reads; then stop writing old | none for reads; a brief write pause at flips | application-level complexity; partial failures (old ok, new failed) need reconciliation; both stores must stay consistent | your case: single application owns all writes, long verification wanted |
| **CDC (change data capture)** | initial load, then tail the source's change log and apply to target until lag is zero, flip | seconds | needs a change stream from the source: Berkeley DB has none built in, so you instrument the application's write path (a write-ahead queue table or log) | sources with logical replication (PostgreSQL to anything); or when you can hook every write |

The strategy for a Berkeley DB source owned by one application server was dual write with an
instrumented write path, which is CDC in spirit: every write goes through one function in the C
server, so that function wrote to both stores and, on failure of the new store, appended the
record key to a **replay queue** reconciled by the engine. This is the shape to describe.

### 2.7 Phase 6: rollback plan

Rollback is a plan written *before* cutover, with a decision owner and a time box. Elements:

- **Keep the old store writable and current** until the end of the soak period: dual write
  continues *after* reads flip, in reverse (new is primary, old is secondary), so the old store
  can take over with no data loss.
- **Triggers for rollback**, decided in advance: parity findings above a threshold, p99 latency
  above X for Y minutes, error rate, a failed business report. Not "it feels wrong".
- **Procedure**: flip the read switch back (a configuration flag, not a deployment), confirm
  with a smoke test, open an incident, keep dual writing. The ability to roll back must be tested
  in staging as a drill.
- **Point of no return**: when the application starts using features the old store cannot
  represent (new columns, new tables) or when the old store is frozen. Announce it; after it,
  rollback means restoring from backup plus replaying, and the plan says so honestly.

### 2.8 Zero-downtime cutover runbook

A runbook is a numbered list with an owner, a check, and a rollback step per line. The skeleton:

```
T-14d  1. Final mapping document and parity suite signed off (owner: you; check: zero open findings)
       2. Target schema deployed with constraints NOT VALID, indexes minimal (owner: DBA)
       3. Dual-write code deployed behind a flag, OFF (owner: app lead; check: flag read in logs)
T-10d  4. Flag dual-write ON (old primary, new secondary). Check: replay queue depth ~0 after 1 h.
          Rollback: flag OFF.
       5. Bulk load of history with the engine, 8 workers, from a snapshot taken at step 4's time.
          Check: checkpoints advance; rejects < threshold. Rollback: none needed (target not read).
       6. Reconcile: engine re-runs over keys changed since snapshot (from the replay/changes log).
          Check: counts match per entity.
T-7d   7. Parity full diff. Check: 0 mismatches; aggregates match month-end report. Fix, re-run.
       8. VALIDATE constraints; build remaining indexes CONCURRENTLY; ANALYZE. Check: plans of the
          top 20 queries (module 06) are index-driven on the cluster (Storage Read Requests small).
       9. Continuous parity daemon ON. Check: alerts wired, test alert fired.
T-3d  10. Shadow reads: application reads from both, serves old, logs differences. Check: 24 h, 0 diffs.
          Rollback: disable shadow reads.
T-0   11. Flip reads to new (flag). Check: latency and error dashboards for 1 h; parity daemon quiet.
          Rollback: flip flag back (tested in staging on T-5d).
      12. Swap dual-write order: new primary, old secondary. Check: replay queue for old ~0.
T+7d  13. Soak. Daily parity, daily business reports compared. Rollback remains one flag.
T+14d 14. Point of no return announced; stop writing old (flag). Old store read-only, backed up,
          retained 1 year. Remove dual-write code in the next release.
```

Every "flag" is a configuration value read at runtime, so cutover and rollback are not
deployments. Every step has a check that is a number, not a feeling.

## 3. Code

`code/m12_migration_engine.cpp` (C++17, compiles with
`g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_migration_engine.cpp -o migrate`) implements the
engine shape with abstract interfaces so it runs standalone:

```cpp
// The four seams. Production swaps FakeKvSource for a Berkeley DB cursor and StubSink for libpq;
// the engine does not change.
struct KvSource {                 // ordered cursor over (key, value) bytes
    virtual bool seek(const Bytes& start_inclusive) = 0;   // DB_SET_RANGE
    virtual bool next(Bytes& key, Bytes& value) = 0;       // DB_NEXT; false at end
    virtual ~KvSource() = default;
};
struct RelationalSink {           // one target transaction at a time
    virtual void begin() = 0;
    virtual void upsert_loans(const std::vector<LoanRow>& rows) = 0;   // INSERT ... ON CONFLICT DO NOTHING / UPDATE
    virtual void write_checkpoint(const Checkpoint& cp) = 0;          // same transaction as the rows
    virtual void write_reject(const Bytes& key, std::string_view reason, const Bytes& raw) = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;
    virtual std::optional<Checkpoint> last_checkpoint(std::string_view run_id) = 0;
    virtual ~RelationalSink() = default;
};
```

The batch loop, which is the heart of it:

```cpp
void MigrationEngine::run() {
    Bytes start = resume_point();                 // last checkpoint's key, or empty for the beginning
    if (!src_.seek(start)) return;
    BatchArena arena(8 << 20);                    // 8 MB bump allocator, reset per batch
    for (;;) {
        arena.reset();
        std::vector<LoanRow> rows; rows.reserve(cfg_.batch_size);
        Bytes key, val, last_key;
        std::size_t n = 0;
        while (n < cfg_.batch_size && src_.next(key, val)) {
            if (n == 0 && key == start && !start.empty()) continue;      // skip the already-committed resume key
            auto decoded = decode_loan(val, arena);                       // memcpy-based, version-dispatched
            if (!decoded) { rejects_.push_back({key, decoded.error(), val}); last_key = key; ++n; continue; }
            LoanRow row = map_to_row(*decoded, arena);                   // mapping document, in code
            row.source_hash = parity_hash(*decoded);                     // canonical bytes -> FNV-1a 64
            rows.push_back(std::move(row));
            last_key = key; ++n;
        }
        if (n == 0) break;
        for (int attempt = 0; ; ++attempt) {                             // retry the whole batch: it is idempotent
            try {
                sink_.begin();
                sink_.upsert_loans(rows);
                for (auto& r : rejects_) sink_.write_reject(r.key, r.reason, r.raw);
                sink_.write_checkpoint({cfg_.run_id, last_key, done_ + n, batch_hash(rows)});
                sink_.commit();
                break;
            } catch (const TransientError& e) {
                sink_.rollback();
                if (attempt >= cfg_.max_retries) throw;
                backoff(attempt);                                        // 40001/40P01/connection loss on the cluster
            }
        }
        done_ += n; rejects_.clear(); start = last_key;
        if (n < cfg_.batch_size) break;
    }
}
```

Also in the file: `FakeKvSource` generating versioned packed records (including deliberately
corrupt ones), the decoder with `rd_le32/rd_be16/bcd_date`, the canonical serialiser and
FNV-1a hash, `StubSink` that keeps rows in memory and prints a parity report (`source hash ==
recomputed hash from the "row"`), and a `#ifdef HAVE_LIBPQ` section sketching `LibpqSink`
with `COPY ... FROM STDIN` into a staging table followed by `INSERT ... SELECT ... ON CONFLICT`.
`code/m12_packed_record_decode.cpp` is the byte-level decoder with tests on golden records.

The parity side in SQL, from `code/m09_schema.sql`'s companion functions:

```sql
-- Same canonical form as the C++ canonicalise(): fields in fixed order, '|' separated, normalised.
CREATE OR REPLACE FUNCTION fn_parity_canonical_loan(p_id bigint) RETURNS text LANGUAGE sql STABLE AS $$
    SELECT concat_ws('|', a.application_no, c.national_id, (a.requested_amount * 100)::bigint,
                     a.status, to_char(a.submitted_at AT TIME ZONE 'UTC', 'YYYYMMDD'), a.branch_code)
    FROM loan_applications a JOIN customers c ON c.id = a.customer_id WHERE a.id = p_id
$$;
-- Hash with the same FNV-1a 64 as the engine (implemented in PL/pgSQL in the code file), compare to migration_parity.hash
```

## 4. Under the hood (senior-level)

**Why `memcpy` and not a cast.** The C and C++ standards require an object of type `T` to be
accessed through a pointer suitably aligned for `T` (except via `char`/`unsigned char`
pointers). `*(uint32_t*)(buf + 13)` on x86 happens to work (x86 tolerates unaligned loads, with
a small penalty across cache lines), but on ARM it may fault or silently load from the rounded
address, and the compiler is allowed to assume alignment and emit instructions (SSE `movaps`)
that fault. `memcpy(&v, buf + 13, 4)` compiles to the same single `mov` on x86 with no UB, and
the compiler knows it. Strict aliasing is the second problem: accessing the same bytes as both
`char[]` and `struct loan_rec_v2` is permitted only in one direction (char access to anything is
fine; struct access to a char buffer is not). Decode through `memcpy`; the optimiser removes it.

**Endianness in practice.** Little-endian stores the least significant byte first; x86 and
modern ARM Linux are little-endian; "network order" and many legacy formats are big-endian. A
record written with `htonl()` on x86 has big-endian integers, and a developer reading it back
with a plain struct on x86 gets byte-swapped values. Write `rd_be32` as
`(uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]` and `rd_le32` the
other way; cast to the unsigned type *before* shifting to avoid signed-shift UB.

**Alignment pitfalls in the *original* writer.** If the original struct had no pack pragma, the
compiler inserted padding (a `uint32_t` after a `char[13]` starts at offset 16, not 13) and
`sizeof` was rounded up to the largest alignment. The padding bytes contain whatever was on the
stack: so two logically identical records have different bytes, and hashing the raw value for
parity is wrong; hash the *decoded* fields. The offsets are those of the original compiler and
ABI (MSVC x86 and GCC x86-64 agree on natural alignment; bitfields and `long` do not). Your phase
0 inventory must record the original compiler.

**Arena allocators and locality.** A bump allocator hands out consecutive addresses, so the
5,000 decoded records of a batch are contiguous in memory: fewer cache misses when serialising
them, no per-object `malloc` metadata, no `free` calls, and no fragmentation growth over a 12-hour
run (glibc `malloc` fragmentation on long-running processes with mixed sizes is real). The cost:
you cannot free one object early, and anything that must outlive the batch (rejects queued for
the next transaction, the checkpoint) must be copied out. `std::pmr::monotonic_buffer_resource`
gives this with standard containers; see 02_Modern_CPP for allocators.

**What COPY into staging buys on the cluster.** On PostgreSQL, `COPY` avoids per-row parsing
and uses multi-insert (module 07). On YugabyteDB, large multi-row inserts are batched into fewer
RPCs per tablet (`ysql_session_max_batch_size`), and the staging-then-`INSERT ... SELECT ... ON
CONFLICT` pattern turns the idempotency check into one distributed statement per batch instead of
per row. Hash-sharded staging tables spread the load.

**Checkpoint atomicity.** The checkpoint row commits in the same transaction as the batch's rows,
so the two possible states after a crash are "batch and checkpoint both committed" or "neither".
With `synchronous_commit = off` the last committed batch may be lost on a *database* crash, but
the checkpoint is lost with it, so the engine redoes the batch: idempotency makes the redo
harmless. This is the same argument as PostgreSQL's own redo being idempotent (module 04).

**Hashing for parity.** FNV-1a 64 is fast and good enough to detect accidental mismatches; it is
not collision resistant against an adversary, which is not the threat model. The canonical form
must be defined so that two representations that the *business* considers equal hash equal
(trailing spaces, `'00'` vs `0`, time zone of dates); every parity bug you will find is a
canonicalisation disagreement between the C++ and the SQL implementations, so test them against
each other on golden records first.

## 5. In production

- **The dirty 3%.** Every real KV store has records no current code can write: old versions,
  truncated values from a crash, test data. Design the quarantine path first; a migration that
  stops on the first bad record never finishes, and one that silently defaults bad fields
  corrupts the target.
- **Source load.** A full cursor scan of the source competes with production for the mpool and
  takes page read locks. Run with snapshot reads or short transactions, throttle (sleep between
  batches when the application's latency rises), and schedule the bulk pass off-peak.
- **Target lock and WAL impact.** Bulk load generates GBs of WAL and, on PostgreSQL, bloats
  nothing (inserts only) but forces checkpoints and FPIs; on YugabyteDB it triggers compactions.
  Watch replication lag (physical standbys must keep up) and disk.
- **Clock and ordering.** When reconciling changes made during the load, "changed since" must
  be judged by the source's own change log sequence, not by wall clock on two machines.
- **Dual-write failure modes**: new store down (append to replay queue, alert, keep serving);
  old store down (that is the old outage, unchanged); both succeed but differ (parity daemon
  catches it; the usual cause is a code path that bypassed the common write function). Grep for
  every write site in phase 0 for exactly this reason.
- **Observability**: records/s, batches/s, rejects/s, checkpoint lag, parity mismatches, p99
  of the application during the load. A migration without a dashboard is a migration run blind.
- **What you would do differently** (interviewers ask): start the parity suite before the
  engine, not after; put the canonicalisation in one shared spec with generated code for both C++
  and SQL; use CDC-style change logging from day one so the reconciliation pass is mechanical;
  rehearse rollback in staging earlier.
- **Numbers to have ready**: record count and bytes migrated, batch size, sustained records/s,
  total bulk-load hours, number of record versions handled, reject rate, parity mismatches found
  and their root causes, dual-write duration, cutover downtime (zero for reads; seconds for the
  flag flips). Use your real ones; say "roughly" where memory is approximate.

## 6. Try it

1. **(easy)** Build and run `code/m12_migration_engine.cpp`. Read the parity report. Done when
   you can explain why every record's source hash equals the recomputed hash, and what the
   deliberately corrupt records did.
2. **(easy)** Build `code/m12_packed_record_decode.cpp` and run its tests. Change one field to
   the wrong endianness and watch which test fails. Done when you can explain the failing bytes.
3. **(medium)** Add a kill switch to the engine (e.g. throw after batch 3), run it, run it
   again, and verify with the stub sink's log that no record is written twice and the resume key
   is correct. Done when the second run starts at the right key and the final counts are exact.
4. **(medium)** Implement `LibpqSink` for real against your WSL PostgreSQL (`#define
   HAVE_LIBPQ`, link `-lpq`), with `COPY` into a staging table and `INSERT ... SELECT ... ON
   CONFLICT DO NOTHING` into `loan_applications`. Load 1 M fake records and run the SQL parity
   function over a 1% sample. Done when the sample reports 0 mismatches and `migration_checkpoints`
   shows the batches.
5. **(hard)** Write the dual-write function for the C server in pseudocode or C: write old, write
   new, on new-failure enqueue to a replay table, with idempotency keys. Then write the parity
   daemon's SQL that re-hashes records changed in the last 10 minutes. Done when you can walk a
   reviewer through every failure branch.
6. **(hard)** Write the complete cutover runbook for the loan schema migration (the 14-step
   skeleton expanded to your real system's components), with a check and rollback per step, and
   present it in 10 minutes to someone else. Done when they can find no step without a number to
   check.

## 7. Interview questions

**Q: Walk me through how you migrated the Berkeley DB data to YugabyteDB.**
A: Inventory first: every database, key and record layout, version, writer and reader, with a
mapping document per record type that doubled as the parity specification. A decoder that read
packed records by explicit offsets with per-field byte order and version dispatch, quarantining
unknown versions. A streaming engine: cursor from the last checkpoint, batches of a few thousand
records decoded into a per-batch arena, idempotent upserts into the target in one transaction
with the checkpoint row, retried on transient errors. A parity suite of per-record canonical
hashes, counts and aggregates, sampled deep diffs, and a continuous diff during dual write. Then
dual write, shadow reads, a flag flip of reads, a soak, and retiring the old store, with rollback
being a flag until the announced point of no return.

**Q: How did you make the engine restartable?**
A: Each batch commits its rows and a checkpoint row (run id, last source key, counts, batch hash)
in the same transaction, so after a crash the engine resumes from the last checkpointed key by
positioning the cursor with a range seek. All writes are idempotent upserts keyed by the natural
key, so redoing a batch whose commit was lost is harmless.

**Q: What byte-level problems did decoding packed records raise?**
A: Unaligned access if you cast the buffer to a struct, so I read fields with memcpy at explicit
offsets; mixed byte order where some fields were written in network order; fixed-width
space-padded strings in a legacy code page needing explicit trimming and UTF-8 conversion;
packed BCD dates and integer cents; and padding bytes with stack garbage in records written by an
unpacked struct, which means you must hash decoded fields, never raw bytes, for parity.

**Q: How did you prove the data was correct?**
A: A canonical serialisation per entity defined in the mapping document, hashed in the engine
from the decoded source and recomputed in SQL from the target rows; per-partition row counts
including rejects; business aggregates compared with existing month-end reports; stratified
sampled field-by-field diffs; and a continuous re-hash of recently changed records on both sides
during the dual-write period. Findings went into a tracked table and cutover required zero open
findings.

**Q: Why dual write rather than big bang or CDC?**
A: The business could not accept hours of downtime, and Berkeley DB has no change stream to tail
for CDC, but a single application owned every write through one code path, so instrumenting that
path to write both stores with a replay queue for failures gave CDC-like completeness with a long
verification window. The cost was application complexity and a reconciliation step, which the
parity suite covered.

**Q: How was the cutover zero-downtime and how could you roll back?**
A: Reads and the write order were runtime flags, not deployments. After the bulk load,
reconciliation, full parity and shadow reads, we flipped reads to the new store, then made it the
primary for writes while still writing the old store, so for the soak period rollback was flipping
the read flag back with no data loss. The point of no return was when we stopped writing the old
store, announced in advance, after which rollback would have meant restore plus replay.

**Q: What would you do differently?**
A: Build the parity suite before the engine, generate the canonicalisation code for both C++ and
SQL from one spec to avoid the disagreements that caused most mismatches, log changes from the
first day so reconciliation was mechanical, and rehearse rollback in staging earlier.

## 8. Recap

- Inventory every layout, writer, invariant and dirt sample before designing anything.
- Decode with memcpy at explicit offsets, per-field byte order, version dispatch, quarantine for unknowns.
- Schema: natural key UNIQUE, surrogate id, source hash stored, constraints NOT VALID then VALIDATE.
- Engine: streaming cursor from last key, batches of thousands, per-batch arena, idempotent upserts.
- Checkpoint row commits with the batch; resume by range seek; retry transient errors; reject table.
- Parity: canonical hash per record on both sides, counts per partition, aggregates, sampled deep diff, continuous diff.
- Cutover: big bang vs dual write vs CDC; dual write with a replay queue when the source has no change log.
- Rollback: flags not deployments, pre-agreed triggers, drilled in staging, announced point of no return.
- Runbook: numbered steps, each with an owner, a numeric check, and a rollback action.
- Have your real numbers and your "what I would change" ready.
