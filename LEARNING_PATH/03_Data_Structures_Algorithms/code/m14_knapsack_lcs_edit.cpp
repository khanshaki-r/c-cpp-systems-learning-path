// m14_knapsack_lcs_edit.cpp
// The 2D DP classics: 0/1 knapsack (full table with item reconstruction, then one row with
// the reverse loop), unbounded knapsack, partition equal subset sum, longest common
// subsequence with reconstruction, edit distance (full table, two rows, and the list of
// operations), and longest palindromic subsequence as LCS(s, reverse(s)).
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m14_knapsack_lcs_edit.cpp -o m14_knapsack_lcs_edit

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

// ---- 0/1 knapsack ---------------------------------------------------------------------------
// dp[i][c] = best value using the first i items with capacity c.
// dp[i][c] = max(dp[i-1][c], dp[i-1][c - w[i-1]] + v[i-1])   (skip item i-1, or take it once)
int knapsack_table(const std::vector<int>& w, const std::vector<int>& v, int cap, std::vector<int>* taken) {
    std::size_t n = w.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(cap + 1, 0));
    for (std::size_t i = 1; i <= n; ++i)
        for (int c = 0; c <= cap; ++c) {
            dp[i][c] = dp[i - 1][c];
            if (w[i - 1] <= c) dp[i][c] = std::max(dp[i][c], dp[i - 1][c - w[i - 1]] + v[i - 1]);
        }
    if (taken) {
        // Walk back: if the value changed when item i-1 was allowed, it was taken.
        taken->clear();
        int c = cap;
        for (std::size_t i = n; i > 0; --i)
            if (dp[i][c] != dp[i - 1][c]) {
                taken->push_back(static_cast<int>(i - 1));
                c -= w[i - 1];
            }
        std::reverse(taken->begin(), taken->end());
    }
    return dp[n][cap];
}

// One row. Capacity loop runs DOWNWARD so dp[c - w] still holds the previous row's value:
// each item is used at most once. Running it upward would let an item be reused (unbounded).
int knapsack_01(const std::vector<int>& w, const std::vector<int>& v, int cap) {
    std::vector<int> dp(cap + 1, 0);
    for (std::size_t i = 0; i < w.size(); ++i)
        for (int c = cap; c >= w[i]; --c) dp[c] = std::max(dp[c], dp[c - w[i]] + v[i]);
    return dp[cap];
}

int knapsack_unbounded(const std::vector<int>& w, const std::vector<int>& v, int cap) {
    std::vector<int> dp(cap + 1, 0);
    for (std::size_t i = 0; i < w.size(); ++i)
        for (int c = w[i]; c <= cap; ++c) dp[c] = std::max(dp[c], dp[c - w[i]] + v[i]);   // upward
    return dp[cap];
}

int knapsack_brute(const std::vector<int>& w, const std::vector<int>& v, std::size_t i, int cap) {
    if (i == w.size()) return 0;
    int best = knapsack_brute(w, v, i + 1, cap);
    if (w[i] <= cap) best = std::max(best, v[i] + knapsack_brute(w, v, i + 1, cap - w[i]));
    return best;
}

// Partition equal subset sum: is there a subset summing to total / 2? A boolean 0/1 knapsack.
bool can_partition(const std::vector<int>& a) {
    int total = std::accumulate(a.begin(), a.end(), 0);
    if (total % 2) return false;
    int target = total / 2;
    std::vector<char> dp(target + 1, 0);
    dp[0] = 1;
    for (int x : a)
        for (int s = target; s >= x; --s) dp[s] = dp[s] || dp[s - x];
    return dp[target];
}

