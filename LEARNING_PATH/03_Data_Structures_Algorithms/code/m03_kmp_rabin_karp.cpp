// m03_kmp_rabin_karp.cpp
// Substring search three ways: naive O(n*m), Rabin-Karp rolling hash (expected O(n+m), with
// verification on hash match), and KMP with a failure table (worst-case O(n+m)).
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_kmp_rabin_karp.cpp -o m03_search

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Baseline. Restarts the comparison at every text position: O(n*m) worst case.
std::vector<std::size_t> find_all_naive(std::string_view text, std::string_view pat) {
    std::vector<std::size_t> hits;
    if (pat.empty() || pat.size() > text.size()) return hits;
    for (std::size_t i = 0; i + pat.size() <= text.size(); ++i) {
        std::size_t k = 0;
        while (k < pat.size() && text[i + k] == pat[k]) ++k;
        if (k == pat.size()) hits.push_back(i);
    }
    return hits;
}

// Rabin-Karp. Treat each window as a base-B number modulo a prime M. Sliding one character
// costs O(1). A hash match is verified with a direct compare so collisions cannot produce
// false positives (they only cost time).
std::vector<std::size_t> find_all_rabin_karp(std::string_view text, std::string_view pat) {
    std::vector<std::size_t> hits;
    const std::size_t n = text.size(), m = pat.size();
    if (m == 0 || m > n) return hits;

    const std::uint64_t B = 131, M = 1'000'000'007ULL;
    std::uint64_t h_pat = 0, h_win = 0, b_pow = 1;   // b_pow = B^(m-1) mod M
    for (std::size_t i = 0; i < m; ++i) {
        h_pat = (h_pat * B + static_cast<unsigned char>(pat[i])) % M;
        h_win = (h_win * B + static_cast<unsigned char>(text[i])) % M;
        if (i + 1 < m) b_pow = (b_pow * B) % M;
    }
    for (std::size_t i = 0;; ++i) {
        if (h_win == h_pat && text.substr(i, m) == pat) hits.push_back(i);  // verify
        if (i + m >= n) break;
        // remove text[i]*B^(m-1), shift, add text[i+m]. Add M before subtracting to stay >= 0.
        std::uint64_t out = (static_cast<unsigned char>(text[i]) * b_pow) % M;
        h_win = ((h_win + M - out) * B + static_cast<unsigned char>(text[i + m])) % M;
    }
    return hits;
}

// KMP failure table. f[k] = length of the longest proper prefix of pat[0..k] that is also a
// suffix of it. Built by matching the pattern against itself.
std::vector<std::size_t> build_failure(std::string_view pat) {
    std::vector<std::size_t> f(pat.size(), 0);
    std::size_t k = 0;                           // length of current matched prefix
    for (std::size_t i = 1; i < pat.size(); ++i) {
        while (k > 0 && pat[i] != pat[k]) k = f[k - 1];   // fall back to a shorter border
        if (pat[i] == pat[k]) ++k;
        f[i] = k;
    }
    return f;
}

// KMP search. The text index i never decreases; k is reset via the failure table.
std::vector<std::size_t> find_all_kmp(std::string_view text, std::string_view pat) {
    std::vector<std::size_t> hits;
    if (pat.empty() || pat.size() > text.size()) return hits;
    std::vector<std::size_t> f = build_failure(pat);
    std::size_t k = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        while (k > 0 && text[i] != pat[k]) k = f[k - 1];
        if (text[i] == pat[k]) ++k;
        if (k == pat.size()) {
            hits.push_back(i + 1 - pat.size());
            k = f[k - 1];                         // continue looking for overlapping matches
        }
    }
    return hits;
}

void show(const char* label, const std::vector<std::size_t>& v) {
    std::printf("%-12s", label);
    for (std::size_t x : v) std::printf("%zu ", x);
    std::printf("\n");
}

}  // namespace

int main() {
    {
        std::vector<std::size_t> f = build_failure("ababc");
        assert((f == std::vector<std::size_t>{0, 0, 1, 2, 0}));
        f = build_failure("abcabd");
        assert((f == std::vector<std::size_t>{0, 0, 0, 1, 2, 0}));
        f = build_failure("aaaa");
        assert((f == std::vector<std::size_t>{0, 1, 2, 3}));
    }
    const std::string text = "abcabcabdabcabdaaaaab";
    for (std::string_view pat : {"abcabd", "aaaab", "abc", "zzz", "a"}) {
        auto a = find_all_naive(text, pat);
        auto b = find_all_rabin_karp(text, pat);
        auto c = find_all_kmp(text, pat);
        assert(a == b && b == c);
        std::printf("pattern \"%.*s\":\n", static_cast<int>(pat.size()), pat.data());
        show("  naive", a);
        show("  rabin-karp", b);
        show("  kmp", c);
    }
    // Overlapping matches must all be found.
    assert(find_all_kmp("aaaa", "aa").size() == 3);
    assert(find_all_rabin_karp("aaaa", "aa").size() == 3);
    std::printf("all search checks passed\n");
    return 0;
}
