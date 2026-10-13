// m11_workflow_engine.cpp
// LLD WALKTHROUGH: a loan-application workflow engine (state machine + rules).
//
// Requirements (as clarified):
//   - A loan application moves through states:
//       Draft -> Submitted -> UnderReview -> Approved -> Disbursed
//                                       \-> Rejected
//       Draft/Submitted/UnderReview/Approved -> Cancelled
//   - Transitions are triggered by events (Submit, StartReview, Approve,
//     Reject, Disburse, Cancel) performed by an actor with a role.
//   - Each transition may have GUARD RULES. All rules run; if any fail, the
//     transition is refused and EVERY failure reason is returned (an officer
//     wants the full list, not the first problem).
//   - Each transition may have ACTIONS (side effects after it succeeds:
//     notify, compute sanction letter, post to core banking).
//   - Every attempt, successful or not, goes into an audit trail.
//   - The transition table is DATA, built at start-up (it could be loaded
//     from a config table), not a switch statement scattered over the code.
//
// This is exactly what lending platforms do in stored procedures or in the
// application server: a status column, a transition table, validation rules.
//
//   +----------------+  uses   +----------------------+      +--------------+
//   | WorkflowEngine |-------->| TransitionTable      |<>--->| Transition   |
//   |  fire(app, ev) |         | (State,Event)->Trans.|      | to, roles,   |
//   +----------------+         +----------------------+      | rules, acts  |
//          | mutates                                         +--------------+
//          v                                                    |  *   |  *
//   +-------------+   1   *  +------------+         +------+    v      v
//   | Application |<>------->| AuditEntry |         | Rule | (name, fn) Action
//   +-------------+          +------------+         +------+
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11f m11_workflow_engine.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

enum class State { Draft, Submitted, UnderReview, Approved, Rejected, Disbursed, Cancelled };
enum class Event { Submit, StartReview, Approve, Reject, Disburse, Cancel };
enum class Role  { Applicant, Officer, Approver, Operations };

const char* name(State s) {
    switch (s) {
        case State::Draft: return "Draft";           case State::Submitted: return "Submitted";
        case State::UnderReview: return "UnderReview"; case State::Approved: return "Approved";
        case State::Rejected: return "Rejected";     case State::Disbursed: return "Disbursed";
        case State::Cancelled: return "Cancelled";
    }
    return "?";
}

struct Actor {
    std::string user;
    Role role;
};

struct AuditEntry {
    State from, to;
    Event event;
    std::string user;
    bool accepted;
    std::vector<std::string> reasons;
};

// The domain object. Money in paise (integer): never double for money.
struct Application {
    std::uint64_t id;
    std::string product;            // "HOME", "PERSONAL"
    std::int64_t amount_paise;
    std::int64_t monthly_income_paise;
    std::int64_t existing_emi_paise;
    std::int64_t proposed_emi_paise;
    int applicant_age;
    bool kyc_verified;
    std::string submitted_by;       // for maker-checker
    State state = State::Draft;
    std::vector<AuditEntry> audit;
};

// A rule: a name and a predicate that returns an empty optional on pass, or
// a human-readable reason on failure.
struct Rule {
    std::string name;
    std::function<std::optional<std::string>(const Application&, const Actor&)> check;
};

using Action = std::function<void(Application&, const Actor&)>;

struct Transition {
    State to;
    std::set<Role> allowed_roles;
    std::vector<Rule> rules;
    std::vector<Action> actions;
};

struct Outcome {
    bool accepted;
    State state;
    std::vector<std::string> reasons;
};

// ---------------------------------------------------------------------------
// The engine knows NOTHING about loans: it is a generic table-driven state
// machine with guards and actions. The loan knowledge is all in the table
// and the rules below. That separation is what lets a second product (gold
// loan, credit card) reuse the engine with a different table.
// ---------------------------------------------------------------------------
class WorkflowEngine {
public:
    void add(State from, Event ev, Transition t) {
        const bool inserted = table_.emplace(std::make_pair(from, ev), std::move(t)).second;
        assert(inserted && "duplicate transition");
        (void)inserted;
    }

