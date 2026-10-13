// m06_visitor_variant.cpp
// VISITOR: add operations to a family of types without editing the types.
//   1. Classic double-dispatch visitor (accept/visit) over a class hierarchy
//   2. std::variant + std::visit: the modern form. No accept(), no base class,
//      compile-time exhaustiveness: forget a case and it does not compile.
//
// Scenario: wire messages of several kinds; operations: serialise, estimate
// cost, audit-describe. The set of message TYPES is closed and stable; the set
// of OPERATIONS grows. That is exactly when visitor beats virtual functions
// (where adding an operation means editing every type).
//
// C equivalent: a tagged union + a switch per operation. std::variant IS a
// type-safe tagged union; std::visit IS the switch, checked by the compiler.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06f m06_visitor_variant.cpp

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Classic visitor. Every element type needs accept(); every new element
//    type means editing the Visitor interface AND every visitor.
// ---------------------------------------------------------------------------
namespace classic {

class Deposit;
class Withdrawal;
class Transfer;

class Visitor {
public:
    virtual ~Visitor() = default;
    virtual void visit(const Deposit&) = 0;
    virtual void visit(const Withdrawal&) = 0;
    virtual void visit(const Transfer&) = 0;
};

class Message {
public:
    virtual ~Message() = default;
    virtual void accept(Visitor& v) const = 0;   // first dispatch: on message type
};

class Deposit final : public Message {
public:
    std::string account; std::int64_t amount;
    Deposit(std::string a, std::int64_t m) : account(std::move(a)), amount(m) {}
    void accept(Visitor& v) const override { v.visit(*this); }   // second dispatch: overload on *this
};
class Withdrawal final : public Message {
public:
    std::string account; std::int64_t amount;
    Withdrawal(std::string a, std::int64_t m) : account(std::move(a)), amount(m) {}
    void accept(Visitor& v) const override { v.visit(*this); }
};
class Transfer final : public Message {
public:
    std::string from, to; std::int64_t amount;
    Transfer(std::string f, std::string t, std::int64_t m) : from(std::move(f)), to(std::move(t)), amount(m) {}
    void accept(Visitor& v) const override { v.visit(*this); }
};

class Serialiser final : public Visitor {
public:
    std::string out;
    void visit(const Deposit& d) override { out += "D|" + d.account + "|" + std::to_string(d.amount) + "\n"; }
    void visit(const Withdrawal& w) override { out += "W|" + w.account + "|" + std::to_string(w.amount) + "\n"; }
    void visit(const Transfer& t) override { out += "T|" + t.from + "|" + t.to + "|" + std::to_string(t.amount) + "\n"; }
};

}  // namespace classic

// ---------------------------------------------------------------------------
// 2. std::variant form. Message types are plain structs (no base class, no
//    vptr, no heap). Operations are visitors: a set of overloads. A missing
//    overload is a COMPILE error, which the classic form also gives you, but
//    without accept() boilerplate and without heap allocation.
// ---------------------------------------------------------------------------
namespace modern {

struct Deposit    { std::string account; std::int64_t amount; };
struct Withdrawal { std::string account; std::int64_t amount; };
struct Transfer   { std::string from, to; std::int64_t amount; };

using Message = std::variant<Deposit, Withdrawal, Transfer>;

// The "overloaded" helper: turns several lambdas into one object with several
// operator() overloads. Standard C++17 idiom; the deduction guide lets you
// write overloaded{...} without naming the lambda types.
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

std::string serialise(const Message& m) {
    return std::visit(overloaded{
        [](const Deposit& d)    { return "D|" + d.account + "|" + std::to_string(d.amount); },
        [](const Withdrawal& w) { return "W|" + w.account + "|" + std::to_string(w.amount); },
        [](const Transfer& t)   { return "T|" + t.from + "|" + t.to + "|" + std::to_string(t.amount); },
    }, m);
}

// A second operation, added WITHOUT touching the message types.
std::int64_t fee(const Message& m) {
    return std::visit(overloaded{
        [](const Deposit&)      { return std::int64_t{0}; },
        [](const Withdrawal& w) { return w.amount > 1000000 ? std::int64_t{2500} : std::int64_t{0}; },
        [](const Transfer& t)   { return t.amount / 1000; },
    }, m);
}

// A visitor can also be a struct with state, when it needs to accumulate.
struct Totals {
    std::int64_t in = 0, out = 0, moved = 0;
    void operator()(const Deposit& d)    { in += d.amount; }
    void operator()(const Withdrawal& w) { out += w.amount; }
    void operator()(const Transfer& t)   { moved += t.amount; }
};

// Generic lambda for "same code for every alternative": auto parameter.
std::int64_t amount_of(const Message& m) {
    return std::visit([](const auto& x) { return x.amount; }, m);   // every alternative has .amount
}

}  // namespace modern

int main() {
    // Classic
    std::vector<std::unique_ptr<classic::Message>> cmsgs;
    cmsgs.push_back(std::make_unique<classic::Deposit>("acct:1", 500));
    cmsgs.push_back(std::make_unique<classic::Transfer>("acct:1", "acct:2", 200));
    classic::Serialiser ser;
    for (auto& m : cmsgs) m->accept(ser);
    std::printf("classic:\n%s", ser.out.c_str());

    // Modern
    std::vector<modern::Message> msgs = {
        modern::Deposit{"acct:1", 500},
        modern::Withdrawal{"acct:1", 2000000},
        modern::Transfer{"acct:1", "acct:2", 200000},
    };
    modern::Totals totals;
    std::printf("modern:\n");
    for (const auto& m : msgs) {
        std::printf("  %-28s fee=%lld amount=%lld\n", modern::serialise(m).c_str(),
                    static_cast<long long>(modern::fee(m)), static_cast<long long>(modern::amount_of(m)));
        std::visit(totals, m);   // stateful visitor, passed by reference (visit takes it by forwarding ref)
    }
    std::printf("  totals in=%lld out=%lld moved=%lld\n", static_cast<long long>(totals.in),
                static_cast<long long>(totals.out), static_cast<long long>(totals.moved));

    std::printf("sizeof(modern::Message) = %zu, holds index %zu for a Transfer\n", sizeof(modern::Message),
                msgs[2].index());
    return 0;
}
