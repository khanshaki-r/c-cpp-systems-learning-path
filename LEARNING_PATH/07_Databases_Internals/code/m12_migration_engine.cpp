// m12_migration_engine.cpp : the shape of a KV-store -> relational migration engine (module 12).
//
// Build (no database needed: a fake source and a stub sink are included):
//     g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_migration_engine.cpp -o migrate
//     ./migrate                 # normal run: simulated crash after batch 3, resume, parity report
//     ./migrate --float-bug     # the sink converts money through float: watch parity catch it
//
// Optional real sink against the WSL PostgreSQL with the m09 schema loaded:
//     g++ -std=c++17 -Wall -Wextra -Werror -pthread -DHAVE_LIBPQ -I/usr/include/postgresql m12_migration_engine.cpp -o migrate_pg -lpq
//     ./migrate_pg --libpq "dbname=loans"
// The fake loan numbers (LA-YYYY-9NNNNN) and product BIZ never occur in m09_seed.sql, so the
// real load does not collide with seeded rows.
//
// The four seams (KvSource, decoder, mapper, RelationalSink) are what production swaps:
// FakeKvSource becomes a Berkeley DB cursor (DB_SET_RANGE / DB_NEXT), StubSink becomes LibpqSink.
// The engine loop in MigrationEngine::run() does not change.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
#endif

using Bytes = std::vector<std::uint8_t>;

// ============================================================================================
// 1. Byte-order helpers. Cast to the unsigned type BEFORE shifting: shifting a promoted int
//    into the sign bit is undefined behaviour. Never cast the buffer to a struct pointer.
// ============================================================================================
namespace bytes {

inline std::uint16_t rd_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint32_t rd_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t rd_le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline void wr_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
inline void wr_le32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void wr_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

}  // namespace bytes

// ============================================================================================
// 2. FNV-1a 64: the per-record parity hash. Must match fn_fnv1a64() in m09_schema.sql byte for byte.
// ============================================================================================
inline std::uint64_t fnv1a64(std::string_view s, std::uint64_t h = 14695981039346656037ULL) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;          // unsigned overflow is defined: arithmetic mod 2^64
    }
    return h;
}
inline std::int64_t as_signed(std::uint64_t h) {
    std::int64_t s;
    std::memcpy(&s, &h, sizeof s);      // same bits, printed the way PostgreSQL prints a bigint
    return s;
}

// ============================================================================================
// 3. A per-batch bump arena. Thousands of small strings per batch are carved out of one block
//    and freed together with reset(): O(1) free, no fragmentation over a 12-hour run.
// ============================================================================================
class BatchArena {
public:
    explicit BatchArena(std::size_t capacity)
        : buf_(new std::byte[capacity]), cap_(capacity) {}

    void* allocate(std::size_t n, std::size_t align = alignof(std::max_align_t)) {
        std::size_t start = (off_ + align - 1) & ~(align - 1);
        if (start + n > cap_) throw std::bad_alloc();   // size the arena for the batch, or fall back
        off_ = start + n;
        high_water_ = std::max(high_water_, off_);
        return buf_.get() + start;
    }
    std::string_view copy(std::string_view s) {
        if (s.empty()) return {};
        char* p = static_cast<char*>(allocate(s.size(), 1));
        std::memcpy(p, s.data(), s.size());
        return {p, s.size()};
    }
    std::string_view copy(const Bytes& b) {
        return copy(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    }
    void reset() { off_ = 0; }                          // every pointer handed out is now dead
    std::size_t high_water() const { return high_water_; }

private:
    std::unique_ptr<std::byte[]> buf_;
    std::size_t cap_;
    std::size_t off_ = 0;
    std::size_t high_water_ = 0;
};

// ============================================================================================
// 4. A tiny Result type (C++17 has no std::expected).
// ============================================================================================
template <class T>
class Result {
public:
    static Result ok(T v) { Result r; r.value_ = std::move(v); return r; }
    static Result fail(std::string e) { Result r; r.error_ = std::move(e); return r; }
    explicit operator bool() const { return value_.has_value(); }
    const T& operator*() const { return *value_; }
    const T* operator->() const { return &*value_; }
    const std::string& error() const { return error_; }
private:
    std::optional<T> value_;
    std::string error_;
};

// ============================================================================================
// 5. The legacy record layouts (as written by the old C server with #pragma pack(1) on x86).
//
//   v1 (28 bytes): version u8 | loan_no char[12] "LNYY999999  " space padded
//                  | customer_id u32 LE | amount_cents i32 LE | status char | bcd_date u8[4]
//                  | branch_no u16 BE
//   v2 (35 bytes): version u8 | loan_no char[12] "LNYYYY999999"
//                  | customer_id u32 LE | amount_cents i64 LE | status char | bcd_date u8[4]
//                  | branch_no u16 BE | officer char[3]
//
// branch_no is big-endian because the old code wrote it with htons(). Mixed byte order inside
// one record is exactly the kind of thing the phase-0 inventory must discover.
// ============================================================================================
struct Date { int y = 0, m = 0, d = 0; };

struct LoanRecord {                     // decoded, typed; string_views point into the batch arena
    std::uint8_t version = 0;
    int loan_year = 0;
    int loan_seq = 0;
    std::uint32_t customer_id = 0;
    std::int64_t amount_cents = 0;
    char status = '?';
    std::optional<Date> submitted;      // nullopt for drafts (all-zero BCD)
    std::uint16_t branch_no = 0;
    std::string_view officer;           // empty for v1
};

namespace layout {
constexpr std::size_t kV1Size = 28;
constexpr std::size_t kV2Size = 35;
}

// Packed BCD YYYYMMDD: 0x20 0x24 0x03 0x15 = 2024-03-15. All zero = "no date".
static Result<std::optional<Date>> decode_bcd_date(const std::uint8_t* p) {
    if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0) return Result<std::optional<Date>>::ok(std::nullopt);
    int digits[8];
    for (int i = 0; i < 4; ++i) {
        int hi = p[i] >> 4, lo = p[i] & 0x0F;
        if (hi > 9 || lo > 9) return Result<std::optional<Date>>::fail("invalid BCD nibble in date");
        digits[2 * i] = hi;
        digits[2 * i + 1] = lo;
    }
    Date d;
    d.y = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
    d.m = digits[4] * 10 + digits[5];
    d.d = digits[6] * 10 + digits[7];
    if (d.y < 1990 || d.y > 2100 || d.m < 1 || d.m > 12 || d.d < 1 || d.d > 31)
        return Result<std::optional<Date>>::fail("date out of range");
    return Result<std::optional<Date>>::ok(d);
}

