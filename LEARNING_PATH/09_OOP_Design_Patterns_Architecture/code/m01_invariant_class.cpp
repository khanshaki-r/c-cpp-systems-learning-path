// m01_invariant_class.cpp
// Encapsulation is about INVARIANTS: facts that must always be true about an object.
// In C, an invariant is a comment and a prayer. In C++, the class boundary enforces it:
// the only code that can break the invariant is the code inside the class.
//
// Example: a bank account whose balance must never go negative and whose
// transaction log must always sum to the balance.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m01b m01_invariant_class.cpp

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The C way: everything is public because C has no other option.
// ---------------------------------------------------------------------------
struct c_account {
    long long balance_paise;        // "must be >= 0" -- says a comment somewhere
    long long* log;                 // each entry is a signed delta
    int log_count;
    int log_cap;
};

// Nothing stops a caller from doing: acct.balance_paise = -500;
// or appending to log without updating balance. Every function that touches
// c_account must re-check the invariant, or trust everyone else did.

// ---------------------------------------------------------------------------
// The C++ way: private state, public operations that preserve the invariant.
// ---------------------------------------------------------------------------
class Account {
public:
    // Strong type for money so you cannot accidentally pass a count of items.
    struct Paise {
        std::int64_t value;
    };

    explicit Account(std::string id) : id_(std::move(id)) {}

    // Every public function is a "transaction" on the invariant: it is true
    // on entry, may be temporarily false inside, and is true again on exit.
    void deposit(Paise amount) {
        if (amount.value <= 0) throw std::invalid_argument("deposit must be positive");
        log_.push_back(amount.value);
        balance_ += amount.value;
    }

    // Returns false instead of throwing: insufficient funds is an expected
    // business outcome, not a programming error. (Module 09 covers error policy.)
    bool withdraw(Paise amount) {
        if (amount.value <= 0) throw std::invalid_argument("withdraw must be positive");
        if (amount.value > balance_) return false;   // invariant would break -> refuse
        log_.push_back(-amount.value);
        balance_ -= amount.value;
        return true;
    }

    // const member functions: callers holding a const Account& can read but not mutate.
    std::int64_t balance() const { return balance_; }
    const std::string& id() const { return id_; }
    const std::vector<std::int64_t>& history() const { return log_; }

    // A debugging helper that checks the invariant explicitly. In production you
    // would call this in unit tests, not on every operation.
    bool invariant_holds() const {
        std::int64_t sum = 0;
        for (auto d : log_) sum += d;
        return sum == balance_ && balance_ >= 0;
    }

private:
    std::string id_;
    std::int64_t balance_ = 0;              // in-class initializer: never uninitialized
    std::vector<std::int64_t> log_;         // vector owns its memory; no log_cap bookkeeping
};

// ---------------------------------------------------------------------------
// An INTERFACE separates "what" from "how". Code that posts transactions does
// not care whether the account lives in memory, in Berkeley DB or in Postgres.
// ---------------------------------------------------------------------------
class AccountRepository {
public:
    virtual ~AccountRepository() = default;
    virtual Account* find(const std::string& id) = 0;     // observer pointer: repo keeps ownership
    virtual void save(const Account& a) = 0;
};

class InMemoryAccountRepository final : public AccountRepository {
public:
    Account* find(const std::string& id) override {
        for (auto& a : accounts_)
            if (a.id() == id) return &a;
        return nullptr;
    }
    void save(const Account& a) override {
        for (auto& existing : accounts_)
            if (existing.id() == a.id()) { existing = a; return; }
        accounts_.push_back(a);
    }

private:
    std::vector<Account> accounts_;  // vector of VALUES, not pointers: simplest ownership
};

// Business logic depends on the interface only. Testable with the in-memory repo,
// deployable with a database-backed one. This is the "D" of SOLID (Module 02).
bool transfer(AccountRepository& repo, const std::string& from, const std::string& to,
              Account::Paise amount) {
    Account* src = repo.find(from);
    Account* dst = repo.find(to);
    if (!src || !dst) return false;
    if (!src->withdraw(amount)) return false;
    dst->deposit(amount);
    repo.save(*src);
    repo.save(*dst);
    return true;
}

int main() {
    InMemoryAccountRepository repo;
    Account a("acct:1001");
    Account b("acct:1002");
    a.deposit(Account::Paise{50000});
    repo.save(a);
    repo.save(b);

    bool ok1 = transfer(repo, "acct:1001", "acct:1002", Account::Paise{20000});
    bool ok2 = transfer(repo, "acct:1001", "acct:1002", Account::Paise{40000});  // insufficient

    const Account* ra = repo.find("acct:1001");
    const Account* rb = repo.find("acct:1002");
    std::printf("transfer 1: %s\n", ok1 ? "ok" : "refused");
    std::printf("transfer 2: %s\n", ok2 ? "ok" : "refused");
    std::printf("1001 balance = %lld, invariant %s\n", static_cast<long long>(ra->balance()),
                ra->invariant_holds() ? "holds" : "BROKEN");
    std::printf("1002 balance = %lld, invariant %s\n", static_cast<long long>(rb->balance()),
                rb->invariant_holds() ? "holds" : "BROKEN");

    // Try to compile this and the compiler stops you. That is the whole point.
    // ra->balance_ = -1;

    // Silence the "unused struct" question: c_account exists only to show the contrast.
    c_account legacy{0, nullptr, 0, 0};
    std::printf("legacy balance field is public: %lld\n", legacy.balance_paise);
    return 0;
}
