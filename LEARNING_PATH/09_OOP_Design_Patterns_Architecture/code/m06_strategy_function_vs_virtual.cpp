// m06_strategy_function_vs_virtual.cpp
// STRATEGY: make an algorithm swappable at runtime. Three spellings:
//   1. Virtual interface (classic GoF)
//   2. std::function (modern, when the strategy is one operation)
//   3. Template parameter (compile-time, when the choice is known at build time)
// plus the C spelling: a function pointer + void* context.
//
// Scenario: fee calculation for a transfer, which varies by customer tier and
// is configured per deployment.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06a m06_strategy_function_vs_virtual.cpp

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

using Paise = std::int64_t;

struct Transfer {
    Paise amount;
    bool cross_bank;
    std::string tier;   // "basic", "premium"
};

// ---------------------------------------------------------------------------
// 0. The C spelling, for reference. Context is a void*; the compiler cannot
//    check that ctx matches fn. Still the right call in C.
// ---------------------------------------------------------------------------
namespace c_style {
struct fee_strategy {
    Paise (*compute)(const void* ctx, const Transfer* t);
    const void* ctx;
};
struct flat_ctx { Paise fee; };
static Paise flat_compute(const void* ctx, const Transfer*) {
    return static_cast<const flat_ctx*>(ctx)->fee;
}
}  // namespace c_style

// ---------------------------------------------------------------------------
// 1. Virtual interface. Best when a strategy has SEVERAL operations, needs
//    state with identity, or must be stored heterogeneously by name.
// ---------------------------------------------------------------------------
class FeePolicy {
public:
    virtual ~FeePolicy() = default;
    virtual Paise fee(const Transfer& t) const = 0;
    virtual std::string name() const = 0;   // a second operation: the reason to prefer a class
};

class FlatFee final : public FeePolicy {
public:
    explicit FlatFee(Paise f) : fee_(f) {}
    Paise fee(const Transfer&) const override { return fee_; }
    std::string name() const override { return "flat(" + std::to_string(fee_) + ")"; }

private:
    Paise fee_;
};

class PercentFee final : public FeePolicy {
public:
    PercentFee(int basis_points, Paise cap) : bps_(basis_points), cap_(cap) {}
    Paise fee(const Transfer& t) const override {
        Paise f = t.amount * bps_ / 10000;
        return f > cap_ ? cap_ : f;
    }
    std::string name() const override { return "percent(" + std::to_string(bps_) + "bps, cap " + std::to_string(cap_) + ")"; }

private:
    int bps_;
    Paise cap_;
};

class TransferServiceV1 {
public:
    explicit TransferServiceV1(std::unique_ptr<FeePolicy> policy) : policy_(std::move(policy)) {}
    Paise total(const Transfer& t) const { return t.amount + policy_->fee(t); }
    void set_policy(std::unique_ptr<FeePolicy> p) { policy_ = std::move(p); }   // swap at runtime
    std::string policy_name() const { return policy_->name(); }

private:
    std::unique_ptr<FeePolicy> policy_;
};

// ---------------------------------------------------------------------------
// 2. std::function. Best when the strategy is ONE operation. No class per
//    strategy; lambdas capture their parameters. The cost: std::function is
//    ~32 bytes, may heap-allocate for large captures, and is an indirect call
//    (same class of cost as virtual).
// ---------------------------------------------------------------------------
using FeeFn = std::function<Paise(const Transfer&)>;

class TransferServiceV2 {
public:
    explicit TransferServiceV2(FeeFn fee) : fee_(std::move(fee)) {}
    Paise total(const Transfer& t) const { return t.amount + fee_(t); }
    void set_fee(FeeFn f) { fee_ = std::move(f); }

private:
    FeeFn fee_;
};

// Strategies as plain functions and lambdas:
static Paise free_for_premium(const Transfer& t) { return t.tier == "premium" ? 0 : 500; }

FeeFn make_percent_fee(int bps, Paise cap) {
    return [bps, cap](const Transfer& t) {    // captures ARE the strategy's state
        Paise f = t.amount * bps / 10000;
        return f > cap ? cap : f;
    };
}

// Composition is trivial with functions: a strategy built from strategies.
FeeFn max_of(FeeFn a, FeeFn b) {
    return [a = std::move(a), b = std::move(b)](const Transfer& t) {
        Paise x = a(t), y = b(t);
        return x > y ? x : y;
    };
}

// ---------------------------------------------------------------------------
// 3. Template parameter. Best when the strategy is fixed at compile time and
//    the call is hot: the compiler inlines it completely. The cost: one
//    TransferServiceV3<X> type per strategy; cannot swap at runtime.
// ---------------------------------------------------------------------------
template <class Fee>
class TransferServiceV3 {
public:
    explicit TransferServiceV3(Fee fee) : fee_(std::move(fee)) {}
    Paise total(const Transfer& t) const { return t.amount + fee_(t); }

private:
    Fee fee_;   // stored by value: a lambda or a small functor, no indirection
};

struct FlatFeeFunctor {
    Paise fee;
    Paise operator()(const Transfer&) const { return fee; }
};

int main() {
    Transfer t1{100000, true, "basic"};
    Transfer t2{5000000, true, "premium"};

    // C
    c_style::flat_ctx fc{250};
    c_style::fee_strategy cs{c_style::flat_compute, &fc};
    std::printf("[c]        fee = %lld\n", static_cast<long long>(cs.compute(cs.ctx, &t1)));

    // 1. virtual
    TransferServiceV1 v1(std::make_unique<FlatFee>(250));
    std::printf("[virtual]  %s total = %lld\n", v1.policy_name().c_str(), static_cast<long long>(v1.total(t1)));
    v1.set_policy(std::make_unique<PercentFee>(25, 10000));   // 0.25%, cap 100 rupees
    std::printf("[virtual]  %s total = %lld\n", v1.policy_name().c_str(), static_cast<long long>(v1.total(t2)));

    // 2. std::function
    TransferServiceV2 v2(free_for_premium);
    std::printf("[function] premium rule: basic=%lld premium=%lld\n", static_cast<long long>(v2.total(t1)),
                static_cast<long long>(v2.total(t2)));
    v2.set_fee(max_of(make_percent_fee(25, 10000), [](const Transfer& t) { return t.cross_bank ? 300 : 0; }));
    std::printf("[function] max(percent, crossbank): t1=%lld t2=%lld\n", static_cast<long long>(v2.total(t1)),
                static_cast<long long>(v2.total(t2)));

    // 3. template
    TransferServiceV3<FlatFeeFunctor> v3a(FlatFeeFunctor{250});
    auto pct = [](const Transfer& t) { return t.amount * 25 / 10000; };
    TransferServiceV3<decltype(pct)> v3b(pct);
    std::printf("[template] flat=%lld pct=%lld (sizeof v3a=%zu, v3b=%zu)\n", static_cast<long long>(v3a.total(t1)),
                static_cast<long long>(v3b.total(t1)), sizeof v3a, sizeof v3b);
    std::printf("sizeof(std::function) = %zu, sizeof(unique_ptr<FeePolicy>) = %zu\n", sizeof(FeeFn),
                sizeof(std::unique_ptr<FeePolicy>));
    return 0;
}