static std::string_view trim_right(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.remove_suffix(1);
    return s;
}

static bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

static int to_int(std::string_view s) {
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

// Decode one value. memcpy-style reads at explicit offsets; version dispatch; every failure is a
// reason string that goes to migration_rejects with the raw bytes. Never throws on bad data.
Result<LoanRecord> decode_loan(const Bytes& v, BatchArena& arena) {
    if (v.empty()) return Result<LoanRecord>::fail("empty value");
    const std::uint8_t* p = v.data();
    LoanRecord r;
    r.version = p[0];
    std::size_t need = r.version == 1 ? layout::kV1Size : r.version == 2 ? layout::kV2Size : 0;
    if (need == 0) return Result<LoanRecord>::fail("unknown record version " + std::to_string(r.version));
    if (v.size() != need)
        return Result<LoanRecord>::fail("bad length " + std::to_string(v.size()) + " for v" + std::to_string(r.version));

    std::string_view loan_no = trim_right(std::string_view(reinterpret_cast<const char*>(p + 1), 12));
    if (loan_no.size() < 2 || loan_no.substr(0, 2) != "LN" || !all_digits(loan_no.substr(2)))
        return Result<LoanRecord>::fail("malformed loan_no");
    if (r.version == 1) {                               // LNYY999999: two-digit year
        if (loan_no.size() != 10) return Result<LoanRecord>::fail("v1 loan_no length");
        r.loan_year = 2000 + to_int(loan_no.substr(2, 2));
        r.loan_seq = to_int(loan_no.substr(4, 6));
    } else {                                            // LNYYYY999999
        if (loan_no.size() != 12) return Result<LoanRecord>::fail("v2 loan_no length");
        r.loan_year = to_int(loan_no.substr(2, 4));
        r.loan_seq = to_int(loan_no.substr(6, 6));
    }

    r.customer_id = bytes::rd_le32(p + 13);
    std::size_t off;
    if (r.version == 1) {
        std::uint32_t raw = bytes::rd_le32(p + 17);
        std::int32_t cents;
        std::memcpy(&cents, &raw, sizeof cents);        // reinterpret bits as signed, no UB
        r.amount_cents = cents;
        off = 21;
    } else {
        std::uint64_t raw = bytes::rd_le64(p + 17);
        std::memcpy(&r.amount_cents, &raw, sizeof r.amount_cents);
        off = 25;
    }
    if (r.amount_cents <= 0) return Result<LoanRecord>::fail("non-positive amount");

    r.status = static_cast<char>(p[off]);
    if (std::string_view("DSARW").find(r.status) == std::string_view::npos)
        return Result<LoanRecord>::fail(std::string("unknown status char '") + r.status + "'");

    auto date = decode_bcd_date(p + off + 1);
    if (!date) return Result<LoanRecord>::fail(date.error());
    r.submitted = *date;
    if (r.status != 'D' && !r.submitted) return Result<LoanRecord>::fail("non-draft without submit date");
    if (r.status == 'D' && r.submitted) return Result<LoanRecord>::fail("draft with submit date");

    r.branch_no = bytes::rd_be16(p + off + 5);          // big-endian: htons() in the old writer
    if (r.branch_no > 499) return Result<LoanRecord>::fail("branch_no out of range");

    if (r.version == 2) r.officer = arena.copy(trim_right(std::string_view(reinterpret_cast<const char*>(p + 32), 3)));
    return Result<LoanRecord>::ok(r);
}

// ============================================================================================
// 6. The mapping document, in code: decoded record -> target row (strings in the arena).
// ============================================================================================
struct LoanRow {
    std::string_view source_key;        // raw key bytes, for migration_parity
    std::string_view application_no;    // LA-YYYY-NNNNNN
    std::string_view national_id;       // customers.national_id, the join key on the target
    std::int64_t amount_cents = 0;
    std::string_view status;            // workflow_states.code
    std::string_view submitted_ymd;     // YYYYMMDD or "~"
    std::string_view branch_code;       // BR0000 .. BR0499
    std::string_view product_code;      // not in the legacy record: mapping rule says BIZ
    int term_months = 0;                // not in the legacy record: mapping rule says 60
    std::int64_t source_hash = 0;
};

static std::string_view status_name(char c) {
    switch (c) {
        case 'D': return "DRAFT";
        case 'S': return "SUBMITTED";
        case 'A': return "APPROVED";
        case 'R': return "REJECTED";
        case 'W': return "WITHDRAWN";
        default:  return "UNKNOWN";     // unreachable: decode_loan rejected it
    }
}

LoanRow map_to_row(const LoanRecord& rec, std::string_view key, BatchArena& arena) {
    char buf[64];
    LoanRow row;
    row.source_key = arena.copy(key);
    std::snprintf(buf, sizeof buf, "LA-%04d-%06d", rec.loan_year, rec.loan_seq);
    row.application_no = arena.copy(buf);
    std::snprintf(buf, sizeof buf, "NID%010u", static_cast<unsigned>(rec.customer_id));
    row.national_id = arena.copy(buf);
    row.amount_cents = rec.amount_cents;
    row.status = status_name(rec.status);               // static storage, no copy needed
    if (rec.submitted) {
        std::snprintf(buf, sizeof buf, "%04d%02d%02d", rec.submitted->y, rec.submitted->m, rec.submitted->d);
        row.submitted_ymd = arena.copy(buf);
    } else {
        row.submitted_ymd = "~";                        // the NULL sentinel of the canonical form
    }
    std::snprintf(buf, sizeof buf, "BR%04u", static_cast<unsigned>(rec.branch_no));
    row.branch_code = arena.copy(buf);
    row.product_code = "BIZ";           // the legacy store held only business loans
    row.term_months = 60;
    return row;
}

// Canonical form: identical to fn_parity_canonical_loan() in m09_schema.sql.
//   application_no|national_id|amount_cents|status|YYYYMMDD or ~|branch_code
static std::string canonical(std::string_view app_no, std::string_view nid, std::int64_t cents,
                             std::string_view status, std::string_view ymd, std::string_view branch) {
    std::string s;
    s.reserve(64);
    s.append(app_no).append("|").append(nid).append("|").append(std::to_string(cents)).append("|")
     .append(status).append("|").append(ymd).append("|").append(branch);
    return s;
}

std::int64_t parity_hash(const LoanRow& r) {
    return as_signed(fnv1a64(canonical(r.application_no, r.national_id, r.amount_cents, r.status,
                                       r.submitted_ymd, r.branch_code)));
}

std::uint64_t batch_hash(const std::vector<LoanRow>& rows) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const auto& r : rows) {
        char b[8];
        std::memcpy(b, &r.source_hash, 8);
        h = fnv1a64(std::string_view(b, 8), h);
    }
    return h;
}

