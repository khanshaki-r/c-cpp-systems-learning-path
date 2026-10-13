// m01_c_vs_cpp_basics.cpp
// What changes for a C programmer: references, overloading, namespaces, auto,
// range-for, uniform init, nullptr, enum class, const correctness.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m01_c_vs_cpp_basics.cpp
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace bank {   // a namespace replaces the C habit of prefixing everything: bank_open_account()
    struct Account {
        std::string id;
        long long balance_paise;
    };

    // Reference parameter: the caller passes an object, we see the SAME object.
    // No pointer syntax, cannot be null, cannot be re-seated.
    void deposit(Account& a, long long paise) { a.balance_paise += paise; }

    // const& : read-only view, no copy. The compiler rejects any modification of 'a'.
    long long balance(const Account& a) { return a.balance_paise; }
}   // namespace bank

// Overloading: same name, different parameter types.
// The compiler picks the overload from the argument types at compile time (no runtime cost).
int         twice(int x)                { return 2 * x; }
double      twice(double x)             { return 2.0 * x; }
std::string twice(const std::string& s) { return s + s; }

enum class Status { Ok, Timeout, Refused };   // scoped: Status::Ok. No implicit conversion to int.

const char* to_string(Status s) {
    switch (s) {   // -Wswitch warns if an enumerator is missing: a free safety net
        case Status::Ok:      return "Ok";
        case Status::Timeout: return "Timeout";
        case Status::Refused: return "Refused";
    }
    return "?";   // keeps -Wreturn-type quiet for out-of-range values (a cast could produce one)
}

int main() {
    bank::Account acc{"ACC-001", 0};   // brace initialisation: no narrowing conversions allowed
    bank::deposit(acc, 50000);
    std::cout << acc.id << " balance " << bank::balance(acc) << " paise\n";

    std::cout << twice(21) << ' ' << twice(1.5) << ' ' << twice(std::string("ab")) << '\n';

    std::vector<int> v{3, 1, 4, 1, 5};
    for (int& x : v) x *= 10;               // range-for by reference: modifies elements in place
    for (int x : v) std::cout << x << ' ';  // by value: copies each int (fine for int, costly for std::string)
    std::cout << '\n';

    auto total = 0LL;                       // auto deduces long long from the initialiser
    for (auto x : v) total += x;
    std::cout << "total=" << total << '\n';

    int* p = nullptr;                       // nullptr has its own type (std::nullptr_t); never confused with int 0
    if (p == nullptr) std::cout << "p is null\n";

    Status s = Status::Timeout;
    // int bad = s;                         // error: no implicit conversion from an enum class
    std::cout << to_string(s) << " as int=" << static_cast<int>(s) << '\n';

    // printf still works in C++. iostream is type-safe and extensible; printf is terser and often faster.
    std::printf("%s has %lld paise\n", acc.id.c_str(), acc.balance_paise);
    return 0;
}
