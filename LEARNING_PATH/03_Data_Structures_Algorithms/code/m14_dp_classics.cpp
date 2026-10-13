// m14_dp_classics.cpp
// The recursion -> memo -> table -> rolling-variables ladder on Fibonacci / climbing stairs,
// then the 1D classics: house robber, coin change (fewest coins and number of ways),
// longest increasing subsequence (O(n^2) and O(n log n) with reconstruction), word break,
// decode ways, and the 2D grid classic unique paths with obstacles. Each table version is
// cross-checked against the brute-force recursion on small random inputs.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m14_dp_classics.cpp -o m14_dp_classics

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

// ---- the ladder, on climbing stairs (ways to climb n steps taking 1 or 2 at a time) ---------

// Step 1: plain recursion. Correct, exponential: ways(n) calls ways(n-1) and ways(n-2), and
// the same subproblems are recomputed over and over. O(phi^n) time, O(n) stack.
std::int64_t stairs_rec(int n) {
    if (n <= 1) return 1;
    return stairs_rec(n - 1) + stairs_rec(n - 2);
}

// Step 2: memoisation (top-down). Same recursion, cache each answer the first time.
// O(n) time, O(n) memo + O(n) stack.
std::int64_t stairs_memo_go(int n, std::vector<std::int64_t>& memo) {
    if (n <= 1) return 1;
    if (memo[n] != -1) return memo[n];
    return memo[n] = stairs_memo_go(n - 1, memo) + stairs_memo_go(n - 2, memo);
}
std::int64_t stairs_memo(int n) {
    std::vector<std::int64_t> memo(n + 1, -1);
    return stairs_memo_go(n, memo);
}

// Step 3: tabulation (bottom-up). Fill dp[] in an order where dependencies are already done.
// O(n) time, O(n) space, no recursion.
std::int64_t stairs_table(int n) {
    std::vector<std::int64_t> dp(std::max(n + 1, 2));
    dp[0] = dp[1] = 1;
    for (int i = 2; i <= n; ++i) dp[i] = dp[i - 1] + dp[i - 2];
    return dp[n];
}

// Step 4: keep only what the recurrence reads. O(n) time, O(1) space.
std::int64_t stairs_o1(int n) {
    std::int64_t prev2 = 1, prev1 = 1;          // dp[i-2], dp[i-1]
    for (int i = 2; i <= n; ++i) {
        std::int64_t cur = prev1 + prev2;
        prev2 = prev1;
        prev1 = cur;
    }
    return prev1;
}

// ---- house robber: max sum with no two adjacent ---------------------------------------------
// dp[i] = max(dp[i-1], dp[i-2] + a[i])   (skip house i, or rob it)
int rob(const std::vector<int>& a) {
    int take_prev = 0, skip_prev = 0;           // best ending with i-1 robbed / not robbed
    for (int x : a) {
        int take = skip_prev + x;
        int skip = std::max(take_prev, skip_prev);
        take_prev = take;
        skip_prev = skip;
    }
    return std::max(take_prev, skip_prev);
}

int rob_brute(const std::vector<int>& a, std::size_t i) {
    if (i >= a.size()) return 0;
    return std::max(rob_brute(a, i + 1), a[i] + rob_brute(a, i + 2));
}

// ---- coin change ---------------------------------------------------------------------------
// Fewest coins to make amount (unlimited coins of each kind), -1 if impossible.
// dp[x] = 1 + min over coins c <= x of dp[x - c]
int coin_change_min(const std::vector<int>& coins, int amount) {
    const int INF = std::numeric_limits<int>::max() / 2;
    std::vector<int> dp(amount + 1, INF);
    dp[0] = 0;
    for (int x = 1; x <= amount; ++x)
        for (int c : coins)
            if (c <= x && dp[x - c] + 1 < dp[x]) dp[x] = dp[x - c] + 1;
    return dp[amount] >= INF ? -1 : dp[amount];
}