// ============================================================================================
// 7. The seams
// ============================================================================================
struct Checkpoint {
    std::string run_id;
    Bytes last_key;
    std::uint64_t records_done = 0;
    std::uint64_t batch_hash = 0;
};

struct TransientError : std::runtime_error {    // 40001, 40P01, connection reset: retry the batch
    using std::runtime_error::runtime_error;
};

struct KvSource {                               // ordered cursor over (key, value) bytes
    virtual bool seek(const Bytes& start_inclusive) = 0;   // DB_SET_RANGE
    virtual bool next(Bytes& key, Bytes& value) = 0;       // DB_NEXT; false at end
    virtual ~KvSource() = default;
};

struct RelationalSink {                         // one target transaction at a time
    virtual void begin() = 0;
    virtual void upsert_loans(const std::vector<LoanRow>& rows) = 0;   // INSERT ... ON CONFLICT DO NOTHING
    virtual void write_checkpoint(const Checkpoint& cp) = 0;          // same transaction as the rows
    virtual void write_reject(const Bytes& key, std::string_view reason, const Bytes& raw) = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;
    virtual std::optional<Checkpoint> last_checkpoint(std::string_view run_id) = 0;
    virtual ~RelationalSink() = default;
};

// ============================================================================================
// 8. FakeKvSource: a sorted map standing in for a Berkeley DB BTREE, filled with packed records
//    of both versions plus deliberately corrupt ones.
// ============================================================================================
class FakeKvSource final : public KvSource {
public:
    explicit FakeKvSource(int n) {
        std::mt19937 rng(42);                   // fixed seed: same data every run
        const char statuses[] = "SSSAAARRWD";   // skewed: drafts and withdrawals are rare
        for (int i = 0; i < n; ++i) {
            int year = 2019 + i % 6;            // 2019 .. 2024; v1 before 2022, v2 after
            int seq = 900001 + i;               // 9NNNNN: never collides with m09_seed.sql numbers
            char status = statuses[rng() % 10];
            std::int64_t cents = 5000000 + static_cast<std::int64_t>(rng() % 300000000u);
            std::uint32_t cust = 1 + static_cast<std::uint32_t>((static_cast<std::uint64_t>(i) * 7919u) % 200000u);
            std::uint16_t branch = static_cast<std::uint16_t>(rng() % 500);
            int month = 1 + static_cast<int>(rng() % 12), day = 1 + static_cast<int>(rng() % 28);

            Bytes key, val;
            char no[32];                        // room for any int: keeps -Wformat-truncation quiet
            if (year < 2022) {
                std::snprintf(no, sizeof no, "LN%02d%06d  ", year % 100, seq);     // space padded to 12
                val.assign(layout::kV1Size, 0);
                val[0] = 1;
                std::memcpy(&val[1], no, 12);
                bytes::wr_le32(&val[13], cust);
                bytes::wr_le32(&val[17], static_cast<std::uint32_t>(cents));
                put_tail(val, 21, status, year, month, day, branch);
            } else {
                std::snprintf(no, sizeof no, "LN%04d%06d", year, seq);
                val.assign(layout::kV2Size, 0);
                val[0] = 2;
                std::memcpy(&val[1], no, 12);
                bytes::wr_le32(&val[13], cust);
                bytes::wr_le64(&val[17], static_cast<std::uint64_t>(cents));
                put_tail(val, 25, status, year, month, day, branch);
                char officer[4];
                std::snprintf(officer, sizeof officer, "K%02d", static_cast<int>(rng() % 100));
                std::memcpy(&val[32], officer, 3);
            }
            key.assign(no, no + 12);            // the key is the 12 raw loan_no bytes: sorts like the BTREE did

            // The dirty few percent every real store has:
            if (i % 997 == 500) val[0] = 9;                                    // unknown version
            if (i % 1499 == 700) val[val[0] == 1 ? 22 : 26] = 0xAA;            // invalid BCD
            if (i % 2003 == 1000) val.resize(val.size() - 5);                  // truncated by a crash
            if (i % 3001 == 1500 && val[0] == 2 && val.size() == layout::kV2Size)
                bytes::wr_le64(&val[17], static_cast<std::uint64_t>(-12345));  // negative amount

            data_.emplace(std::move(key), std::move(val));
        }
        it_ = data_.end();
    }

