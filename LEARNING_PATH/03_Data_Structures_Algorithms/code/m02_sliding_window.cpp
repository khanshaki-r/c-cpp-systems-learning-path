// m02_sliding_window.cpp
// Fixed and variable sliding windows: max sum of size k, minimum subarray with sum >= target,
// longest ones after flipping k zeroes, longest subarray with at most two distinct values.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_sliding_window.cpp -o m02_sw

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

// Fixed window of size k: add the entering element, subtract the leaving one.
long long max_sum_window(const std::vector<int>& v, int k) {
    int n = static_cast<int>(v.size());
    if (k <= 0 || k > n) return 0;
    long long sum = 0;
    for (int i = 0; i < k; ++i) sum += v[static_cast<std::size_t>(i)];
    long long best = sum;
    for (int r = k; r < n; ++r) {
        sum += v[static_cast<std::size_t>(r)] - v[static_cast<std::size_t>(r - k)];
        best = std::max(best, sum);
    }
    return best;
}

// Variable window, shortest valid. Requires all positive so that "sum >= target" is monotone.
int min_subarray_len(int target, const std::vector<int>& v) {
    int n = static_cast<int>(v.size()), l = 0, best = n + 1;
    long long sum = 0;
    for (int r = 0; r < n; ++r) {
        sum += v[static_cast<std::size_t>(r)];
        while (sum >= target) {   // valid: record, then try to shrink
            best = std::min(best, r - l + 1);
            sum -= v[static_cast<std::size_t>(l++)];
        }
    }
    return best == n + 1 ? 0 : best;
}

// Variable window, longest valid. Valid = at most k zeroes inside.
int longest_ones(const std::vector<int>& v, int k) {
    int l = 0, zeros = 0, best = 0;
    for (int r = 0; r < static_cast<int>(v.size()); ++r) {
        if (v[static_cast<std::size_t>(r)] == 0) ++zeros;
        while (zeros > k)                                    // invalid: drop from the left
            if (v[static_cast<std::size_t>(l++)] == 0) --zeros;
        best = std::max(best, r - l + 1);
    }
    return best;
}

// Longest subarray with at most `limit` distinct values. Same template; validity needs a
// count map so we know when a value fully leaves the window.
int longest_k_distinct(const std::vector<int>& v, int limit) {
    std::unordered_map<int, int> count;
    int l = 0, best = 0;
    for (int r = 0; r < static_cast<int>(v.size()); ++r) {
        ++count[v[static_cast<std::size_t>(r)]];
        while (static_cast<int>(count.size()) > limit) {
            int left = v[static_cast<std::size_t>(l++)];
            if (--count[left] == 0) count.erase(left);   // erase so size() reflects distinct count
        }
        best = std::max(best, r - l + 1);
    }
    return best;
}

}  // namespace

int main() {
    assert(max_sum_window({2, 1, 5, 1, 3, 2}, 3) == 9);
    assert(min_subarray_len(7, {2, 3, 1, 2, 4, 3}) == 2);
    assert(min_subarray_len(11, {1, 1, 1, 1, 1, 1, 1, 1}) == 0);
    assert(longest_ones({1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0}, 2) == 6);
    assert(longest_k_distinct({1, 2, 1, 2, 3}, 2) == 4);
    assert(longest_k_distinct({1, 2, 3, 2, 2}, 2) == 4);

    std::printf("max_sum_window(k=3)    = %lld\n", max_sum_window({2, 1, 5, 1, 3, 2}, 3));
    std::printf("min_subarray_len(7)    = %d\n", min_subarray_len(7, {2, 3, 1, 2, 4, 3}));
    std::printf("longest_ones(k=2)      = %d\n", longest_ones({1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0}, 2));
    std::printf("longest_k_distinct(2)  = %d\n", longest_k_distinct({1, 2, 1, 2, 3}, 2));
    std::printf("all sliding-window checks passed\n");
    return 0;
}
