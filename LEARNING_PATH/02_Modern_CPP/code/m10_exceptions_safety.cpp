// m10_exceptions_safety.cpp
// Exception safety guarantees (basic / strong / nothrow), unwinding through destructors,
// translating library exceptions into domain exceptions, catching by const reference.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m10_exceptions_safety.cpp
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct ParseError : std::runtime_error {   // a domain exception: catch sites can be specific
    int line;
    ParseError(const std::string& what, int l) : std::runtime_error(what), line(l) {}
};

class Ledger {
public:
    // BASIC guarantee only: if push_back throws, entries_ is still a valid vector, but total_ was
    // already updated. The object is valid but INCONSISTENT. Avoid this ordering.
    void add_basic(long long amount) {
        total_ += amount;
        entries_.push_back(amount);   // may throw std::bad_alloc
    }
    // STRONG guarantee: do everything that can throw FIRST, then commit with steps that cannot throw.
    void add_strong(long long amount) {
        entries_.push_back(amount);   // may throw; if it does, nothing has changed
        total_ += amount;             // cannot throw: the commit point
    }
    // Multi-step update with the strong guarantee: build the new state on the side, then swap (noexcept).
    void replace_all(const std::vector<long long>& fresh) {
        std::vector<long long> copy(fresh);   // may throw; *this is untouched
        long long sum = 0;
        for (long long v : copy) sum += v;
        entries_.swap(copy);                  // noexcept
        total_ = sum;                         // noexcept
    }
    long long total() const noexcept { return total_; }
    std::size_t count() const noexcept { return entries_.size(); }
private:
    std::vector<long long> entries_;
    long long total_ = 0;
};

long long parse_amount(const std::string& s, int line) {
    try {
        std::size_t used = 0;
        long long v = std::stoll(s, &used);
        if (used != s.size()) throw ParseError("trailing characters in '" + s + "'", line);
        return v;
    } catch (const std::invalid_argument&) {
        throw ParseError("not a number: '" + s + "'", line);   // translate: callers should not know about stoll
    } catch (const std::out_of_range&) {
        throw ParseError("out of range: '" + s + "'", line);
    }
}

struct Guard {   // destructors run during unwinding. A destructor must NOT throw: throwing while
    const char* name;   // another exception is in flight calls std::terminate.
    ~Guard() { std::cout << "  unwinding through " << name << '\n'; }
};

void level3(const std::string& s) { Guard g{"level3"}; std::cout << "parsed " << parse_amount(s, 3) << '\n'; }
void level2(const std::string& s) { Guard g{"level2"}; level3(s); }
void level1(const std::string& s) { Guard g{"level1"}; level2(s); }

int main() {
    Ledger l;
    l.add_strong(100);
    l.add_strong(250);
    l.replace_all({1, 2, 3});
    std::cout << "count=" << l.count() << " total=" << l.total() << '\n';

    std::cout << "good input:\n";
    level1("42");
    std::cout << "bad input:\n";
    try {
        level1("4x2");
        std::cout << "not reached\n";
    } catch (const ParseError& e) {      // catch by const reference: no slicing, no copy
        std::cout << "ParseError at line " << e.line << ": " << e.what() << '\n';
    } catch (const std::exception& e) {  // the most general handler goes LAST
        std::cout << "other: " << e.what() << '\n';
    }

    static_assert(noexcept(l.total()), "total() is declared noexcept");
    static_assert(!noexcept(l.add_strong(1)), "add_strong may throw (allocation)");
    static_assert(std::is_nothrow_move_constructible_v<Ledger>, "vector move is noexcept, so Ledger's is too");
    return 0;
}