    bool seek(const Bytes& start_inclusive) override {
        it_ = data_.lower_bound(start_inclusive);   // DB_SET_RANGE: first key >= start
        return it_ != data_.end();
    }
    bool next(Bytes& key, Bytes& value) override {
        if (it_ == data_.end()) return false;
        key = it_->first;
        value = it_->second;
        ++it_;
        return true;
    }
    std::size_t size() const { return data_.size(); }

private:
    static void put_tail(Bytes& v, std::size_t off, char status, int y, int m, int d, std::uint16_t branch) {
        v[off] = static_cast<std::uint8_t>(status);
        if (status != 'D') {
            auto bcd = [](int x) { return static_cast<std::uint8_t>(((x / 10) << 4) | (x % 10)); };
            v[off + 1] = bcd(y / 100);
            v[off + 2] = bcd(y % 100);
            v[off + 3] = bcd(m);
            v[off + 4] = bcd(d);
        }
        bytes::wr_be16(&v[off + 5], branch);
    }

    std::map<Bytes, Bytes> data_;
    std::map<Bytes, Bytes>::const_iterator it_;
};

// ============================================================================================
// 9. StubSink: an in-memory "database" with transactions, ON CONFLICT semantics, checkpoints,
//    rejects and parity rows. It stores rows as the TARGET sees them (owned strings), and the
//    parity check recomputes the hash from those, independently of the engine's in-flight rows.
// ============================================================================================
struct TargetRow {                      // what SELECT ... FROM loan_applications JOIN customers returns
    std::string application_no, national_id, status, submitted_ymd, branch_code;
    std::int64_t amount_cents = 0;
};

class StubSink final : public RelationalSink {
public:
    explicit StubSink(bool float_bug) : float_bug_(float_bug) {}

