// m08_sorting.cpp
// Insertion, selection, merge (top-down and bottom-up), quicksort (Lomuto, Hoare, three-way,
// median-of-three), heapsort, counting sort, LSD radix sort. All checked against std::sort,
// then timed on 1M ints. Also demonstrates quicksort's O(n^2) case with a bad pivot.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m08_sorting.cpp -o m08_sort

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

// ---- O(n^2) sorts -----------------------------------------------------------------------
// Insertion: prefix [0, i) is sorted; slide v[i] left into place. O(n) when already sorted.
void insertion_sort(std::vector<int>& v) {
    for (std::size_t i = 1; i < v.size(); ++i) {
        int x = v[i];
        std::size_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }   // shift, do not swap
        v[j] = x;
    }
}
// Selection: find the minimum of the unsorted suffix, swap it into place. Always O(n^2).
void selection_sort(std::vector<int>& v) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
        std::size_t m = i;
        for (std::size_t j = i + 1; j < v.size(); ++j) if (v[j] < v[m]) m = j;
        std::swap(v[i], v[m]);
    }
}

// ---- merge sort -------------------------------------------------------------------------
// Merge [lo, mid) and [mid, hi) using tmp as scratch. "<=" from the left keeps it stable.
void merge(std::vector<int>& v, std::vector<int>& tmp, std::size_t lo, std::size_t mid, std::size_t hi) {
    std::size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) tmp[k++] = (v[i] <= v[j]) ? v[i++] : v[j++];
    while (i < mid) tmp[k++] = v[i++];
    while (j < hi) tmp[k++] = v[j++];
    std::copy(tmp.begin() + static_cast<std::ptrdiff_t>(lo), tmp.begin() + static_cast<std::ptrdiff_t>(hi),
              v.begin() + static_cast<std::ptrdiff_t>(lo));
}
void merge_sort_rec(std::vector<int>& v, std::vector<int>& tmp, std::size_t lo, std::size_t hi) {
    if (hi - lo < 2) return;
    std::size_t mid = lo + (hi - lo) / 2;
    merge_sort_rec(v, tmp, lo, mid);
    merge_sort_rec(v, tmp, mid, hi);
    merge(v, tmp, lo, mid, hi);
}
void merge_sort(std::vector<int>& v) {
    std::vector<int> tmp(v.size());        // one scratch buffer for the whole sort
    merge_sort_rec(v, tmp, 0, v.size());
}
// Bottom-up: merge runs of width 1, 2, 4, ... No recursion; this is the shape external sort uses.
void merge_sort_bottom_up(std::vector<int>& v) {
    std::vector<int> tmp(v.size());
    std::size_t n = v.size();
    for (std::size_t width = 1; width < n; width *= 2)
        for (std::size_t lo = 0; lo + width < n; lo += 2 * width)
            merge(v, tmp, lo, lo + width, std::min(lo + 2 * width, n));
}

// ---- quicksort variants ------------------------------------------------------------------
// Lomuto: pivot at hi; i = boundary of "< pivot". Simple; many equal keys -> O(n^2).
int partition_lomuto(std::vector<int>& v, int lo, int hi) {
    int pivot = v[static_cast<std::size_t>(hi)];
    int i = lo;
    for (int j = lo; j < hi; ++j)
        if (v[static_cast<std::size_t>(j)] < pivot) std::swap(v[static_cast<std::size_t>(i++)], v[static_cast<std::size_t>(j)]);
    std::swap(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(hi)]);
    return i;                               // pivot's final position
}
void quicksort_lomuto_first_pivot(std::vector<int>& v, int lo, int hi) {
    // Deliberately bad: uses v[lo] as pivot (moved to hi for Lomuto). Sorted input -> O(n^2).
    while (lo < hi) {
        std::swap(v[static_cast<std::size_t>(lo)], v[static_cast<std::size_t>(hi)]);
        int p = partition_lomuto(v, lo, hi);
        if (p - lo < hi - p) { quicksort_lomuto_first_pivot(v, lo, p - 1); lo = p + 1; }
        else                 { quicksort_lomuto_first_pivot(v, p + 1, hi); hi = p - 1; }
    }
}

