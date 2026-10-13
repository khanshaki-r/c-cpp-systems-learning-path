// m10_api_quality.cpp
// API DESIGN AND CODE QUALITY, demonstrated on a small funds-transfer service.
//
//   1. Strong types        : AccountId, Paise, Ifsc. Swapping two arguments or
//                            adding rupees to an account id does not compile.
//   2. Parse, don't check  : Ifsc can only be created through parse(), so every
//                            Ifsc in the program is valid by construction.
//   3. [[nodiscard]]       : on Result and on functions whose return value is
//                            the whole point (try_reserve, parse).
//   4. const and noexcept  : const member functions, noexcept moves and swaps.
//   5. enum instead of bool: post(..., Posting::DryRun) reads at the call site.
//   6. Seams               : Clock, Ledger, Notifier are interfaces injected by
//                            reference. Tests use a FAKE ledger (working, simple)
//                            and a MOCK notifier (records calls, verifies them).
//   7. Versioning          : inline namespace v2 so old binaries keep linking.
//   8. Strangler router    : send some traffic to new code, keep the rest on the
//                            legacy path, shadow-compare both, count mismatches.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m10 m10_api_quality.cpp

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ===========================================================================
// 1. Strong types. Zero runtime cost: same size and codegen as the raw int.
// ===========================================================================
template <class Tag, class Rep>
class Strong {
public:
    constexpr explicit Strong(Rep v) noexcept : v_(v) {}   // explicit: no silent conversion
    constexpr Rep get() const noexcept { return v_; }
    friend constexpr bool operator==(Strong a, Strong b) noexcept { return a.v_ == b.v_; }
    friend constexpr bool operator!=(Strong a, Strong b) noexcept { return a.v_ != b.v_; }
    friend constexpr bool operator<(Strong a, Strong b) noexcept { return a.v_ < b.v_; }
private:
    Rep v_;
};

using AccountId = Strong<struct AccountIdTag, std::uint64_t>;
static_assert(sizeof(AccountId) == sizeof(std::uint64_t), "strong type must be free");

// Money gets arithmetic, but only with itself. Paise * Paise is meaningless,
// so it is not provided; Paise * integer is.
class Paise {
public:
    constexpr explicit Paise(std::int64_t v) noexcept : v_(v) {}
    constexpr std::int64_t get() const noexcept { return v_; }
    constexpr Paise operator+(Paise o) const noexcept { return Paise(v_ + o.v_); }
    constexpr Paise operator-(Paise o) const noexcept { return Paise(v_ - o.v_); }
    constexpr Paise operator*(std::int64_t k) const noexcept { return Paise(v_ * k); }
    constexpr bool operator<(Paise o) const noexcept { return v_ < o.v_; }
    constexpr bool operator==(Paise o) const noexcept { return v_ == o.v_; }
private:
    std::int64_t v_;
};

// The swap bug strong types prevent:
//   void transfer(uint64_t from, uint64_t to, int64_t amount);
//   transfer(to, from, amount);            // compiles, moves money the wrong way
// With AccountId for both it still compiles (same type). The fix is a
// request struct with NAMED fields (section 3) or distinct types for roles.
static_assert(!std::is_convertible_v<std::uint64_t, AccountId>, "no implicit int -> id");
static_assert(!std::is_convertible_v<AccountId, Paise>, "ids are not money");

// ===========================================================================
// 2. Parse, don't validate. Ifsc: 4 letters, '0', 6 alphanumerics.
// ===========================================================================
class Ifsc {
public:
    [[nodiscard]] static std::optional<Ifsc> parse(std::string_view s) {
        if (s.size() != 11 || s[4] != '0') return std::nullopt;
        for (int i = 0; i < 4; ++i) if (!std::isupper(static_cast<unsigned char>(s[i]))) return std::nullopt;
        for (int i = 5; i < 11; ++i) if (!std::isalnum(static_cast<unsigned char>(s[i]))) return std::nullopt;
        return Ifsc(std::string(s));
    }
    const std::string& str() const noexcept { return code_; }   // const ref: no copy, no mutation
private:
    explicit Ifsc(std::string c) : code_(std::move(c)) {}       // private: parse() is the only door
    std::string code_;
};

// ===========================================================================
// 3. Result type and request struct.
// ===========================================================================
enum class TransferError { SameAccount, NonPositiveAmount, InsufficientFunds, UnknownAccount };