// ---- longest common subsequence -------------------------------------------------------------
// dp[i][j] = LCS of a[0..i) and b[0..j).
// a[i-1] == b[j-1]  ->  dp[i-1][j-1] + 1
// otherwise         ->  max(dp[i-1][j], dp[i][j-1])
std::string lcs(const std::string& a, const std::string& b) {
    std::size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j)
            dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1 : std::max(dp[i - 1][j], dp[i][j - 1]);
    std::string out;
    for (std::size_t i = n, j = m; i > 0 && j > 0;) {
        if (a[i - 1] == b[j - 1]) { out.push_back(a[i - 1]); --i; --j; }
        else if (dp[i - 1][j] >= dp[i][j - 1]) --i;
        else --j;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

int lcs_len_brute(const std::string& a, const std::string& b, std::size_t i, std::size_t j) {
    if (i == a.size() || j == b.size()) return 0;
    if (a[i] == b[j]) return 1 + lcs_len_brute(a, b, i + 1, j + 1);
    return std::max(lcs_len_brute(a, b, i + 1, j), lcs_len_brute(a, b, i, j + 1));
}

bool is_subsequence(const std::string& sub, const std::string& s) {
    std::size_t k = 0;
    for (char ch : s)
        if (k < sub.size() && sub[k] == ch) ++k;
    return k == sub.size();
}

// ---- edit distance (Levenshtein) -----------------------------------------------------------
// dp[i][j] = edits to turn a[0..i) into b[0..j).
// equal last chars -> dp[i-1][j-1]
// else 1 + min(dp[i-1][j]   delete a[i-1],
//              dp[i][j-1]   insert b[j-1],
//              dp[i-1][j-1] replace a[i-1] by b[j-1])
int edit_distance(const std::string& a, const std::string& b, std::vector<std::string>* ops) {
    std::size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1));
    for (std::size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (std::size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j)
            dp[i][j] = a[i - 1] == b[j - 1]
                           ? dp[i - 1][j - 1]
                           : 1 + std::min({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1]});
    if (ops) {
        ops->clear();
        std::size_t i = n, j = m;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) { --i; --j; }
            else if (i > 0 && j > 0 && dp[i][j] == dp[i - 1][j - 1] + 1) {
                ops->push_back(std::string("replace ") + a[i - 1] + "->" + b[j - 1]);
                --i; --j;
            } else if (i > 0 && dp[i][j] == dp[i - 1][j] + 1) {
                ops->push_back(std::string("delete ") + a[i - 1]);
                --i;
            } else {
                ops->push_back(std::string("insert ") + b[j - 1]);
                --j;
            }
        }
        std::reverse(ops->begin(), ops->end());
    }
    return dp[n][m];
}

// Two rows: O(min(n, m)) memory. Swap so the shorter string is the column dimension.
int edit_distance_2rows(std::string a, std::string b) {
    if (a.size() < b.size()) std::swap(a, b);
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] : 1 + std::min({prev[j], cur[j - 1], prev[j - 1]});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

int longest_palindromic_subsequence(const std::string& s) {
    std::string r(s.rbegin(), s.rend());
    return static_cast<int>(lcs(s, r).size());
}

int main() {
    std::vector<int> w{1, 3, 4, 5}, v{1, 4, 5, 7};
    std::vector<int> taken;
    assert(knapsack_table(w, v, 7, &taken) == 9);            // items 1 and 2 (weights 3 + 4)
    assert((taken == std::vector<int>{1, 2}));
    assert(knapsack_01(w, v, 7) == 9);
    assert(knapsack_unbounded({2, 3}, {3, 5}, 7) == 11);     // 3 + 2 + 2 -> 5 + 3 + 3
    assert(knapsack_01({2, 3}, {3, 5}, 7) == 8);

    assert(can_partition({1, 5, 11, 5}));
    assert(!can_partition({1, 2, 3, 5}));

    assert(lcs("abcde", "ace") == "ace");
    assert(lcs("abc", "def").empty());
    assert(lcs("AGGTAB", "GXTXAYB").size() == 4);           // "GTAB"

    std::vector<std::string> ops;
    assert(edit_distance("horse", "ros", &ops) == 3 && ops.size() == 3);
    assert(edit_distance("intention", "execution", nullptr) == 5);
    assert(edit_distance("", "abc", &ops) == 3 && ops.size() == 3);
    assert(edit_distance_2rows("horse", "ros") == 3);
    assert(edit_distance_2rows("kitten", "sitting") == 3);

    assert(longest_palindromic_subsequence("bbbab") == 4);
    assert(longest_palindromic_subsequence("cbbd") == 2);

    std::mt19937 rng(5);
    for (int trial = 0; trial < 300; ++trial) {
        std::size_t n = rng() % 12;
        std::vector<int> ww(n), vv(n);
        for (std::size_t i = 0; i < n; ++i) { ww[i] = 1 + static_cast<int>(rng() % 10); vv[i] = static_cast<int>(rng() % 20); }
        int cap = static_cast<int>(rng() % 30);
        int brute = knapsack_brute(ww, vv, 0, cap);
        assert(knapsack_01(ww, vv, cap) == brute);
        std::vector<int> t;
        assert(knapsack_table(ww, vv, cap, &t) == brute);
        int tw = 0, tv = 0;
        for (int i : t) { tw += ww[i]; tv += vv[i]; }
        assert(tw <= cap && tv == brute);                    // the reconstruction is a real solution

        std::string a, b;
        for (std::size_t i = rng() % 9; i > 0; --i) a.push_back(static_cast<char>('a' + rng() % 3));
        for (std::size_t i = rng() % 9; i > 0; --i) b.push_back(static_cast<char>('a' + rng() % 3));
        std::string l = lcs(a, b);
        assert(static_cast<int>(l.size()) == lcs_len_brute(a, b, 0, 0));
        assert(is_subsequence(l, a) && is_subsequence(l, b));
        assert(edit_distance(a, b, &ops) == edit_distance_2rows(a, b));
        assert(static_cast<int>(ops.size()) == edit_distance_2rows(a, b));
    }

    std::printf("all knapsack / LCS / edit distance checks passed\n");
    return 0;
}