// Median of three: moves a reasonable pivot to v[lo + (hi-lo)/2] and returns its value.
int median_of_three(std::vector<int>& v, int lo, int hi) {
    std::size_t a = static_cast<std::size_t>(lo), m = static_cast<std::size_t>(lo + (hi - lo) / 2), b = static_cast<std::size_t>(hi);
    if (v[m] < v[a]) std::swap(v[m], v[a]);
    if (v[b] < v[a]) std::swap(v[b], v[a]);
    if (v[b] < v[m]) std::swap(v[b], v[m]);
    return v[m];
}
// Hoare: two scans meet in the middle; equal keys land on both sides. Returns j with
// [lo, j] <= pivot <= [j+1, hi].
int partition_hoare(std::vector<int>& v, int lo, int hi) {
    int pivot = median_of_three(v, lo, hi);
    int i = lo - 1, j = hi + 1;
    while (true) {
        do { ++i; } while (v[static_cast<std::size_t>(i)] < pivot);
        do { --j; } while (v[static_cast<std::size_t>(j)] > pivot);
        if (i >= j) return j;
        std::swap(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(j)]);
    }
}
void quicksort_hoare(std::vector<int>& v, int lo, int hi) {
    while (lo < hi) {
        int p = partition_hoare(v, lo, hi);
        // Recurse into the smaller part, iterate on the larger: O(log n) stack depth.
        if (p - lo < hi - p) { quicksort_hoare(v, lo, p); lo = p + 1; }
        else                 { quicksort_hoare(v, p + 1, hi); hi = p; }
    }
}
// Three-way (Dutch national flag): [lo, lt) < pivot, [lt, gt] == pivot, (gt, hi] > pivot.
void quicksort_3way(std::vector<int>& v, int lo, int hi) {
    while (lo < hi) {
        int pivot = median_of_three(v, lo, hi);
        int lt = lo, gt = hi, i = lo;
        while (i <= gt) {
            int x = v[static_cast<std::size_t>(i)];
            if (x < pivot)      std::swap(v[static_cast<std::size_t>(lt++)], v[static_cast<std::size_t>(i++)]);
            else if (x > pivot) std::swap(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(gt--)]);
            else                ++i;
        }
        // Equal block [lt, gt] is final; recurse on the smaller outer part.
        if (lt - lo < hi - gt) { quicksort_3way(v, lo, lt - 1); lo = gt + 1; }
        else                   { quicksort_3way(v, gt + 1, hi); hi = lt - 1; }
    }
}

// ---- heapsort -----------------------------------------------------------------------------
void sift_down(std::vector<int>& v, int i, int n) {
    while (true) {
        int largest = i, l = 2 * i + 1, r = l + 1;
        if (l < n && v[static_cast<std::size_t>(l)] > v[static_cast<std::size_t>(largest)]) largest = l;
        if (r < n && v[static_cast<std::size_t>(r)] > v[static_cast<std::size_t>(largest)]) largest = r;
        if (largest == i) return;
        std::swap(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(largest)]);
        i = largest;
    }
}
void heapsort(std::vector<int>& v) {
    int n = static_cast<int>(v.size());
    for (int i = n / 2 - 1; i >= 0; --i) sift_down(v, i, n);           // O(n) heapify
    for (int end = n - 1; end > 0; --end) {
        std::swap(v[0], v[static_cast<std::size_t>(end)]);              // max goes to its final slot
        sift_down(v, 0, end);
    }
}

// ---- non-comparison sorts ----------------------------------------------------------------
// Counting sort for keys in [0, k). Stable: place from the back using end positions.
void counting_sort(std::vector<int>& v, int k) {
    std::vector<std::size_t> count(static_cast<std::size_t>(k) + 1, 0);
    for (int x : v) ++count[static_cast<std::size_t>(x) + 1];
    for (int i = 0; i < k; ++i) count[static_cast<std::size_t>(i) + 1] += count[static_cast<std::size_t>(i)];
    std::vector<int> out(v.size());
    for (int x : v) out[count[static_cast<std::size_t>(x)]++] = x;   // count[x] = next free slot for key x
    v.swap(out);
}
// LSD radix sort on uint32: 4 stable passes of 8 bits each. O(4 * (n + 256)).
void radix_sort(std::vector<std::uint32_t>& v) {
    std::vector<std::uint32_t> tmp(v.size());
    for (int shift = 0; shift < 32; shift += 8) {
        std::array<std::size_t, 257> count{};
        for (std::uint32_t x : v) ++count[((x >> shift) & 0xFFu) + 1];
        for (std::size_t i = 0; i < 256; ++i) count[i + 1] += count[i];
        for (std::uint32_t x : v) tmp[count[(x >> shift) & 0xFFu]++] = x;
        v.swap(tmp);
    }
}

