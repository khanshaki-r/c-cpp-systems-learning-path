// m09_binary_search.cpp
// Binary search variants: lower/upper bound by hand (checked against std::), first/last/count,
// predicate search (first_true), integer sqrt, search-on-answer (ship capacity, Koko),
// rotated array search + minimum, peak element, 2D matrix, bisection on a real function,
// B-tree node child selection, and a map-vs-sorted-vector lookup benchmark.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m09_binary_search.cpp -o m09_bs

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <vector>

namespace {

// ---- the template ---------------------------------------------------------------------------
// First index i with v[i] >= t, or v.size(). Invariant: v[0..lo) < t, v[hi..n) >= t.
std::size_t lower_bound(const std::vector<int>& v, int t) {
    std::size_t lo = 0, hi = v.size();
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
// First index i with v[i] > t. Only the comparison changes.
std::size_t upper_bound(const std::vector<int>& v, int t) {
    std::size_t lo = 0, hi = v.size();
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (v[mid] <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
long long count_of(const std::vector<int>& v, int t) {
    return static_cast<long long>(upper_bound(v, t)) - static_cast<long long>(lower_bound(v, t));
}
long long first_occurrence(const std::vector<int>& v, int t) {
    std::size_t i = lower_bound(v, t);
    return (i < v.size() && v[i] == t) ? static_cast<long long>(i) : -1;
}
long long last_occurrence(const std::vector<int>& v, int t) {
    std::size_t i = upper_bound(v, t);
    return (i > 0 && v[i - 1] == t) ? static_cast<long long>(i - 1) : -1;
}

// ---- predicate search: first x in [lo, hi) with P(x) true (P monotone false -> true) --------
long long first_true(long long lo, long long hi, const std::function<bool(long long)>& P) {
    while (lo < hi) {
        long long mid = lo + (hi - lo) / 2;
        if (P(mid)) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// Largest r with r*r <= x. P(r) = r*r > x is monotone; answer = first_true - 1.
// Compare with x / r to avoid overflow for x near 2^63.
long long isqrt(long long x) {
    if (x < 2) return x;
    long long r = first_true(1, 3'037'000'500LL, [x](long long m) { return m > x / m; });
    return r - 1;
}

// Minimum ship capacity to ship all weights in order within `days` days.
int ship_within_days(const std::vector<int>& w, int days) {
    auto feasible = [&](long long cap) {
        int used = 1;
        long long load = 0;
        for (int x : w) {
            if (load + x > cap) { ++used; load = 0; }   // start a new day
            load += x;
        }
        return used <= days;
    };
    long long lo = *std::max_element(w.begin(), w.end());
    long long hi = std::accumulate(w.begin(), w.end(), 0LL);
    return static_cast<int>(first_true(lo, hi + 1, feasible));
}

// Koko: minimum eating speed k so that all piles are finished within h hours (ceil(p/k) each).
int min_eating_speed(const std::vector<int>& piles, int h) {
    auto ok = [&](long long k) {
        long long hours = 0;
        for (int p : piles) hours += (p + k - 1) / k;
        return hours <= h;
    };
    long long hi = *std::max_element(piles.begin(), piles.end());
    return static_cast<int>(first_true(1, hi + 1, ok));
}

// ---- rotated arrays -------------------------------------------------------------------------
int search_rotated(const std::vector<int>& v, int t) {
    int lo = 0, hi = static_cast<int>(v.size()) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        std::size_t m = static_cast<std::size_t>(mid), l = static_cast<std::size_t>(lo), r = static_cast<std::size_t>(hi);
        if (v[m] == t) return mid;
        if (v[l] <= v[m]) {                                  // left half [lo, mid] is sorted
            if (v[l] <= t && t < v[m]) hi = mid - 1; else lo = mid + 1;
        } else {                                             // right half [mid, hi] is sorted
            if (v[m] < t && t <= v[r]) lo = mid + 1; else hi = mid - 1;
        }
    }
    return -1;
}
// Minimum of a rotated sorted array (distinct values): P(i) = v[i] <= v[n-1] is monotone.
int min_rotated(const std::vector<int>& v) {
    std::size_t lo = 0, hi = v.size() - 1;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (v[mid] <= v[v.size() - 1]) hi = mid;
        else lo = mid + 1;
    }
    return v[lo];
}

// ---- a peak element: move toward the larger neighbour; a peak always exists that way -------
int peak_index(const std::vector<int>& v) {
    std::size_t lo = 0, hi = v.size() - 1;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < v[mid + 1]) lo = mid + 1;   // ascending: peak is to the right
        else hi = mid;                            // descending or equal: peak at mid or left
    }
    return static_cast<int>(lo);
}

// ---- 2D matrix with rows sorted and each row's first > previous row's last ------------------
bool search_matrix(const std::vector<std::vector<int>>& m, int t) {
    if (m.empty() || m[0].empty()) return false;
    std::size_t rows = m.size(), cols = m[0].size();
    std::size_t lo = 0, hi = rows * cols;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        int x = m[mid / cols][mid % cols];
        if (x < t) lo = mid + 1; else hi = mid;
    }
    return lo < rows * cols && m[lo / cols][lo % cols] == t;
}

// ---- bisection on a real function: monthly rate r for a loan given payment ------------------
// Payment for principal P over n months at monthly rate r: P * r / (1 - (1+r)^-n). Monotone in r.
double solve_monthly_rate(double principal, int months, double payment) {
    auto pay_at = [&](double r) { return principal * r / (1.0 - std::pow(1.0 + r, -months)); };
    double lo = 1e-9, hi = 1.0;                  // 0 < r < 100% per month
    for (int it = 0; it < 100; ++it) {           // 100 halvings: far beyond double precision
        double mid = (lo + hi) / 2;
        if (pay_at(mid) < payment) lo = mid; else hi = mid;
    }
    return lo;
}

// ---- B-tree node: which child to descend into, and where a leaf cursor starts --------------
// Node with separator keys k[0..m) and children c[0..m]; child i covers [k[i-1], k[i]).
std::size_t child_index_for(const std::vector<int>& keys, int t) {
    return upper_bound(keys, t);          // first separator > t  ==  index of the child to follow
}
std::size_t leaf_scan_start(const std::vector<int>& leaf_keys, int t) {
    return lower_bound(leaf_keys, t);     // first key >= t: where a ">= t" range scan begins
}

}  // namespace

int main() {
    // ---- template vs std:: on random inputs with duplicates ----------------------------------
    std::mt19937 rng(5);
    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<int> v(rng() % 40);
        for (int& x : v) x = static_cast<int>(rng() % 20);
        std::sort(v.begin(), v.end());
        for (int t = -1; t <= 20; ++t) {
            assert(lower_bound(v, t) == static_cast<std::size_t>(std::lower_bound(v.begin(), v.end(), t) - v.begin()));
            assert(upper_bound(v, t) == static_cast<std::size_t>(std::upper_bound(v.begin(), v.end(), t) - v.begin()));
            assert(count_of(v, t) == std::count(v.begin(), v.end(), t));
        }
    }
    std::vector<int> v = {1, 2, 2, 2, 3, 5, 8};
    assert(first_occurrence(v, 2) == 1 && last_occurrence(v, 2) == 3 && count_of(v, 2) == 3);
    assert(first_occurrence(v, 4) == -1 && last_occurrence(v, 4) == -1);
    assert(lower_bound(v, 4) == 5 && upper_bound(v, 8) == 7);