    void begin() override {
        if (in_txn_) throw std::logic_error("begin inside a transaction");
        in_txn_ = true;
        pending_rows_.clear(); pending_parity_.clear(); pending_rejects_.clear(); pending_cp_.reset();
    }
    void upsert_loans(const std::vector<LoanRow>& rows) override {
        for (const auto& r : rows) {
            TargetRow t;
            t.application_no.assign(r.application_no);
            t.national_id.assign(r.national_id);
            t.status.assign(r.status);
            t.submitted_ymd.assign(r.submitted_ymd);
            t.branch_code.assign(r.branch_code);
            // The bug this flag simulates: money converted through a float on its way to numeric.
            t.amount_cents = float_bug_
                ? static_cast<std::int64_t>(static_cast<float>(r.amount_cents) / 100.0f * 100.0f)
                : r.amount_cents;
            pending_rows_.push_back(std::move(t));
            pending_parity_.push_back({std::string(r.source_key), {std::string(r.application_no), r.source_hash}});
        }
    }
    void write_checkpoint(const Checkpoint& cp) override { pending_cp_ = cp; }
    void write_reject(const Bytes& key, std::string_view reason, const Bytes& raw) override {
        pending_rejects_.push_back({std::string(key.begin(), key.end()), std::string(reason), raw.size()});
    }
    void commit() override {
        ++commits_attempted_;
        if (commits_attempted_ == fail_commit_number_) {     // one injected serialization failure
            throw TransientError("40001 could not serialize access (injected)");
        }
        for (auto& t : pending_rows_) {
            auto [it, inserted] = table_.emplace(t.application_no, t);   // ON CONFLICT (application_no) DO NOTHING
            (void)it;
            if (!inserted) ++conflicts_skipped_;
        }
        for (auto& p : pending_parity_) parity_[p.first] = p.second;
        for (auto& r : pending_rejects_) rejects_.push_back(r);
        if (pending_cp_) checkpoints_.push_back(*pending_cp_);
        in_txn_ = false;
    }
    void rollback() override {
        pending_rows_.clear(); pending_parity_.clear(); pending_rejects_.clear(); pending_cp_.reset();
        in_txn_ = false;
        ++rollbacks_;
    }
    std::optional<Checkpoint> last_checkpoint(std::string_view run_id) override {
        for (auto it = checkpoints_.rbegin(); it != checkpoints_.rend(); ++it)
            if (it->run_id == run_id) return *it;
        return std::nullopt;
    }

    void fail_on_commit(int n) { fail_commit_number_ = n; }

    // The parity regression suite, scaled down: per-record hash, counts, an aggregate.
    bool parity_report(std::size_t source_records, std::int64_t source_cents_total) const {
        std::size_t matched = 0, mismatched = 0, missing = 0;
        std::int64_t target_cents_total = 0;
        for (const auto& [app_no, row] : table_) target_cents_total += row.amount_cents;
        for (const auto& [key, ph] : parity_) {
            auto it = table_.find(ph.first);
            if (it == table_.end()) { ++missing; continue; }
            const TargetRow& t = it->second;
            std::int64_t recomputed = as_signed(fnv1a64(canonical(t.application_no, t.national_id, t.amount_cents,
                                                                  t.status, t.submitted_ymd, t.branch_code)));
            if (recomputed == ph.second) {
                ++matched;
            } else {
                if (mismatched < 3)
                    std::printf("  MISMATCH %s: source hash %lld, target hash %lld\n", t.application_no.c_str(),
                                static_cast<long long>(ph.second), static_cast<long long>(recomputed));
                ++mismatched;
            }
        }
        std::printf("\n=== parity report ===\n");
        std::printf("source records            : %zu\n", source_records);
        std::printf("target rows               : %zu\n", table_.size());
        std::printf("rejects                   : %zu\n", rejects_.size());
        std::printf("count check (src = tgt + rej): %s\n",
                    source_records == table_.size() + rejects_.size() ? "OK" : "FAIL");
        std::printf("per-record hash           : %zu matched, %zu mismatched, %zu missing\n", matched, mismatched, missing);
        std::printf("amount aggregate (cents)  : source %lld, target %lld -> %s\n",
                    static_cast<long long>(source_cents_total), static_cast<long long>(target_cents_total),
                    source_cents_total == target_cents_total ? "OK" : "FAIL");
        std::printf("checkpoints committed     : %zu, ON CONFLICT skips: %zu, rollbacks: %d\n",
                    checkpoints_.size(), conflicts_skipped_, rollbacks_);
        std::printf("reject reasons (first 5)  :\n");
        for (std::size_t i = 0; i < rejects_.size() && i < 5; ++i)
            std::printf("  key=%s reason=%s raw_len=%zu\n", rejects_[i].key.c_str(), rejects_[i].reason.c_str(),
                        rejects_[i].raw_len);
        return mismatched == 0 && missing == 0 && source_records == table_.size() + rejects_.size() &&
               source_cents_total == target_cents_total;
    }

private:
    struct RejectRow { std::string key, reason; std::size_t raw_len; };
    using ParityEntry = std::pair<std::string, std::pair<std::string, std::int64_t>>;

