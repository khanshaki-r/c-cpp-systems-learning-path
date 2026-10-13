// m04_hash_problems.cpp
// Six hash-map problems: two sum, longest consecutive sequence, group anagrams by count key,
// first unique character, top-k frequent (bucket sort by frequency), contains duplicate
// within distance k. Plus a std::hash specialisation for a user struct.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_hash_problems.cpp -o m04_probs

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// A composite key and how to make it hashable. Interviewers accept either a std::hash
// specialisation (shown) or a functor passed as the third template argument.
struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};
namespace std {
template <>
struct hash<Point> {
    std::size_t operator()(const Point& p) const noexcept {
        std::size_t seed = std::hash<int>{}(p.x);
        // boost::hash_combine. Plain XOR would make (a,b) and (b,a) collide.
        seed ^= std::hash<int>{}(p.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};
}  // namespace std

namespace {

// 1. Two Sum, unsorted. Look up the complement BEFORE inserting the current value so that
//    the two indices are always distinct. O(n) time, O(n) space.
std::pair<int, int> two_sum(const std::vector<int>& v, int target) {
    std::unordered_map<int, int> index_of;   // value -> index
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        int x = v[static_cast<std::size_t>(i)];
        auto it = index_of.find(target - x);
        if (it != index_of.end()) return {it->second, i};
        index_of.emplace(x, i);
    }
    return {-1, -1};
}

// 2. Longest consecutive sequence in O(n). Only start counting from run starts (x-1 absent),
//    so every element is walked over at most once across the whole loop.
int longest_consecutive(const std::vector<int>& v) {
    std::unordered_set<int> s(v.begin(), v.end());
    int best = 0;
    for (int x : s) {
        if (s.count(x - 1)) continue;        // not a run start
        int len = 1;
        while (s.count(x + len)) ++len;
        best = std::max(best, len);
    }
    return best;
}

// 3. Group anagrams with a 26-count key: O(total chars), no sorting.
std::vector<std::vector<std::string>> group_anagrams(const std::vector<std::string>& words) {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const std::string& w : words) {
        std::array<int, 26> cnt{};
        for (char c : w) ++cnt[static_cast<std::size_t>(c - 'a')];
        std::string key;
        for (int c : cnt) { key += std::to_string(c); key += '#'; }   // '#' separates counts
        groups[key].push_back(w);
    }
    std::vector<std::vector<std::string>> out;
    for (auto& kv : groups) out.push_back(std::move(kv.second));
    return out;
}

// 4. First unique character: count then scan. 256-entry array beats a map for bytes.
int first_unique_char(std::string_view s) {
    std::array<int, 256> cnt{};
    for (char c : s) ++cnt[static_cast<unsigned char>(c)];
    for (int i = 0; i < static_cast<int>(s.size()); ++i)
        if (cnt[static_cast<unsigned char>(s[static_cast<std::size_t>(i)])] == 1) return i;
    return -1;
}

// 5. Top-k frequent in O(n): count, then bucket values by frequency (frequency <= n), then
//    read buckets from the highest frequency down.
std::vector<int> top_k_frequent(const std::vector<int>& v, int k) {
    std::unordered_map<int, int> freq;
    for (int x : v) ++freq[x];
    std::vector<std::vector<int>> by_count(v.size() + 1);
    for (const auto& kv : freq) by_count[static_cast<std::size_t>(kv.second)].push_back(kv.first);
    std::vector<int> out;
    for (std::size_t c = by_count.size(); c-- > 0 && static_cast<int>(out.size()) < k;)
        for (int x : by_count[c])
            if (static_cast<int>(out.size()) < k) out.push_back(x);
    return out;
}

// 6. Contains duplicate within distance k: a sliding-window set of the last k values.
bool contains_nearby_duplicate(const std::vector<int>& v, int k) {
    std::unordered_set<int> window;
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        int x = v[static_cast<std::size_t>(i)];
        if (window.count(x)) return true;
        window.insert(x);
        if (i >= k) window.erase(v[static_cast<std::size_t>(i - k)]);   // keep only the last k
    }
    return false;
}

}  // namespace

int main() {
    {
        auto [i, j] = two_sum({2, 7, 11, 15}, 9);
        assert(i == 0 && j == 1);
        auto [a, b] = two_sum({3, 3}, 6);
        assert(a == 0 && b == 1);
        std::printf("two_sum({2,7,11,15}, 9) -> (%d, %d)\n", i, j);
    }
    assert(longest_consecutive({100, 4, 200, 1, 3, 2}) == 4);
    assert(longest_consecutive({0, 3, 7, 2, 5, 8, 4, 6, 0, 1}) == 9);
    assert(longest_consecutive({}) == 0);

    assert(group_anagrams({"eat", "tea", "tan", "ate", "nat", "bat"}).size() == 3);

    assert(first_unique_char("leetcode") == 0);
    assert(first_unique_char("loveleetcode") == 2);
    assert(first_unique_char("aabb") == -1);

    {
        auto top = top_k_frequent({1, 1, 1, 2, 2, 3}, 2);
        std::sort(top.begin(), top.end());
        assert((top == std::vector<int>{1, 2}));
    }
    assert(contains_nearby_duplicate({1, 2, 3, 1}, 3));
    assert(!contains_nearby_duplicate({1, 2, 3, 1, 2, 3}, 2));

    std::unordered_set<Point> pts;
    pts.insert({1, 2});
    pts.insert({2, 1});
    pts.insert({1, 2});
    assert(pts.size() == 2);
    std::printf("Point set: %zu entries, %zu buckets, load %.2f\n", pts.size(), pts.bucket_count(),
                pts.load_factor());

    std::printf("all hash-problem checks passed\n");
    return 0;
}
