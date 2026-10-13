// m13_cpp17_tour.cpp
// C++17 features in one file: structured bindings, if-init, string_view, CTAD, fold expressions,
// [[nodiscard]], [[maybe_unused]], inline variables, std::any, std::byte, std::filesystem.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m13_cpp17_tour.cpp
#include <any>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

inline constexpr int kProtocolVersion = 3;   // inline variable: define it in a header, get ONE object program-wide

struct Stats { int count; double mean; };
Stats compute(const std::vector<double>& v) {
    double sum = 0;
    for (double x : v) sum += x;
    return {static_cast<int>(v.size()), v.empty() ? 0.0 : sum / static_cast<double>(v.size())};
}

template <class... Ts>
constexpr bool all_positive(Ts... xs) { return ((xs > 0) && ...); }   // fold over &&; true for an empty pack

[[nodiscard]] bool try_reserve(std::vector<int>& v, std::size_t n) {   // ignoring the result is a warning (-Werror: error)
    if (n > 1'000'000) return false;
    v.reserve(n);
    return true;
}

std::string_view trim(std::string_view s) {   // no allocation: a string_view is just (pointer, length)
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

int main() {
    // Structured bindings: unpack structs, pairs, tuples and arrays into named variables.
    auto [count, mean] = compute({1.0, 2.0, 6.0});
    std::cout << "count=" << count << " mean=" << mean << '\n';

    std::map<std::string, int> ages{{"ann", 31}, {"bob", 42}};
    for (const auto& [name, age] : ages) std::cout << name << ':' << age << ' ';
    std::cout << '\n';
    if (auto [it, inserted] = ages.insert({"ann", 99}); !inserted)
        std::cout << "ann already present with " << it->second << '\n';

    // if with initialiser: the lookup result lives only inside the if/else.
    if (auto it = ages.find("bob"); it != ages.end()) std::cout << "bob is " << it->second << '\n';
    else std::cout << "no bob\n";

    // string_view: cheap to pass and slice. It does NOT own: never let it outlive the string it points into.
    std::string line = "   GET /balance   ";
    std::string_view v = trim(line);
    std::cout << '[' << v << "] len=" << v.size() << '\n';
    // std::string_view dangling = trim(std::string("temp"));   // BUG: the temporary dies at the semicolon

    // Class template argument deduction (CTAD): no more std::pair<int, double> p(...)
    std::pair p{1, 2.5};                                       // std::pair<int, double>
    std::vector words{std::string("a"), std::string("b")};     // std::vector<std::string>
    std::tuple t{1, 'c', "txt"};                               // std::tuple<int, char, const char*>
    std::cout << p.first << ' ' << words.size() << ' ' << std::get<1>(t) << '\n';

    // Fold expressions evaluated at compile time.
    static_assert(all_positive(1, 2, 3));
    std::cout << "all_positive(1,-2)=" << all_positive(1, -2) << '\n';

    // [[nodiscard]] and [[maybe_unused]]
    std::vector<int> buf;
    if (!try_reserve(buf, 10)) std::cout << "reserve refused\n";
    [[maybe_unused]] bool ok = try_reserve(buf, 2'000'000);   // only used inside assert(); the attribute keeps -DNDEBUG builds warning-free
    assert(!ok);

    // std::any: holds anything, checked at run time. Prefer std::variant when the set of types is known.
    std::any a = 42;
    a = std::string("now a string");
    if (auto* s = std::any_cast<std::string>(&a)) std::cout << "any holds a string: " << *s << '\n';

    // std::byte: raw memory that is NOT an arithmetic type. Only bitwise ops are allowed.
    std::byte b{0x0F};
    b <<= 4;
    std::cout << "byte=" << std::to_integer<int>(b) << '\n';

    // std::filesystem: portable paths, directories, sizes, iteration.
    fs::path dir = fs::temp_directory_path() / "m13_demo";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "hello.txt");   // path converts implicitly; the file closes at the brace (RAII)
        out << "hello\n";
    }
    for (const auto& entry : fs::directory_iterator(dir))
        std::cout << entry.path().filename() << " size=" << entry.file_size()
                  << " ext=" << entry.path().extension() << '\n';
    fs::remove_all(dir);

    // Parallel algorithms also arrived in C++17 but on GCC they need Intel TBB at link time (-ltbb):
    //   #include <execution>   std::sort(std::execution::par, v.begin(), v.end());
    std::cout << "protocol v" << kProtocolVersion << '\n';
    return 0;
}