    bool float_bug_;
    bool in_txn_ = false;
    int commits_attempted_ = 0;
    int fail_commit_number_ = -1;
    int rollbacks_ = 0;
    std::size_t conflicts_skipped_ = 0;
    std::vector<TargetRow> pending_rows_;
    std::vector<ParityEntry> pending_parity_;
    std::vector<RejectRow> pending_rejects_;
    std::optional<Checkpoint> pending_cp_;
    std::map<std::string, TargetRow> table_;                              // keyed by the natural key
    std::map<std::string, std::pair<std::string, std::int64_t>> parity_; // source_key -> (app_no, hash)
    std::vector<RejectRow> rejects_;
    std::vector<Checkpoint> checkpoints_;
};

// ============================================================================================
// 10. LibpqSink (compiled only with -DHAVE_LIBPQ): COPY into a temp staging table, then one
//     set-based INSERT ... SELECT ... ON CONFLICT per batch, checkpoint in the same transaction.
// ============================================================================================
#ifdef HAVE_LIBPQ
class LibpqSink final : public RelationalSink {
public:
    explicit LibpqSink(const char* conninfo) : conn_(PQconnectdb(conninfo)) {
        if (PQstatus(conn_) != CONNECTION_OK) throw std::runtime_error(PQerrorMessage(conn_));
        exec_ok("SET search_path = loans, public");
        exec_ok("SET synchronous_commit = off");         // safe: checkpoint and data commit together
        exec_ok("CREATE TEMP TABLE IF NOT EXISTS stg (LIKE loans.loan_applications_staging, source_key bytea, "
                "source_hash bigint) ON COMMIT DELETE ROWS");
    }
    ~LibpqSink() override { PQfinish(conn_); }
    LibpqSink(const LibpqSink&) = delete;
    LibpqSink& operator=(const LibpqSink&) = delete;

    void begin() override { exec_ok("BEGIN"); }

    void upsert_loans(const std::vector<LoanRow>& rows) override {
        exec_status("COPY stg (requested_amount, submitted_at, term_months, application_no, national_id, "
                    "product_code, branch_code, status, source_key, source_hash) FROM STDIN", PGRES_COPY_IN);
        std::string line;
        for (const auto& r : rows) {
            line.clear();
            char amt[48];
            std::snprintf(amt, sizeof amt, "%lld.%02lld", static_cast<long long>(r.amount_cents / 100),
                          static_cast<long long>(r.amount_cents % 100));   // exact: never through double
            line.append(amt).append("\t");
            if (r.submitted_ymd == "~") line.append("\\N");
            else line.append(r.submitted_ymd).append(" 00:00:00+00");
            line.append("\t").append(std::to_string(r.term_months)).append("\t");
            line.append(r.application_no).append("\t").append(r.national_id).append("\t");
            line.append(r.product_code).append("\t").append(r.branch_code).append("\t").append(r.status).append("\t");
            line.append("\\\\x");                       // COPY text format: backslash must be doubled
            static const char* hex = "0123456789abcdef";
            for (unsigned char c : r.source_key) { line.push_back(hex[c >> 4]); line.push_back(hex[c & 15]); }
            line.append("\t").append(std::to_string(r.source_hash)).append("\n");
            if (PQputCopyData(conn_, line.data(), static_cast<int>(line.size())) != 1) fail("PQputCopyData");
        }
        if (PQputCopyEnd(conn_, nullptr) != 1) fail("PQputCopyEnd");
        check(PQgetResult(conn_), PGRES_COMMAND_OK);
        while (PGresult* extra = PQgetResult(conn_)) PQclear(extra);
        // One statement for the whole batch: idempotent through the natural key.
        exec_ok("INSERT INTO loan_applications (requested_amount, customer_id, product_id, submitted_at, "
                "term_months, application_no, branch_code, status) "
                "SELECT s.requested_amount, c.id, p.id, s.submitted_at, s.term_months, s.application_no, "
                "s.branch_code, s.status FROM stg s "
                "JOIN customers c ON c.national_id = s.national_id "
                "JOIN loan_products p ON p.code = s.product_code "
                "ON CONFLICT (application_no) DO NOTHING");
        exec_ok("INSERT INTO migration_parity (source_db, source_key, application_no, hash) "
                "SELECT 'loans.db', source_key, application_no, source_hash FROM stg "
                "ON CONFLICT (source_db, source_key) DO UPDATE SET hash = EXCLUDED.hash, migrated_at = now()");
    }