int coin_min_brute(const std::vector<int>& coins, int amount) {
    if (amount == 0) return 0;
    int best = -1;
    for (int c : coins)
        if (c <= amount) {
            int sub = coin_min_brute(coins, amount - c);
            if (sub >= 0 && (best < 0 || sub + 1 < best)) best = sub + 1;
        }
    return best;
}

// Number of COMBINATIONS (order does not matter) that make amount.
// Coins in the OUTER loop: each combination is counted once, in coin order.
// Swap the loops and you count PERMUTATIONS (1+2 and 2+1 separately) instead.
std::int64_t coin_change_ways(const std::vector<int>& coins, int amount) {
    std::vector<std::int64_t> dp(amount + 1, 0);
    dp[0] = 1;
    for (int c : coins)
        for (int x = c; x <= amount; ++x) dp[x] += dp[x - c];
    return dp[amount];
}

std::int64_t coin_change_permutations(const std::vector<int>& coins, int amount) {
    std::vector<std::int64_t> dp(amount + 1, 0);
    dp[0] = 1;
    for (int x = 1; x <= amount; ++x)
        for (int c : coins)
            if (c <= x) dp[x] += dp[x - c];
    return dp[amount];
}

// ---- longest increasing subsequence ---------------------------------------------------------
// O(n^2): dp[i] = length of the longest strictly increasing subsequence ENDING at i.
int lis_n2(const std::vector<int>& a) {
    std::vector<int> dp(a.size(), 1);
    int best = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j)
            if (a[j] < a[i]) dp[i] = std::max(dp[i], dp[j] + 1);
        best = std::max(best, dp[i]);
    }
    return best;
}

// O(n log n) patience sorting: tails[k] = smallest possible tail of an increasing subsequence
// of length k+1. tails is always sorted, so each element does one lower_bound.
// Also reconstructs one actual LIS via predecessor links.
std::vector<int> lis_nlogn(const std::vector<int>& a) {
    std::vector<int> tails;                     // values
    std::vector<int> tail_idx;                  // index in a of each tail
    std::vector<int> prev(a.size(), -1);
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto it = std::lower_bound(tails.begin(), tails.end(), a[i]);   // lower_bound: strictly increasing
        std::size_t k = static_cast<std::size_t>(it - tails.begin());
        if (k > 0) prev[i] = tail_idx[k - 1];
        if (it == tails.end()) { tails.push_back(a[i]); tail_idx.push_back(static_cast<int>(i)); }
        else { *it = a[i]; tail_idx[k] = static_cast<int>(i); }
    }
    std::vector<int> seq;
    for (int i = tail_idx.empty() ? -1 : tail_idx.back(); i != -1; i = prev[i]) seq.push_back(a[i]);
    std::reverse(seq.begin(), seq.end());
    return seq;
}

// ---- word break -----------------------------------------------------------------------------
// dp[i] = s[0..i) can be segmented. dp[i] = OR over j < i of dp[j] && s[j..i) in dict.
bool word_break(const std::string& s, const std::vector<std::string>& words) {
    std::unordered_set<std::string> dict(words.begin(), words.end());
    std::size_t maxlen = 0;
    for (const auto& w : words) maxlen = std::max(maxlen, w.size());
    std::vector<char> dp(s.size() + 1, 0);
    dp[0] = 1;
    for (std::size_t i = 1; i <= s.size(); ++i)
        for (std::size_t len = 1; len <= std::min(i, maxlen) && !dp[i]; ++len)   // only plausible word lengths
            if (dp[i - len] && dict.count(s.substr(i - len, len))) dp[i] = 1;
    return dp[s.size()];
}

