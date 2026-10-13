// m07_lambdas_std_function.cpp
// Lambdas: captures by value/reference, init-capture, mutable, generic lambdas; what std::function costs.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m07_lambdas_std_function.cpp
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// Template parameter: the lambda's exact type is known, the call is inlined. Zero overhead.
template <class F>
void apply_template(const std::vector<int>& v, F f) { for (int x : v) f(x); }

// std::function: type-erased. One indirect call per invocation, possible heap allocation to store the callable.
void apply_function(const std::vector<int>& v, const std::function<void(int)>& f) { for (int x : v) f(x); }

int main() {
    int threshold = 10;
    int hits = 0;

    auto by_value = [threshold](int x) { return x > threshold; };   // COPIES threshold now, at creation time
    auto by_ref   = [&hits](int x) { if (x > 5) ++hits; };          // REFERS to hits: hits must outlive the lambda
    auto counter  = [n = 0]() mutable { return ++n; };              // init-capture + mutable: the lambda owns state
    auto generic  = [](auto a, auto b) { return a + b; };           // generic lambda: operator() is a template
    auto empty    = [] { std::cout << "  empty lambda called\n"; };

    std::vector<int> v{3, 8, 12, 20};
    threshold = 100;   // no effect on by_value: it already holds its own copy (10)
    std::cout << "by_value(12)=" << by_value(12) << '\n';

    apply_template(v, by_ref);
    std::cout << "hits=" << hits << '\n';
    std::cout << "counter: " << counter() << counter() << counter() << '\n';
    std::cout << "generic: " << generic(1, 2) << ' ' << generic(std::string("a"), std::string("b")) << '\n';
    empty();

    // A lambda is just a struct with an operator(). Its size is the size of its captures.
    std::cout << "sizeof(by_value)=" << sizeof(by_value) << " (one int)"
              << " sizeof(by_ref)=" << sizeof(by_ref) << " (one pointer)"
              << " sizeof(empty)=" << sizeof(empty) << " (empty class)"
              << " sizeof(std::function)=" << sizeof(std::function<void(int)>) << '\n';

    // std::function earns its cost when you must STORE callables of different types in one place.
    std::vector<std::function<void(int)>> handlers;
    handlers.push_back(by_ref);
    handlers.push_back([](int x) { std::cout << "  handler saw " << x << '\n'; });
    for (auto& h : handlers) h(42);
    apply_function(v, by_ref);
    std::cout << "hits=" << hits << '\n';

    // Dangling capture: NEVER return (or store in a longer-lived object) a lambda that captures a local by reference.
    //   auto make_bad = [] { int local = 5; return [&local] { return local; }; };   // calling the result is UB
    auto make_good = [] { int local = 5; return [local] { return local; }; };          // copies: safe
    std::cout << "make_good()()=" << make_good()() << '\n';

    // Capturing 'this' in a member function stores a pointer to the object. If the object dies before the
    // lambda runs (common with callbacks and threads), that is the same dangling bug in disguise.
    return 0;
}
