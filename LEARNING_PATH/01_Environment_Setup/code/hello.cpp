// hello.cpp - proves the compiler really speaks C++17.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread hello.cpp -o hello
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

// std::optional: a value that may be absent, instead of a magic -1 or NULL.
std::optional<int> find_index(const std::vector<std::string>& v, std::string_view key) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == key) return static_cast<int>(i);
    }
    return std::nullopt;
}

template <typename T>
std::string describe(const T& value) {
    // if constexpr: the branch not taken is not even compiled.
    if constexpr (std::is_integral_v<T>) {
        return "integral " + std::to_string(value);
    } else {
        return "other";
    }
}

}  // namespace

int main() {
    std::vector<std::string> words{"alpha", "beta", "gamma"};

    // Structured binding over a map.
    std::map<std::string, int> counts{{"get", 3}, {"set", 5}};
    for (const auto& [name, n] : counts) {
        std::cout << name << " = " << n << '\n';
    }

    if (auto idx = find_index(words, "beta"); idx.has_value()) {
        std::cout << "beta at index " << *idx << '\n';
    }

    std::cout << describe(42) << ", " << describe(3.5) << '\n';

    // shared_mutex exists only from C++17.
    std::shared_mutex m;
    {
        std::shared_lock<std::shared_mutex> reader(m);
        std::cout << "shared_lock acquired\n";
    }

    std::cout << "cwd: " << std::filesystem::current_path() << '\n';
    std::cout << "C++ standard: " << __cplusplus << '\n';
    std::cout << "all C++17 checks passed\n";
    return 0;
}