    void write_checkpoint(const Checkpoint& cp) override {
        std::string done = std::to_string(cp.records_done);
        std::string bh = std::to_string(as_signed(cp.batch_hash));
        const char* vals[4] = {cp.run_id.c_str(), reinterpret_cast<const char*>(cp.last_key.data()), done.c_str(), bh.c_str()};
        int lens[4] = {0, static_cast<int>(cp.last_key.size()), 0, 0};
        int fmts[4] = {0, 1, 0, 0};                     // last_key sent as binary bytea
        check(PQexecParams(conn_, "INSERT INTO migration_checkpoints (run_id, source_db, last_key, records_done, batch_hash) "
                                  "VALUES ($1, 'loans.db', $2, $3, $4)", 4, nullptr, vals, lens, fmts, 0),
              PGRES_COMMAND_OK);
    }

    void write_reject(const Bytes& key, std::string_view reason, const Bytes& raw) override {
        std::string why(reason);
        const char* vals[3] = {reinterpret_cast<const char*>(key.data()), why.c_str(), reinterpret_cast<const char*>(raw.data())};
        int lens[3] = {static_cast<int>(key.size()), 0, static_cast<int>(raw.size())};
        int fmts[3] = {1, 0, 1};
        check(PQexecParams(conn_, "INSERT INTO migration_rejects (run_id, source_db, source_key, reason, raw) "
                                  "VALUES ('m12-demo', 'loans.db', $1, $2, $3)", 3, nullptr, vals, lens, fmts, 0),
              PGRES_COMMAND_OK);
    }

    void commit() override { exec_ok("COMMIT"); }
    void rollback() override { PQclear(PQexec(conn_, "ROLLBACK")); }

    std::optional<Checkpoint> last_checkpoint(std::string_view run_id) override {
        std::string id(run_id);
        const char* vals[1] = {id.c_str()};
        PGresult* r = PQexecParams(conn_, "SELECT last_key, records_done, batch_hash FROM migration_checkpoints "
                                          "WHERE run_id = $1 ORDER BY records_done DESC LIMIT 1",
                                   1, nullptr, vals, nullptr, nullptr, 1);   // binary results
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); fail("last_checkpoint"); }
        std::optional<Checkpoint> cp;
        if (PQntuples(r) == 1) {
            Checkpoint c;
            c.run_id = id;
            const auto* k = reinterpret_cast<const std::uint8_t*>(PQgetvalue(r, 0, 0));
            c.last_key.assign(k, k + PQgetlength(r, 0, 0));
            const auto* d = reinterpret_cast<const std::uint8_t*>(PQgetvalue(r, 0, 1));
            for (int i = 0; i < 8; ++i) c.records_done = (c.records_done << 8) | d[i];   // int8 binary is big-endian
            cp = c;
        }
        PQclear(r);
        return cp;
    }

private:
    [[noreturn]] void fail(const char* what) {
        std::string msg = std::string(what) + ": " + PQerrorMessage(conn_);
        if (PQstatus(conn_) != CONNECTION_OK) throw TransientError(msg);
        throw std::runtime_error(msg);
    }
    void check(PGresult* r, ExecStatusType want) {
        if (PQresultStatus(r) == want) { PQclear(r); return; }
        const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        std::string msg = PQresultErrorMessage(r);
        bool transient = state && (std::strcmp(state, "40001") == 0 || std::strcmp(state, "40P01") == 0);
        PQclear(r);
        if (transient || PQstatus(conn_) != CONNECTION_OK) throw TransientError(msg);
        throw std::runtime_error(msg);
    }
    void exec_status(const char* sql, ExecStatusType want) { check(PQexec(conn_, sql), want); }
    void exec_ok(const char* sql) { exec_status(sql, PGRES_COMMAND_OK); }

    PGconn* conn_;
};
#endif  // HAVE_LIBPQ

// ============================================================================================
// 11. The engine
// ============================================================================================
struct Config {
    std::string run_id = "m12-demo";
    std::size_t batch_size = 2000;
    int max_retries = 5;
    int crash_after_batches = -1;       // simulate kill -9 after N committed batches (-1: never)
};

struct Reject { Bytes key; std::string reason; Bytes raw; };

class MigrationEngine {
public:
    MigrationEngine(KvSource& src, RelationalSink& sink, Config cfg)
        : src_(src), sink_(sink), cfg_(std::move(cfg)) {}

    void run();
    std::uint64_t done() const { return done_; }

private:
    Bytes resume_point() {
        if (auto cp = sink_.last_checkpoint(cfg_.run_id)) {
            done_ = cp->records_done;
            std::printf("resuming run %s after %llu records\n", cfg_.run_id.c_str(),
                        static_cast<unsigned long long>(done_));
            return cp->last_key;
        }
        return {};
    }
    static void backoff(int attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));   // 1, 2, 4 ms ... add jitter in production
    }

    KvSource& src_;
    RelationalSink& sink_;
    Config cfg_;
    std::uint64_t done_ = 0;
    std::vector<Reject> rejects_;       // copied out of the arena: must survive a batch retry
};