    assert(isqrt(0) == 0 && isqrt(1) == 1 && isqrt(15) == 3 && isqrt(16) == 4);
    assert(isqrt(1'000'000'000'000'000'000LL) == 1'000'000'000LL);
    assert(isqrt(9223372036854775807LL) == 3037000499LL);

    assert(ship_within_days({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 5) == 15);
    assert(ship_within_days({3, 2, 2, 4, 1, 4}, 3) == 6);
    assert(min_eating_speed({3, 6, 7, 11}, 8) == 4);
    assert(min_eating_speed({30, 11, 23, 4, 20}, 5) == 30);

    std::vector<int> rot = {4, 5, 6, 7, 0, 1, 2};
    assert(search_rotated(rot, 0) == 4 && search_rotated(rot, 3) == -1 && search_rotated(rot, 4) == 0);
    assert(min_rotated(rot) == 0 && min_rotated({1, 2, 3}) == 1 && min_rotated({2, 1}) == 1);

    std::vector<int> pk = {1, 2, 1, 3, 5, 6, 4};
    int p = peak_index(pk);
    assert(p == 1 || p == 5);

    assert(search_matrix({{1, 3, 5, 7}, {10, 11, 16, 20}, {23, 30, 34, 60}}, 3));
    assert(!search_matrix({{1, 3, 5, 7}, {10, 11, 16, 20}, {23, 30, 34, 60}}, 13));

    // 100,000 over 12 months at 1% per month -> payment 8884.88; recover the rate.
    double r = solve_monthly_rate(100000.0, 12, 8884.878868);
    std::printf("recovered monthly rate: %.6f (expected 0.010000)\n", r);
    assert(std::fabs(r - 0.01) < 1e-6);

    // B-tree node with separators 10, 20, 30: children cover (-inf,10) [10,20) [20,30) [30,inf)
    std::vector<int> seps = {10, 20, 30};
    assert(child_index_for(seps, 5) == 0 && child_index_for(seps, 10) == 1 && child_index_for(seps, 25) == 2 &&
           child_index_for(seps, 30) == 3 && child_index_for(seps, 99) == 3);
    std::vector<int> leaf = {12, 14, 17};
    assert(leaf_scan_start(leaf, 13) == 1 && leaf_scan_start(leaf, 14) == 1 && leaf_scan_start(leaf, 18) == 3);

    // ---- benchmark: std::map::find vs lower_bound on a sorted vector, 1M keys ---------------
    {
        const int n = 1'000'000;
        std::vector<int> keys(static_cast<std::size_t>(n));
        for (int& k : keys) k = static_cast<int>(rng());
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::map<int, int> m;
        for (int k : keys) m.emplace(k, k);
        std::vector<int> queries = keys;
        std::shuffle(queries.begin(), queries.end(), rng);

        long long s1 = 0, s2 = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int q : queries) s1 += m.find(q)->second;
        double t_map = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        for (int q : queries) s2 += *std::lower_bound(keys.begin(), keys.end(), q);
        double t_vec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        assert(s1 == s2);
        std::printf("%zu random lookups: std::map %.0f ns/op, sorted vector + lower_bound %.0f ns/op\n",
                    queries.size(), t_map * 1e9 / static_cast<double>(queries.size()),
                    t_vec * 1e9 / static_cast<double>(queries.size()));
    }
    std::printf("all binary-search checks passed\n");
    return 0;
}