    Outcome fire(Application& app, Event ev, const Actor& actor) const {
        const State from = app.state;
        std::vector<std::string> reasons;
        auto it = table_.find({from, ev});
        if (it == table_.end()) {
            reasons.push_back(std::string("event not allowed in state ") + name(from));
        } else {
            const Transition& t = it->second;
            if (!t.allowed_roles.count(actor.role)) reasons.push_back("role not permitted");
            for (const Rule& r : t.rules)                       // run ALL rules
                if (auto why = r.check(app, actor)) reasons.push_back(r.name + ": " + *why);
        }
        const bool ok = reasons.empty();
        const State to = ok ? it->second.to : from;
        app.audit.push_back(AuditEntry{from, to, ev, actor.user, ok, reasons});
        if (!ok) return Outcome{false, from, std::move(reasons)};

        app.state = to;
        // Actions run AFTER the state change. In a real server, state change +
        // audit row + an outbox row for each action commit in ONE database
        // transaction; a worker then performs the side effects (outbox pattern).
        for (const Action& a : it->second.actions) a(app, actor);
        return Outcome{true, to, {}};
    }

    // Useful for UI ("which buttons to show") and for tests of the table itself.
    std::vector<Event> available_events(State s) const {
        std::vector<Event> out;
        for (const auto& [key, t] : table_) if (key.first == s) out.push_back(key.second);
        return out;
    }

private:
    std::map<std::pair<State, Event>, Transition> table_;
};

// ---------------------------------------------------------------------------
// Rules. Each is small, named and independently testable. Parameters (limits)
// are captured, so they can come from a config table per product.
// ---------------------------------------------------------------------------
Rule kyc_complete() {
    return {"KYC", [](const Application& a, const Actor&) -> std::optional<std::string> {
        if (!a.kyc_verified) return "KYC not verified";
        return std::nullopt;
    }};
}

Rule age_between(int lo, int hi) {
    return {"AGE", [=](const Application& a, const Actor&) -> std::optional<std::string> {
        if (a.applicant_age < lo || a.applicant_age > hi)
            return "age " + std::to_string(a.applicant_age) + " outside " + std::to_string(lo) + "-" + std::to_string(hi);
        return std::nullopt;
    }};
}

Rule max_amount(std::map<std::string, std::int64_t> limit_by_product) {
    return {"AMOUNT", [lim = std::move(limit_by_product)](const Application& a, const Actor&)
                          -> std::optional<std::string> {
        auto it = lim.find(a.product);
        if (it == lim.end()) return "unknown product " + a.product;
        if (a.amount_paise > it->second) return "amount above product limit";
        return std::nullopt;
    }};
}

// FOIR: fixed obligations to income ratio, (existing EMIs + new EMI) / income.
// Integer arithmetic in basis points avoids floating-point surprises.
Rule foir_at_most(int max_bps) {
    return {"FOIR", [=](const Application& a, const Actor&) -> std::optional<std::string> {
        if (a.monthly_income_paise <= 0) return "income missing";
        const std::int64_t bps = (a.existing_emi_paise + a.proposed_emi_paise) * 10000 / a.monthly_income_paise;
        if (bps > max_bps) return "FOIR " + std::to_string(bps) + " bps exceeds " + std::to_string(max_bps);
        return std::nullopt;
    }};
}

// Maker-checker: the person who submitted cannot approve.
Rule maker_checker() {
    return {"MAKER_CHECKER", [](const Application& a, const Actor& actor) -> std::optional<std::string> {
        if (a.submitted_by == actor.user) return "approver is the maker";
        return std::nullopt;
    }};
}

