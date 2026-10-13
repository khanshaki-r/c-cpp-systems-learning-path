// m02_prefix_sums_kadane.cpp
// Prefix sums for O(1) range queries, "subarray sum equals k" with a hash map (handles
// negatives), product-except-self, Kadane's maximum subarray (with indices), best time to
// buy and sell stock, and the circular variant.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_prefix_sums_kadane.cpp -o m02_ps

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

// P[i] = sum of v[0..i). Size n+1 so that sum(v[i..j)) = P[j] - P[i] has no special cases.
std::vector<long long> build_prefix(const std::vector<int>& v) {
    std::vector<long long> p(v.size() + 1, 0);
    for (std::size_t i = 0; i < v.size(); ++i) p[i + 1] = p[i] + v[i];
    return p;
}

long long range_sum(const std::vector<long long>& p, int i, int j) {  // [i, j)
    return p[static_cast<std::size_t>(j)] - p[static_cast<std::size_t>(i)];
}

// Count subarrays with sum == k. Works with negatives because we never shrink a window;
// we ask "how many earlier prefixes equal current - k".
int subarray_sum_equals_k(const std::vector<int>& v, int k) {
    std::unordered_map<long long, int> seen;
    seen[0] = 1;  // the empty prefix, so subarrays starting at index 0 are counted
    long long prefix = 0;
    int count = 0;
    for (int x : v) {
        prefix += x;
        auto it = seen.find(prefix - k);
        if (it != seen.end()) count += it->second;
        ++seen[prefix];
    }
    return count;
}

// Product of array except self: left running product into out, then right running product.
std::vector<long long> product_except_self(const std::vector<int>& v) {
    std::size_t n = v.size();
    std::vector<long long> out(n, 1);
    long long run = 1;
    for (std::size_t i = 0; i < n; ++i) { out[i] = run; run *= v[i]; }       // product of v[0..i)
    run = 1;
    for (std::size_t i = n; i-- > 0;) { out[i] *= run; run *= v[i]; }        // times product of v(i..n)
    return out;
}

// Kadane with indices. Returns {best_sum, start, end_inclusive}.
std::tuple<long long, int, int> max_subarray(const std::vector<int>& v) {
    long long cur = v[0], best = v[0];
    int start = 0, best_l = 0, best_r = 0;
    for (int i = 1; i < static_cast<int>(v.size()); ++i) {
        int x = v[static_cast<std::size_t>(i)];
        if (cur + x < x) { cur = x; start = i; }   // restart: the prefix was dragging us down
        else cur += x;                             // extend
        if (cur > best) { best = cur; best_l = start; best_r = i; }
    }
    return {best, best_l, best_r};
}

// Best single buy/sell: max over i of price[i] - min(price[0..i)). Kadane on differences.
int max_profit(const std::vector<int>& price) {
    int lo = price.empty() ? 0 : price[0], best = 0;
    for (int p : price) {
        best = std::max(best, p - lo);
        lo = std::min(lo, p);
    }
    return best;
}

// Circular maximum subarray: either the normal Kadane answer, or total minus the minimum
// subarray (the wrap-around case). If all are negative the second form would give 0 for an
// "empty" subarray, so guard it.
long long max_subarray_circular(const std::vector<int>& v) {
    long long total = 0, cur_max = 0, best_max = v[0], cur_min = 0, best_min = v[0];
    for (int x : v) {
        total += x;
        cur_max = std::max<long long>(x, cur_max + x); best_max = std::max(best_max, cur_max);
        cur_min = std::min<long long>(x, cur_min + x); best_min = std::min(best_min, cur_min);
    }
    if (best_max < 0) return best_max;             // all negative
    return std::max(best_max, total - best_min);
}

}  // namespace

int main() {
    std::vector<int> v = {3, -1, 4, -1, 5, -9, 2, 6};
    auto p = build_prefix(v);
    assert(range_sum(p, 0, 8) == 9);
    assert(range_sum(p, 2, 5) == 8);        // 4 + -1 + 5
    std::printf("range_sum [2,5) = %lld\n", range_sum(p, 2, 5));

    assert(subarray_sum_equals_k({1, 1, 1}, 2) == 2);
    assert(subarray_sum_equals_k({1, 2, 3}, 3) == 2);
    assert(subarray_sum_equals_k({1, -1, 0}, 0) == 3);
    std::printf("subarray_sum_equals_k({1,-1,0}, 0) = %d\n", subarray_sum_equals_k({1, -1, 0}, 0));

    auto prod = product_except_self({1, 2, 3, 4});
    assert((prod == std::vector<long long>{24, 12, 8, 6}));
    prod = product_except_self({0, 1});
    assert((prod == std::vector<long long>{1, 0}));

    auto [best, l, r] = max_subarray({-2, 1, -3, 4, -1, 2, 1, -5, 4});
    std::printf("max_subarray = %lld over [%d, %d]\n", best, l, r);
    assert(best == 6 && l == 3 && r == 6);

    assert(max_profit({7, 1, 5, 3, 6, 4}) == 5);
    assert(max_profit({7, 6, 4, 3, 1}) == 0);

    assert(max_subarray_circular({5, -3, 5}) == 10);
    assert(max_subarray_circular({-3, -2, -3}) == -2);
    assert(max_subarray_circular({1, -2, 3, -2}) == 3);
    std::printf("all prefix-sum / Kadane checks passed\n");
    return 0;
}