// ---- decode ways: "12" -> "AB" or "L" = 2 ---------------------------------------------------
int num_decodings(const std::string& s) {
    if (s.empty()) return 0;
    int prev2 = 1;                              // dp[i-2]
    int prev1 = s[0] != '0' ? 1 : 0;            // dp[i-1]
    for (std::size_t i = 1; i < s.size(); ++i) {
        int cur = 0;
        if (s[i] != '0') cur += prev1;                              // single digit 1..9
        int two = (s[i - 1] - '0') * 10 + (s[i] - '0');
        if (s[i - 1] != '0' && two <= 26) cur += prev2;             // two digits 10..26
        prev2 = prev1;
        prev1 = cur;
    }
    return prev1;
}

// ---- unique paths with obstacles (2D grid DP, rolled to one row) ----------------------------
// dp[r][c] = dp[r-1][c] + dp[r][c-1], 0 on obstacles. One row suffices: dp[c] still holds the
// row above until we overwrite it, and dp[c-1] already holds the current row.
std::int64_t unique_paths(const std::vector<std::vector<int>>& grid) {
    std::size_t C = grid[0].size();
    std::vector<std::int64_t> dp(C, 0);
    dp[0] = grid[0][0] == 0 ? 1 : 0;
    for (const auto& row : grid)
        for (std::size_t c = 0; c < C; ++c) {
            if (row[c] == 1) dp[c] = 0;
            else if (c > 0) dp[c] += dp[c - 1];
        }
    return dp[C - 1];
}

int main() {
    for (int n = 0; n <= 25; ++n) {
        std::int64_t r = stairs_rec(n);
        assert(stairs_memo(n) == r && stairs_table(n) == r && stairs_o1(n) == r);
    }
    assert(stairs_o1(90) == stairs_memo(90));  // 90 would take the plain recursion ~10^18 calls

    assert(rob({1, 2, 3, 1}) == 4);
    assert(rob({2, 7, 9, 3, 1}) == 12);
    assert(rob({}) == 0);

    assert(coin_change_min({1, 2, 5}, 11) == 3);
    assert(coin_change_min({2}, 3) == -1);
    assert(coin_change_min({1}, 0) == 0);
    assert(coin_change_min({1, 3, 4}, 6) == 2);              // greedy (4+1+1) would say 3
    assert(coin_change_ways({1, 2, 5}, 5) == 4);
    assert(coin_change_permutations({1, 2, 3}, 4) == 7);

    assert(lis_n2({10, 9, 2, 5, 3, 7, 101, 18}) == 4);
    {
        auto s = lis_nlogn({10, 9, 2, 5, 3, 7, 101, 18});
        assert(s.size() == 4);
        for (std::size_t i = 1; i < s.size(); ++i) assert(s[i - 1] < s[i]);
    }
    assert(lis_nlogn({7, 7, 7}).size() == 1);
    assert(lis_nlogn({}).empty());

    assert(word_break("leetcode", {"leet", "code"}));
    assert(word_break("applepenapple", {"apple", "pen"}));
    assert(!word_break("catsandog", {"cats", "dog", "sand", "and", "cat"}));

    assert(num_decodings("12") == 2);
    assert(num_decodings("226") == 3);
    assert(num_decodings("06") == 0);
    assert(num_decodings("10") == 1);

    assert(unique_paths({{0, 0, 0}, {0, 1, 0}, {0, 0, 0}}) == 2);
    assert(unique_paths(std::vector<std::vector<int>>(3, std::vector<int>(7, 0))) == 28);

    // Differential tests: the DP must agree with the exponential brute force.
    std::mt19937 rng(11);
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<int> a(rng() % 15);
        for (int& x : a) x = static_cast<int>(rng() % 20);
        assert(rob(a) == rob_brute(a, 0));
        assert(static_cast<int>(lis_nlogn(a).size()) == lis_n2(a));

        std::vector<int> coins;
        for (int k = 1 + static_cast<int>(rng() % 3); k > 0; --k) coins.push_back(1 + static_cast<int>(rng() % 7));
        int amount = static_cast<int>(rng() % 20);
        assert(coin_change_min(coins, amount) == coin_min_brute(coins, amount));
    }

    std::printf("all DP classic checks passed\n");
    return 0;
}
