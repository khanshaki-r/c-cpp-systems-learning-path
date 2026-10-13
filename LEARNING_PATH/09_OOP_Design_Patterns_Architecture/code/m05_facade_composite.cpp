// m05_facade_composite.cpp
// FACADE: one simple entry point in front of several subsystems, so callers do
// not need to know the choreography between them.
// COMPOSITE: a tree where leaves and groups share one interface, so callers
// treat "one rule" and "a group of rules" identically.
//
// Scenario: a loan-eligibility check. Rules form a composite tree; the facade
// hides the sequence "load applicant, run rules, record decision, notify".
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m05b m05_facade_composite.cpp

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Domain value
// ---------------------------------------------------------------------------
struct Applicant {
    std::string id;
    int age;
    long monthly_income;
    long requested_amount;
    int credit_score;
    bool existing_default;
};

// ---------------------------------------------------------------------------
// COMPOSITE: Rule is the component. Leaf rules check one thing. AllOf and AnyOf
// are composites holding children. The caller sees only Rule.
//
// C equivalent: a struct rule { int (*eval)(const struct rule*, const applicant*);
// struct rule** children; int n; } with eval_all/eval_any functions. The same
// shape, with the tree walk written by hand.
// ---------------------------------------------------------------------------
class Rule {
public:
    virtual ~Rule() = default;
    virtual bool passes(const Applicant& a) const = 0;
    virtual std::string describe(int indent = 0) const = 0;

protected:
    static std::string pad(int n) { return std::string(static_cast<std::size_t>(n) * 2, ' '); }
};

class MinAge final : public Rule {
public:
    explicit MinAge(int years) : years_(years) {}
    bool passes(const Applicant& a) const override { return a.age >= years_; }
    std::string describe(int indent) const override { return pad(indent) + "age >= " + std::to_string(years_) + "\n"; }

private:
    int years_;
};

class MinCreditScore final : public Rule {
public:
    explicit MinCreditScore(int s) : score_(s) {}
    bool passes(const Applicant& a) const override { return a.credit_score >= score_; }
    std::string describe(int indent) const override { return pad(indent) + "score >= " + std::to_string(score_) + "\n"; }

private:
    int score_;
};

class MaxDebtToIncome final : public Rule {
public:
    // amount / (income * months) must be <= ratio. Kept integer to avoid float noise.
    MaxDebtToIncome(int months, int percent) : months_(months), percent_(percent) {}
    bool passes(const Applicant& a) const override {
        long capacity = a.monthly_income * months_ * percent_ / 100;
        return a.requested_amount <= capacity;
    }
    std::string describe(int indent) const override {
        return pad(indent) + "amount <= income*" + std::to_string(months_) + "*" + std::to_string(percent_) + "%\n";
    }

private:
    int months_;
    int percent_;
};

class NoExistingDefault final : public Rule {
public:
    bool passes(const Applicant& a) const override { return !a.existing_default; }
    std::string describe(int indent) const override { return pad(indent) + "no existing default\n"; }
};

// Composite nodes. They OWN their children (unique_ptr tree, Module 03).
class AllOf final : public Rule {
public:
    AllOf& add(std::unique_ptr<Rule> r) { children_.push_back(std::move(r)); return *this; }
    bool passes(const Applicant& a) const override {
        for (auto& c : children_)
            if (!c->passes(a)) return false;
        return true;
    }
    std::string describe(int indent) const override {
        std::string s = pad(indent) + "ALL OF\n";
        for (auto& c : children_) s += c->describe(indent + 1);
        return s;
    }

private:
    std::vector<std::unique_ptr<Rule>> children_;
};

class AnyOf final : public Rule {
public:
    AnyOf& add(std::unique_ptr<Rule> r) { children_.push_back(std::move(r)); return *this; }
    bool passes(const Applicant& a) const override {
        for (auto& c : children_)
            if (c->passes(a)) return true;
        return false;
    }
    std::string describe(int indent) const override {
        std::string s = pad(indent) + "ANY OF\n";
        for (auto& c : children_) s += c->describe(indent + 1);
        return s;
    }

private:
    std::vector<std::unique_ptr<Rule>> children_;
};

// ---------------------------------------------------------------------------
// Subsystems the facade coordinates. Each is simple; the COMBINATION is what
// callers should not have to learn.
// ---------------------------------------------------------------------------
class ApplicantRepository {
public:
    Applicant load(const std::string& id) const {
        // Fake lookup.
        if (id == "A-1") return Applicant{id, 34, 80000, 1500000, 720, false};
        if (id == "A-2") return Applicant{id, 19, 15000, 900000, 610, false};
        return Applicant{id, 45, 120000, 500000, 790, true};
    }
};

class DecisionLog {
public:
    void record(const std::string& id, bool approved, const std::string& why) {
        entries_.push_back(id + ": " + (approved ? "APPROVED" : "DECLINED") + " (" + why + ")");
    }
    const std::vector<std::string>& entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

class Notifier {
public:
    void notify(const std::string& id, bool approved) {
        std::printf("  [notify] %s -> %s\n", id.c_str(), approved ? "approved" : "declined");
    }
};

// ---------------------------------------------------------------------------
// FACADE. One method. Inside: the choreography. Callers (an RPC handler, a
// batch job, a test) never touch the subsystems directly.
//
// C equivalent: a loan_decide() function in loan.c that calls into repo.c,
// rules.c, log.c, notify.c. Facades are the most "C-like" pattern: they are
// just a good module boundary.
// ---------------------------------------------------------------------------
class LoanEligibilityFacade {
public:
    LoanEligibilityFacade(ApplicantRepository& repo, const Rule& policy, DecisionLog& log, Notifier& notifier)
        : repo_(repo), policy_(policy), log_(log), notifier_(notifier) {}

    bool decide(const std::string& applicant_id) {
        Applicant a = repo_.load(applicant_id);
        bool ok = policy_.passes(a);
        log_.record(applicant_id, ok, ok ? "policy satisfied" : "policy failed");
        notifier_.notify(applicant_id, ok);
        return ok;
    }

private:
    ApplicantRepository& repo_;   // all observed: the composition root owns them
    const Rule& policy_;
    DecisionLog& log_;
    Notifier& notifier_;
};

int main() {
    // Build the policy tree. Reads like the business rule it encodes.
    auto strong_profile = std::make_unique<AllOf>();
    strong_profile->add(std::make_unique<MinCreditScore>(750)).add(std::make_unique<MaxDebtToIncome>(60, 50));

    auto standard_profile = std::make_unique<AllOf>();
    standard_profile->add(std::make_unique<MinCreditScore>(650))
        .add(std::make_unique<MaxDebtToIncome>(36, 40))
        .add(std::make_unique<NoExistingDefault>());

    auto either = std::make_unique<AnyOf>();
    either->add(std::move(strong_profile)).add(std::move(standard_profile));

    AllOf policy;
    policy.add(std::make_unique<MinAge>(21)).add(std::move(either));

    std::printf("policy:\n%s", policy.describe(1).c_str());

    // Wire the facade.
    ApplicantRepository repo;
    DecisionLog log;
    Notifier notifier;
    LoanEligibilityFacade loans(repo, policy, log, notifier);

    std::printf("decisions:\n");
    for (const char* id : {"A-1", "A-2", "A-3"}) loans.decide(id);

    std::printf("log:\n");
    for (auto& e : log.entries()) std::printf("  %s\n", e.c_str());
    return 0;
}