// ---- harness -------------------------------------------------------------------------------
using SortFn = std::function<void(std::vector<int>&)>;

double time_sort(const SortFn& f, std::vector<int> v) {
    auto t0 = std::chrono::steady_clock::now();
    f(v);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    assert(std::is_sorted(v.begin(), v.end()));
    return ms;
}

}  // namespace

int main() {
    std::mt19937 rng(123);
    std::vector<std::pair<std::string, SortFn>> sorts = {
        {"insertion", insertion_sort},
        {"selection", selection_sort},
        {"merge (top-down)", merge_sort},
        {"merge (bottom-up)", merge_sort_bottom_up},
        {"quick (hoare, med3)", [](std::vector<int>& v) { if (!v.empty()) quicksort_hoare(v, 0, static_cast<int>(v.size()) - 1); }},
        {"quick (3-way)", [](std::vector<int>& v) { if (!v.empty()) quicksort_3way(v, 0, static_cast<int>(v.size()) - 1); }},
        {"heapsort", heapsort},
        {"counting (k=1000)", [](std::vector<int>& v) { counting_sort(v, 1000); }},
        {"std::sort", [](std::vector<int>& v) { std::sort(v.begin(), v.end()); }},
        {"std::stable_sort", [](std::vector<int>& v) { std::stable_sort(v.begin(), v.end()); }},
    };

    // Correctness on many small random arrays, including duplicates-heavy and edge sizes.
    for (int trial = 0; trial < 300; ++trial) {
        std::size_t n = static_cast<std::size_t>(rng() % 50);
        std::vector<int> v(n);
        for (int& x : v) x = static_cast<int>(rng() % (trial % 2 ? 5 : 1000));   // many dups on odd trials
        std::vector<int> expect = v;
        std::sort(expect.begin(), expect.end());
        for (auto& [name, fn] : sorts) {
            std::vector<int> w = v;
            fn(w);
            if (w != expect) { std::printf("FAIL: %s\n", name.c_str()); return 1; }
        }
    }
    std::printf("all sorts agree with std::sort on 300 random inputs\n\n");

    // Timing on 1M random ints in [0, 1000). The O(n^2) sorts are run on 20k only.
    const std::size_t big_n = 1'000'000;
    std::vector<int> big(big_n);
    for (int& x : big) x = static_cast<int>(rng() % 1000);
    std::vector<int> small(big.begin(), big.begin() + 20'000);
    std::printf("%-22s %12s\n", "algorithm", "time");
    for (auto& [name, fn] : sorts) {
        bool quadratic = (name == "insertion" || name == "selection");
        double ms = time_sort(fn, quadratic ? small : big);
        std::printf("%-22s %9.2f ms  (n = %zu)\n", name.c_str(), ms, quadratic ? small.size() : big.size());
    }

    // Radix on uint32 for comparison with std::sort.
    {
        std::vector<std::uint32_t> r(big_n);
        for (std::uint32_t& x : r) x = rng();
        std::vector<std::uint32_t> s = r;
        auto t0 = std::chrono::steady_clock::now();
        radix_sort(r);
        double t_radix = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        std::sort(s.begin(), s.end());
        double t_std = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        assert(r == s);
        std::printf("%-22s %9.2f ms  (n = %zu, uint32)\n", "radix (LSD, 4x8 bit)", t_radix, big_n);
        std::printf("%-22s %9.2f ms  (n = %zu, uint32)\n\n", "std::sort", t_std, big_n);
    }

    // Quicksort worst case: first-element pivot on sorted input is O(n^2); median-of-three is not.
    {
        std::vector<int> sorted(50'000);
        for (std::size_t i = 0; i < sorted.size(); ++i) sorted[i] = static_cast<int>(i);
        std::vector<int> a = sorted, b = sorted;
        auto t0 = std::chrono::steady_clock::now();
        quicksort_lomuto_first_pivot(a, 0, static_cast<int>(a.size()) - 1);
        double t_bad = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        quicksort_hoare(b, 0, static_cast<int>(b.size()) - 1);
        double t_good = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        assert(std::is_sorted(a.begin(), a.end()) && std::is_sorted(b.begin(), b.end()));
        std::printf("sorted input, n=50000: first-element pivot %.1f ms (O(n^2)), median-of-three %.2f ms\n",
                    t_bad, t_good);
    }
    return 0;
}
