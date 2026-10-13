// m08_variadic_if_constexpr.cpp
// Variadic templates, fold expressions, if constexpr, type traits, enable_if (concepts preview).
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m08_variadic_if_constexpr.cpp
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

// Variadic template + fold expression (C++17): one function for any number of arguments.
template <class... Ts>
void log_line(const Ts&... parts) {
    (std::cout << ... << parts) << '\n';   // expands to ((cout << p1) << p2) << p3 ...
}

template <class... Ts>
auto sum_all(Ts... xs) { return (xs + ... + 0); }   // 0 is the value for an empty pack

// if constexpr: the branch not taken is DISCARDED, not compiled. So to_text(int) never sees 'return v' as string.
template <class T>
std::string to_text(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) {
        return v;
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(v);
    } else if constexpr (std::is_pointer_v<T>) {
        return v ? to_text(*v) : std::string("null");
    } else {
        static_assert(sizeof(T) == 0, "to_text: unsupported type");   // fires only if this branch is instantiated
    }
}

// Pre-C++17 style recursion for comparison: a base case plus peel-one-off.
void print_all() {}
template <class T, class... Rest>
void print_all(const T& first, const Rest&... rest) {
    std::cout << first;
    if constexpr (sizeof...(rest) > 0) std::cout << ", ";
    print_all(rest...);
}

// Type traits choose a strategy at compile time.
template <class T>
const char* copy_strategy() {
    return std::is_trivially_copyable_v<T> ? "memcpy" : "element-wise copy constructor";
}

// Constraining a template in C++17: enable_if. In C++20 the same thing reads
//   template <std::integral T> T twice(T x) { return 2 * x; }
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
T twice(T x) { return 2 * x; }

int main() {
    log_line("txn ", 42, " amount ", 3.5);
    std::cout << sum_all(1, 2, 3, 4) << ' ' << sum_all(1.5, 2.5) << ' ' << sum_all() << '\n';

    int n = 7;
    std::cout << to_text(std::string("s")) << ' ' << to_text(3.25) << ' ' << to_text(&n) << ' '
              << to_text<int*>(nullptr) << '\n';
    // to_text(std::vector<int>{});   // error: static_assert "unsupported type"

    print_all(1, "two", 3.0);
    std::cout << '\n';

    std::cout << "int: " << copy_strategy<int>() << ", std::string: " << copy_strategy<std::string>()
              << ", std::vector<int>: " << copy_strategy<std::vector<int>>() << '\n';

    std::cout << twice(21) << ' ' << twice(21L) << '\n';
    // twice(1.5);   // error: no matching function. enable_if removed the only candidate. (Concepts give a clearer message.)
    return 0;
}