WorkflowEngine build_loan_workflow(std::vector<std::string>& outbox) {
    WorkflowEngine e;
    auto notify = [&outbox](const std::string& what) {
        return Action([&outbox, what](Application& a, const Actor&) {
            outbox.push_back(what + " #" + std::to_string(a.id));
        });
    };
    const std::set<Role> any_staff{Role::Officer, Role::Approver, Role::Operations};

    e.add(State::Draft, Event::Submit,
          {State::Submitted, {Role::Applicant, Role::Officer},
           {kyc_complete(), age_between(21, 65), max_amount({{"HOME", 5'00'00'000'00}, {"PERSONAL", 25'00'000'00}})},
           {[](Application& a, const Actor& who) { a.submitted_by = who.user; }, notify("ack-sms")}});
    e.add(State::Submitted, Event::StartReview, {State::UnderReview, {Role::Officer}, {}, {}});
    e.add(State::UnderReview, Event::Approve,
          {State::Approved, {Role::Approver}, {foir_at_most(5000), maker_checker()}, {notify("sanction-letter")}});
    e.add(State::UnderReview, Event::Reject, {State::Rejected, {Role::Approver}, {}, {notify("reject-letter")}});
    e.add(State::Approved, Event::Disburse, {State::Disbursed, {Role::Operations}, {}, {notify("post-to-cbs")}});
    for (State s : {State::Draft, State::Submitted, State::UnderReview, State::Approved})
        e.add(s, Event::Cancel, {State::Cancelled, any_staff, {}, {notify("cancel-notice")}});
    return e;
}

int main() {
    std::vector<std::string> outbox;
    const WorkflowEngine wf = build_loan_workflow(outbox);
    const Actor applicant{"ravi", Role::Applicant}, officer{"meera", Role::Officer},
                approver{"arjun", Role::Approver}, ops{"ops1", Role::Operations};

    // 1. Happy path.
    Application app{1001, "PERSONAL", 5'00'000'00, 1'00'000'00, 10'000'00, 20'000'00, 30, true, "", State::Draft, {}};
    assert(wf.fire(app, Event::Submit, applicant).accepted);
    assert(app.submitted_by == "ravi");
    assert(wf.fire(app, Event::StartReview, officer).state == State::UnderReview);
    assert(wf.fire(app, Event::Approve, approver).accepted);
    assert(wf.fire(app, Event::Disburse, ops).state == State::Disbursed);
    assert((outbox == std::vector<std::string>{"ack-sms #1001", "sanction-letter #1001", "post-to-cbs #1001"}));
    assert(app.audit.size() == 4);

    // 2. All failing rules are reported together; state unchanged; audited.
    Application bad{1002, "PERSONAL", 30'00'000'00, 50'000'00, 30'000'00, 10'000'00, 19, false, "", State::Draft, {}};
    Outcome o = wf.fire(bad, Event::Submit, applicant);
    assert(!o.accepted && bad.state == State::Draft && o.reasons.size() == 3);   // KYC, AGE, AMOUNT
    assert(bad.audit.size() == 1 && !bad.audit[0].accepted);

    // 3. Wrong role, and an event that is not valid in this state.
    Application app3{1003, "HOME", 50'00'000'00, 2'00'000'00, 0, 60'000'00, 40, true, "", State::Draft, {}};
    assert(!wf.fire(app3, Event::Approve, approver).accepted);                    // not from Draft
    assert(wf.fire(app3, Event::Submit, officer).accepted);                       // officer keys it in
    assert(!wf.fire(app3, Event::StartReview, applicant).accepted);               // role refused
    assert(wf.fire(app3, Event::StartReview, officer).accepted);

    // 4. Maker-checker and FOIR: officer "meera" submitted; she cannot approve
    //    even with the Approver role, and FOIR 3000 bps passes.
    Outcome mc = wf.fire(app3, Event::Approve, Actor{"meera", Role::Approver});
    assert(!mc.accepted && mc.reasons.size() == 1 && mc.reasons[0].rfind("MAKER_CHECKER", 0) == 0);
    assert(wf.fire(app3, Event::Approve, approver).accepted);

    // 5. FOIR failure: (30k + 40k) / 1L = 7000 bps > 5000.
    Application app5{1005, "PERSONAL", 1'00'000'00, 1'00'000'00, 30'000'00, 40'000'00, 35, true, "", State::Draft, {}};
    assert(wf.fire(app5, Event::Submit, applicant).accepted);
    assert(wf.fire(app5, Event::StartReview, officer).accepted);
    Outcome f = wf.fire(app5, Event::Approve, approver);
    assert(!f.accepted && f.reasons[0].rfind("FOIR", 0) == 0);
    assert(wf.fire(app5, Event::Reject, approver).state == State::Rejected);
    assert(wf.available_events(State::Rejected).empty());                        // terminal

    // 6. Cancel from any open state; terminal states refuse everything.
    assert(wf.available_events(State::UnderReview).size() == 3);                 // Approve, Reject, Cancel
    std::printf("final states: %s %s %s %s\n", name(app.state), name(bad.state), name(app3.state), name(app5.state));
    std::puts("m11_workflow_engine: all checks passed");
    return 0;
}
