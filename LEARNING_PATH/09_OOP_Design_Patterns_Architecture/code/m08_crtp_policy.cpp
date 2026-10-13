// m08_crtp_policy.cpp
// CRTP (Curiously Recurring Template Pattern) for STATIC polymorphism, and
// POLICY-BASED DESIGN for compile-time composition of behaviour.
//
// CRTP: class Derived : Base<Derived>. The base calls
// static_cast<Derived&>(*this).step(), which is resolved at compile time and
// inlined. You get "template method" with zero virtual calls, at the cost of
// losing a common base type (no vector<Base*> of mixed derived types).
//
// Policy-based design: class Store<StoragePolicy, LockPolicy, LogPolicy>. Each
// policy is a small class supplying a piece of behaviour; the host combines them.
// It is the decorator stack of Module 05 flattened into one type at compile time.
//
// C equivalent for CRTP: a macro that expands a driver loop with the step
// functions pasted in. Policy: #ifdef-selected implementations. Both are
// "resolve at build time"; templates do it with type checking.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m08b m08_crtp_policy.cpp

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 1. CRTP: template method without virtual.
// ---------------------------------------------------------------------------
template <class Derived>
class RequestProcessor {
public:
    // The skeleton (compare Module 06 NVI). Calls into Derived at compile time.
    std::string process(const std::string& input) {
        auto& self = derived();
        if (!self.validate(input)) return "ERR invalid";
        ++count_;
        return self.run(input);
    }
    std::uint64_t processed() const { return count_; }

    // A default step. Derived may "override" it simply by defining its own validate;
    // name lookup in Derived finds Derived's version first.
    bool validate(const std::string& s) const { return !s.empty(); }

private:
    Derived& derived() { return static_cast<Derived&>(*this); }
    std::uint64_t count_ = 0;
};

class EchoProcessor : public RequestProcessor<EchoProcessor> {
public:
    std::string run(const std::string& s) { return "ECHO " + s; }
};

class UpperProcessor : public RequestProcessor<UpperProcessor> {
public:
    bool validate(const std::string& s) const { return s.size() <= 8; }   // replaces the default
    std::string run(const std::string& s) {
        std::string out = s;
        for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }
};

// CRTP also gives you mixins: reusable functionality injected into any class.
template <class Derived>
struct Comparable {   // derive from this, define operator<, get the rest for free
    friend bool operator>(const Derived& a, const Derived& b) { return b < a; }
    friend bool operator<=(const Derived& a, const Derived& b) { return !(b < a); }
    friend bool operator>=(const Derived& a, const Derived& b) { return !(a < b); }
    friend bool operator==(const Derived& a, const Derived& b) { return !(a < b) && !(b < a); }
    friend bool operator!=(const Derived& a, const Derived& b) { return (a < b) || (b < a); }
};

struct Money : Comparable<Money> {
    std::int64_t paise;
    friend bool operator<(const Money& a, const Money& b) { return a.paise < b.paise; }
};

// ---------------------------------------------------------------------------
// 2. Policy-based design. Each policy is a tiny class with a fixed "duck-typed"
//    interface. The host inherits (empty base optimisation: stateless policies
//    cost zero bytes) or holds them.
// ---------------------------------------------------------------------------

// --- Storage policies: how key/value pairs are kept ---
struct OrderedStorage {
    std::map<std::string, std::string> data;
    static constexpr const char* name = "map";
};
struct HashedStorage {
    std::unordered_map<std::string, std::string> data;
    static constexpr const char* name = "unordered_map";
};

// --- Lock policies: thread safety or not. Both expose the same two names. ---
struct NoLock {
    struct Guard { explicit Guard(NoLock&) {} };   // does nothing; compiles away entirely
    static constexpr const char* name = "no-lock";
};
struct MutexLock {
    std::mutex mu;
    struct Guard {
        std::lock_guard<std::mutex> lk;
        explicit Guard(MutexLock& p) : lk(p.mu) {}
    };
    static constexpr const char* name = "mutex";
};

// --- Log policies ---
struct NoLog {
    static void log(const char*, const std::string&) {}
    static constexpr const char* name = "silent";
};
struct StdoutLog {
    static void log(const char* op, const std::string& k) { std::printf("    [log] %s %s\n", op, k.c_str()); }
    static constexpr const char* name = "stdout";
};

// --- The host ---
template <class Storage, class Lock, class Log>
class PolicyStore : private Storage, private Lock {   // private inheritance: policies are implementation details
public:
    void put(const std::string& k, const std::string& v) {
        typename Lock::Guard g(*this);           // zero-cost with NoLock, lock_guard with MutexLock
        Log::log("put", k);
        Storage::data[k] = v;
    }
    std::optional<std::string> get(const std::string& k) {
        typename Lock::Guard g(*this);
        Log::log("get", k);
        auto it = Storage::data.find(k);
        return it == Storage::data.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    static std::string describe() {
        return std::string(Storage::name) + " + " + Lock::name + " + " + Log::name;
    }
};

// Named configurations read like a spec.
using FastLocalStore   = PolicyStore<HashedStorage, NoLock, NoLog>;
using SharedAuditStore = PolicyStore<OrderedStorage, MutexLock, StdoutLog>;

int main() {
    EchoProcessor echo;
    UpperProcessor upper;
    std::printf("%s | %s | %s\n", echo.process("hello").c_str(), upper.process("hello").c_str(),
                upper.process("too long input").c_str());
    std::printf("processed: echo=%llu upper=%llu\n", static_cast<unsigned long long>(echo.processed()),
                static_cast<unsigned long long>(upper.processed()));

    Money a{{}, 500}, b{{}, 700};   // first {} initialises the empty Comparable<Money> base
    std::printf("money: a<b %d, a>=b %d, a!=b %d\n", a < b, a >= b, a != b);

    FastLocalStore fast;
    SharedAuditStore shared;
    std::printf("%s (sizeof %zu)\n", FastLocalStore::describe().c_str(), sizeof(fast));
    fast.put("k", "1");
    std::printf("  fast get k = %s\n", fast.get("k")->c_str());
    std::printf("%s (sizeof %zu)\n", SharedAuditStore::describe().c_str(), sizeof(shared));
    shared.put("k", "2");
    std::printf("  shared get k = %s\n", shared.get("k")->c_str());
    return 0;
}