void MigrationEngine::run() {
    Bytes start = resume_point();                 // last checkpoint's key, or empty for the beginning
    if (!src_.seek(start)) return;
    BatchArena arena(8 << 20);                    // 8 MB bump allocator, reset per batch
    int batches = 0;
    for (;;) {
        arena.reset();
        std::vector<LoanRow> rows;
        rows.reserve(cfg_.batch_size);
        Bytes key, val, last_key;
        std::size_t n = 0;
        while (n < cfg_.batch_size && src_.next(key, val)) {
            if (n == 0 && key == start && !start.empty()) continue;      // skip the already-committed resume key
            auto decoded = decode_loan(val, arena);                       // version-dispatched, never throws on data
            if (!decoded) { rejects_.push_back({key, decoded.error(), val}); last_key = key; ++n; continue; }
            std::string_view k(reinterpret_cast<const char*>(key.data()), key.size());
            LoanRow row = map_to_row(*decoded, k, arena);                 // the mapping document, in code
            row.source_hash = parity_hash(row);                           // canonical bytes -> FNV-1a 64
            rows.push_back(row);
            last_key = key;
            ++n;
        }
        if (n == 0) break;
        for (int attempt = 0;; ++attempt) {                              // retry the whole batch: it is idempotent
            try {
                sink_.begin();
                sink_.upsert_loans(rows);
                for (auto& r : rejects_) sink_.write_reject(r.key, r.reason, r.raw);
                sink_.write_checkpoint({cfg_.run_id, last_key, done_ + n, batch_hash(rows)});
                sink_.commit();
                break;
            } catch (const TransientError& e) {
                sink_.rollback();
                std::printf("batch %d attempt %d: %s -> retry\n", batches + 1, attempt + 1, e.what());
                if (attempt >= cfg_.max_retries) throw;
                backoff(attempt);
            }
        }
        done_ += n;
        rejects_.clear();
        start = last_key;
        ++batches;
        std::printf("batch %3d committed: %zu records (%zu rows), total %llu, arena high water %zu KB\n",
                    batches, n, rows.size(), static_cast<unsigned long long>(done_), arena.high_water() / 1024);
        if (cfg_.crash_after_batches == batches) throw std::runtime_error("simulated crash (kill -9)");
        if (n < cfg_.batch_size) break;
    }
}

// ============================================================================================
// 12. main: crash after 3 batches, restart, finish, verify parity.
// ============================================================================================
static std::int64_t source_total_cents(FakeKvSource& src) {
    // Independent pass over the source for the aggregate check: decode with a private arena.
    BatchArena arena(1 << 20);
    std::int64_t total = 0;
    Bytes k, v;
    src.seek({});
    std::size_t i = 0;
    while (src.next(k, v)) {
        if (++i % 1000 == 0) arena.reset();
        auto d = decode_loan(v, arena);
        if (d) total += d->amount_cents;
    }
    return total;
}

int main(int argc, char** argv) {
    bool float_bug = false;
    const char* conninfo = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--float-bug") float_bug = true;
        else if (a == "--libpq" && i + 1 < argc) conninfo = argv[++i];
        else { std::fprintf(stderr, "usage: %s [--float-bug] [--libpq conninfo]\n", argv[0]); return 2; }
    }

    FakeKvSource src(23456);
    Config cfg;

#ifdef HAVE_LIBPQ
    if (conninfo) {
        LibpqSink pg(conninfo);
        MigrationEngine engine(src, pg, cfg);
        engine.run();
        std::printf("loaded %llu records into PostgreSQL; verify with:\n"
                    "  SELECT count(*) FILTER (WHERE fn_parity_hash_loan(a.id) <> p.hash) AS mismatches\n"
                    "  FROM migration_parity p JOIN loan_applications a USING (application_no)\n"
                    "  WHERE random() < 0.01;   -- 1%% sample: fn_fnv1a64 in numeric is slow\n",
                    static_cast<unsigned long long>(engine.done()));
        return 0;
    }
#else
    if (conninfo) { std::fprintf(stderr, "built without -DHAVE_LIBPQ\n"); return 2; }
#endif

    StubSink sink(float_bug);
    sink.fail_on_commit(5);                       // the 5th commit attempt fails once with 40001

    cfg.crash_after_batches = 3;
    try {
        MigrationEngine first(src, sink, cfg);
        first.run();
    } catch (const std::runtime_error& e) {
        std::printf("run 1 died: %s\n", e.what());
    }

    cfg.crash_after_batches = -1;                 // "restart the process": a new engine, same database
    MigrationEngine second(src, sink, cfg);
    second.run();

    bool ok = sink.parity_report(src.size(), source_total_cents(src));
    std::printf("\nRESULT: %s\n", ok ? "PARITY OK" : "PARITY FAILED");
    return ok ? 0 : 1;
}