class [[nodiscard]] TransferResult {
public:
    static TransferResult success(std::uint64_t ref) { return TransferResult(ref, {}); }
    static TransferResult failure(TransferError e) { return TransferResult(0, e); }
    bool ok() const noexcept { return !err_; }
    std::uint64_t reference() const { assert(ok()); return ref_; }
    TransferError error() const { assert(!ok()); return *err_; }
private:
    TransferResult(std::uint64_t r, std::optional<TransferError> e) : ref_(r), err_(e) {}
    std::uint64_t ref_;
    std::optional<TransferError> err_;
};

// Named fields: designated-initialiser-like readability at the call site,
// and no positional swap of from/to.
struct TransferRequest {
    AccountId from;
    AccountId to;
    Paise amount;
    std::string narration;
};

enum class Posting { Commit, DryRun };   // instead of `bool dry_run`

// ===========================================================================
// 6. Seams. Small interfaces, injected by reference, owned by the caller.
// ===========================================================================
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;
};

class Ledger {
public:
    virtual ~Ledger() = default;
    virtual std::optional<Paise> balance(AccountId) const = 0;
    // Atomic debit+credit. Returns false if either account is unknown.
    [[nodiscard]] virtual bool post(AccountId debit, AccountId credit, Paise amt) = 0;
};

class Notifier {
public:
    virtual ~Notifier() = default;
    virtual void transfer_done(AccountId from, AccountId to, Paise amt) = 0;
};

class TransferService {
public:
    TransferService(Ledger& l, Notifier& n, const Clock& c) noexcept : ledger_(l), notifier_(n), clock_(c) {}

    TransferResult transfer(const TransferRequest& r, Posting mode = Posting::Commit) {
        if (r.from == r.to) return TransferResult::failure(TransferError::SameAccount);
        if (!(Paise(0) < r.amount)) return TransferResult::failure(TransferError::NonPositiveAmount);
        auto bal = ledger_.balance(r.from);
        if (!bal || !ledger_.balance(r.to)) return TransferResult::failure(TransferError::UnknownAccount);
        if (*bal < r.amount) return TransferResult::failure(TransferError::InsufficientFunds);
        if (mode == Posting::DryRun) return TransferResult::success(0);
        if (!ledger_.post(r.from, r.to, r.amount)) return TransferResult::failure(TransferError::UnknownAccount);
        notifier_.transfer_done(r.from, r.to, r.amount);
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(clock_.now().time_since_epoch());
        return TransferResult::success(static_cast<std::uint64_t>(secs.count()) * 1000 + ++seq_);
    }

private:
    Ledger& ledger_;
    Notifier& notifier_;
    const Clock& clock_;
    std::uint64_t seq_ = 0;
};

// ---- Test doubles --------------------------------------------------------
// FAKE: a real, working, simplified implementation. Tests assert on its
// resulting STATE (balances), not on how it was called.
class InMemoryLedger final : public Ledger {
public:
    void open(AccountId a, Paise initial) { bal_.emplace(a, initial); }
    std::optional<Paise> balance(AccountId a) const override {
        auto it = bal_.find(a);
        if (it == bal_.end()) return std::nullopt;
        return it->second;
    }
    bool post(AccountId d, AccountId c, Paise amt) override {
        auto di = bal_.find(d), ci = bal_.find(c);
        if (di == bal_.end() || ci == bal_.end()) return false;
        di->second = di->second - amt;
        ci->second = ci->second + amt;
        return true;
    }
private:
    std::map<AccountId, Paise> bal_;
};

// MOCK: records INTERACTIONS so the test can verify them. Use it for things
// whose only observable effect is the call itself (an SMS, an audit event).
class MockNotifier final : public Notifier {
public:
    struct Call { AccountId from, to; Paise amt; };
    void transfer_done(AccountId f, AccountId t, Paise a) override { calls.push_back({f, t, a}); }
    std::vector<Call> calls;
};

class FixedClock final : public Clock {
public:
    explicit FixedClock(std::chrono::system_clock::time_point t) : t_(t) {}
    std::chrono::system_clock::time_point now() const override { return t_; }
private:
    std::chrono::system_clock::time_point t_;
};

// ===========================================================================
// 7. Versioning with inline namespaces. Code compiled against v1 refers to
// api::v1::fee and keeps linking; new code calling api::fee gets v2.
// ===========================================================================
namespace api {
namespace v1 { inline Paise fee(Paise amt) { return Paise(amt.get() / 100); } }            // 1 percent
inline namespace v2 {
inline Paise fee(Paise amt) { return Paise(std::min<std::int64_t>(amt.get() / 100, 2500)); }  // capped
}
}  // namespace api

