// m03_string_problems.cpp
// Classic string problems: palindrome, reverse words in place, anagram, longest substring
// without repeats, longest palindromic substring, group anagrams, character replacement.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_string_problems.cpp -o m03_probs

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

// Cast once so that bytes >= 128 do not become negative indices or invalid <cctype> input.
bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// 1. Valid palindrome over alphanumerics, case-insensitive. Two pointers, O(n), O(1).
bool is_palindrome(std::string_view s) {
    int l = 0, r = static_cast<int>(s.size()) - 1;
    while (l < r) {
        while (l < r && !is_alnum(s[static_cast<std::size_t>(l)])) ++l;
        while (l < r && !is_alnum(s[static_cast<std::size_t>(r)])) --r;
        if (lower(s[static_cast<std::size_t>(l)]) != lower(s[static_cast<std::size_t>(r)])) return false;
        ++l; --r;
    }
    return true;
}

// 2. Reverse words in place: reverse everything, then reverse each word back. O(n), O(1).
//    Assumes single spaces and no leading/trailing spaces (state this in an interview).
void reverse_words(std::string& s) {
    std::reverse(s.begin(), s.end());
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ' ') {
            std::reverse(s.begin() + static_cast<long>(start), s.begin() + static_cast<long>(i));
            start = i + 1;
        }
    }
}

// 3. Anagram check with a 26-entry count. +1 for s, -1 for t. O(n), O(1).
bool is_anagram(std::string_view s, std::string_view t) {
    if (s.size() != t.size()) return false;
    std::array<int, 26> count{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        ++count[static_cast<std::size_t>(s[i] - 'a')];
        --count[static_cast<std::size_t>(t[i] - 'a')];
    }
    return std::all_of(count.begin(), count.end(), [](int c) { return c == 0; });
}

// 4. Longest substring without repeating characters. Window [l, r]; `last[c]` is index+1 of
//    the last occurrence. l jumps forward past the previous occurrence and never goes back.
int longest_unique_substring(std::string_view s) {
    std::array<int, 256> last{};
    int l = 0, best = 0;
    for (int r = 0; r < static_cast<int>(s.size()); ++r) {
        unsigned char c = static_cast<unsigned char>(s[static_cast<std::size_t>(r)]);
        l = std::max(l, last[c]);
        last[c] = r + 1;
        best = std::max(best, r - l + 1);
    }
    return best;
}

// 5. Longest palindromic substring by expanding around 2n-1 centres. O(n^2), O(1).
std::string_view longest_palindrome(std::string_view s) {
    if (s.empty()) return s;
    int n = static_cast<int>(s.size()), best_l = 0, best_len = 1;
    auto expand = [&](int l, int r) {
        while (l >= 0 && r < n && s[static_cast<std::size_t>(l)] == s[static_cast<std::size_t>(r)]) { --l; ++r; }
        // loop overshot by one on each side: palindrome is (l, r) exclusive
        if (r - l - 1 > best_len) { best_len = r - l - 1; best_l = l + 1; }
    };
    for (int c = 0; c < n; ++c) {
        expand(c, c);       // odd length, centre at c
        expand(c, c + 1);   // even length, centre between c and c+1
    }
    return s.substr(static_cast<std::size_t>(best_l), static_cast<std::size_t>(best_len));
}

// 6. Group anagrams: key is the sorted word. O(total chars * log k).
std::vector<std::vector<std::string>> group_anagrams(const std::vector<std::string>& words) {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const std::string& w : words) {
        std::string key = w;
        std::sort(key.begin(), key.end());
        groups[key].push_back(w);
    }
    std::vector<std::vector<std::string>> out;
    out.reserve(groups.size());
    for (auto& kv : groups) out.push_back(std::move(kv.second));
    return out;
}

// 7. Longest repeating character replacement with at most k changes. Window is valid when
//    (window length - most frequent count) <= k. max_freq is never decreased: a stale,
//    too-large max_freq only makes the window length stay the same, never grow wrongly,
//    and the answer is the maximum window length ever seen, which remains correct.
int character_replacement(std::string_view s, int k) {
    std::array<int, 26> count{};
    int l = 0, max_freq = 0, best = 0;
    for (int r = 0; r < static_cast<int>(s.size()); ++r) {
        max_freq = std::max(max_freq, ++count[static_cast<std::size_t>(s[static_cast<std::size_t>(r)] - 'A')]);
        while ((r - l + 1) - max_freq > k) --count[static_cast<std::size_t>(s[static_cast<std::size_t>(l++)] - 'A')];
        best = std::max(best, r - l + 1);
    }
    return best;
}

}  // namespace

int main() {
    assert(is_palindrome("A man, a plan, a canal: Panama"));
    assert(!is_palindrome("race a car"));
    assert(is_palindrome(" "));

    std::string words = "the quick brown fox";
    reverse_words(words);
    std::printf("reverse_words -> \"%s\"\n", words.c_str());
    assert(words == "fox brown quick the");

    assert(is_anagram("anagram", "nagaram"));
    assert(!is_anagram("rat", "car"));

    assert(longest_unique_substring("abcabcbb") == 3);
    assert(longest_unique_substring("bbbbb") == 1);
    assert(longest_unique_substring("pwwkew") == 3);
    assert(longest_unique_substring("") == 0);

    std::string_view lp = longest_palindrome("babad");
    std::printf("longest_palindrome(\"babad\") -> \"%.*s\"\n", static_cast<int>(lp.size()), lp.data());
    assert(lp == "bab" || lp == "aba");
    assert(longest_palindrome("cbbd") == "bb");

    auto groups = group_anagrams({"eat", "tea", "tan", "ate", "nat", "bat"});
    assert(groups.size() == 3);
    std::printf("group_anagrams -> %zu groups\n", groups.size());

    assert(character_replacement("ABAB", 2) == 4);
    assert(character_replacement("AABABBA", 1) == 4);

    std::printf("all string-problem checks passed\n");
    return 0;
}