// ===========================================================================
// 8. Strangler router: route by product, shadow-compare in "Shadow" mode.
// ===========================================================================
enum class Route { Legacy, New, Shadow };

class StranglerRouter {
public:
    using Handler = std::function<std::string(const std::string& product, const std::string& payload)>;
    StranglerRouter(Handler legacy, Handler modern) : legacy_(std::move(legacy)), modern_(std::move(modern)) {}

    void set_route(const std::string& product, Route r) { routes_[product] = r; }

    std::string handle(const std::string& product, const std::string& payload) {
        auto it = routes_.find(product);
        const Route r = it == routes_.end() ? Route::Legacy : it->second;   // default: old path
        if (r == Route::Legacy) return legacy_(product, payload);
        if (r == Route::New) return modern_(product, payload);
        // Shadow: the legacy answer is the one returned; the new one is only
        // compared. In production, run the shadow call asynchronously and only
        // for read-only requests (or against a scratch schema).
        std::string old_answer = legacy_(product, payload);
        if (modern_(product, payload) != old_answer) ++mismatches_;
        return old_answer;
    }
    std::uint64_t mismatches() const noexcept { return mismatches_; }

private:
    Handler legacy_, modern_;
    std::map<std::string, Route> routes_;
    std::uint64_t mismatches_ = 0;
};

int main() {
    // Strong types and parse-don't-validate.
    assert(Ifsc::parse("HDFC0001234") && Ifsc::parse("HDFC0001234")->str() == "HDFC0001234");
    assert(!Ifsc::parse("HDFC1001234") && !Ifsc::parse("hdfc0001234") && !Ifsc::parse("HDFC000"));
    assert((Paise(150) + Paise(50)) * 2 == Paise(400));

    // Service with a fake ledger and a mock notifier.
    const AccountId a(1), b(2), ghost(99);
    InMemoryLedger ledger;
    ledger.open(a, Paise(10'000));
    ledger.open(b, Paise(0));
    MockNotifier notes;
    FixedClock clock(std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000)));
    TransferService svc(ledger, notes, clock);

    TransferResult r = svc.transfer({a, b, Paise(2'500), "rent"});
    assert(r.ok() && r.reference() == 1'700'000'000ULL * 1000 + 1);
    assert(*ledger.balance(a) == Paise(7'500) && *ledger.balance(b) == Paise(2'500));   // fake: assert state
    assert(notes.calls.size() == 1 && notes.calls[0].amt == Paise(2'500));             // mock: assert call

    assert(svc.transfer({a, a, Paise(1), ""}).error() == TransferError::SameAccount);
    assert(svc.transfer({a, b, Paise(0), ""}).error() == TransferError::NonPositiveAmount);
    assert(svc.transfer({a, b, Paise(1'000'000), ""}).error() == TransferError::InsufficientFunds);
    assert(svc.transfer({a, ghost, Paise(1), ""}).error() == TransferError::UnknownAccount);
    assert(svc.transfer({a, b, Paise(100), ""}, Posting::DryRun).ok());
    assert(notes.calls.size() == 1 && *ledger.balance(a) == Paise(7'500));             // dry run: no effects

    // Versioning.
    assert(api::v1::fee(Paise(1'000'000)) == Paise(10'000));
    assert(api::fee(Paise(1'000'000)) == Paise(2'500));                                 // v2 by default

    // Strangler: product PL moved to the new path, HL being shadow-tested.
    auto legacy = [](const std::string& p, const std::string& x) { return p + ":" + x; };
    auto modern = [](const std::string& p, const std::string& x) { return p == "HL" && x == "edge" ? std::string("DIFF") : p + ":" + x; };
    StranglerRouter router(legacy, modern);
    router.set_route("PL", Route::New);
    router.set_route("HL", Route::Shadow);
    assert(router.handle("PL", "1") == "PL:1");
    assert(router.handle("HL", "normal") == "HL:normal");
    assert(router.handle("HL", "edge") == "HL:edge");      // legacy answer still served
    assert(router.mismatches() == 1);                       // and the difference is counted
    assert(router.handle("GL", "z") == "GL:z");             // unrouted product: legacy

    std::puts("m10_api_quality: all checks passed");
    return 0;
}
