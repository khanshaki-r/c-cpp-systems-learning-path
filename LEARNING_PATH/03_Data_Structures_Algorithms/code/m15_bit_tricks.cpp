// m15_bit_tricks.cpp
// Bit manipulation you are expected to know cold: test/set/clear/toggle, lowest set bit,
// power of two, popcount three ways, counting bits 0..n, single number (XOR) and single
// number II (bit counting), missing number, reverse bits, subset enumeration with masks,
// submask enumeration, Gray code, std::bitset and a sieve, a hand-written dynamic bitset,
// and a bitmask DP (travelling salesman on a few cities).
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m15_bit_tricks.cpp -o m15_bit_tricks

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>

// Always do bit work on UNSIGNED types: right-shifting a negative signed value is
// implementation-defined before C++20, and left-shifting into the sign bit is undefined.

constexpr bool test_bit(std::uint32_t x, int i) { return (x >> i) & 1u; }
constexpr std::uint32_t set_bit(std::uint32_t x, int i) { return x | (1u << i); }
constexpr std::uint32_t clear_bit(std::uint32_t x, int i) { return x & ~(1u << i); }
constexpr std::uint32_t toggle_bit(std::uint32_t x, int i) { return x ^ (1u << i); }

constexpr std::uint32_t lowest_set(std::uint32_t x) { return x & (~x + 1u); }   // x & -x, written unsigned-safe
constexpr std::uint32_t clear_lowest(std::uint32_t x) { return x & (x - 1); }
constexpr bool is_pow2(std::uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

// Popcount, three ways.
int popcount_loop(std::uint32_t x) {             // O(32)
    int c = 0;
    for (; x; x >>= 1) c += static_cast<int>(x & 1u);
    return c;
}
int popcount_kernighan(std::uint32_t x) {        // O(number of set bits)
    int c = 0;
    for (; x; x &= x - 1) ++c;                   // each step clears the lowest 1
    return c;
}
int popcount_builtin(std::uint32_t x) {          // one POPCNT instruction with -mpopcnt / -march=native
    return __builtin_popcount(x);
}

// Count bits for every i in 0..n: bits[i] = bits[i >> 1] + (i & 1). O(n). A tiny DP.
std::vector<int> count_bits(int n) {
    std::vector<int> b(n + 1, 0);
    for (int i = 1; i <= n; ++i) b[i] = b[i >> 1] + (i & 1);
    return b;
}

// Every number appears twice except one: XOR everything. a ^ a = 0, a ^ 0 = a, order irrelevant.
int single_number(const std::vector<int>& a) {
    int x = 0;
    for (int v : a) x ^= v;
    return x;
}

// Every number appears three times except one: count each bit position mod 3.
int single_number_ii(const std::vector<int>& a) {
    std::uint32_t result = 0;
    for (int b = 0; b < 32; ++b) {
        int cnt = 0;
        for (int v : a) cnt += static_cast<int>((static_cast<std::uint32_t>(v) >> b) & 1u);
        if (cnt % 3) result |= (1u << b);
    }
    return static_cast<int>(result);             // two's complement round-trip, well-defined since C++20 and in practice on GCC
}

// 0..n with one missing: XOR of indices and values cancels everything except the missing one.
int missing_number(const std::vector<int>& a) {
    int x = static_cast<int>(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) x ^= static_cast<int>(i) ^ a[i];
    return x;
}

std::uint32_t reverse_bits(std::uint32_t x) {
    // Swap halves, then quarters, ... (divide and conquer, 5 steps instead of 32).
    x = (x >> 16) | (x << 16);
    x = ((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8);
    x = ((x & 0xF0F0F0F0u) >> 4) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x & 0xCCCCCCCCu) >> 2) | ((x & 0x33333333u) << 2);
    x = ((x & 0xAAAAAAAAu) >> 1) | ((x & 0x55555555u) << 1);
    return x;
}

// All subsets of n items as masks 0 .. 2^n - 1. Bit i set = item i included.
std::vector<std::vector<int>> subsets(const std::vector<int>& items) {
    std::size_t n = items.size();
    std::vector<std::vector<int>> out;
    for (std::uint32_t mask = 0; mask < (1u << n); ++mask) {
        std::vector<int> s;
        for (std::size_t i = 0; i < n; ++i)
            if (mask & (1u << i)) s.push_back(items[i]);
        out.push_back(std::move(s));
    }
    return out;
}

// All submasks of m, largest first, including 0. (sub - 1) & m steps to the next smaller submask.
std::vector<std::uint32_t> submasks(std::uint32_t m) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t s = m;; s = (s - 1) & m) {
        out.push_back(s);
        if (s == 0) break;
    }
    return out;
}

// Gray code: consecutive values differ in exactly one bit. g(i) = i ^ (i >> 1).
std::vector<std::uint32_t> gray_code(int n) {
    std::vector<std::uint32_t> g;
    for (std::uint32_t i = 0; i < (1u << n); ++i) g.push_back(i ^ (i >> 1));
    return g;
}

// Sieve of Eratosthenes on std::bitset: N bits = N/8 bytes, 8x denser than vector<char>.
template <std::size_t N>
std::size_t count_primes_below() {
    static std::bitset<N> composite;             // static: a large bitset would blow the stack
    composite.reset();
    composite[0] = composite[1] = true;
    for (std::size_t i = 2; i * i < N; ++i)
        if (!composite[i])
            for (std::size_t j = i * i; j < N; j += i) composite[j] = true;
    return N - composite.count();
}

// A dynamic bitset (size chosen at runtime): 64 flags per word, popcount per word.
// Think "which of 10 million account ids appeared in today's batch": 1.25 MB instead of 10 MB
// for vector<char> or ~400 MB for unordered_set<int>.
class DynBitset {
public:
    explicit DynBitset(std::size_t n) : n_(n), w_((n + 63) / 64, 0) {}
    void set(std::size_t i) { w_[i >> 6] |= (std::uint64_t{1} << (i & 63)); }
    void reset(std::size_t i) { w_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
    bool test(std::size_t i) const { return (w_[i >> 6] >> (i & 63)) & 1u; }
    std::size_t count() const {
        std::size_t c = 0;
        for (std::uint64_t x : w_) c += static_cast<std::size_t>(__builtin_popcountll(x));
        return c;
    }
    // Set intersection in n/64 word operations.
    DynBitset& operator&=(const DynBitset& o) {
        for (std::size_t k = 0; k < w_.size(); ++k) w_[k] &= o.w_[k];
        return *this;
    }
    // Next set bit at or after i, or n if none: skip whole zero words, then count trailing zeros.
    std::size_t next(std::size_t i) const {
        if (i >= n_) return n_;
        std::size_t k = i >> 6;
        std::uint64_t word = w_[k] & (~std::uint64_t{0} << (i & 63));
        while (word == 0) {
            if (++k == w_.size()) return n_;
            word = w_[k];
        }
        return std::min(n_, k * 64 + static_cast<std::size_t>(__builtin_ctzll(word)));
    }

private:
    std::size_t n_;
    std::vector<std::uint64_t> w_;
};

// Bitmask DP: shortest Hamiltonian cycle (TSP) starting and ending at city 0.
// dp[mask][v] = cheapest path that starts at 0, visits exactly the cities in mask, ends at v.
// O(2^n * n^2): fine for n <= ~16, hopeless beyond ~25.
int tsp(const std::vector<std::vector<int>>& d) {
    int n = static_cast<int>(d.size());
    const int INF = std::numeric_limits<int>::max() / 2;
    std::vector<std::vector<int>> dp(1u << n, std::vector<int>(n, INF));
    dp[1][0] = 0;
    for (std::uint32_t mask = 1; mask < (1u << n); ++mask)
        for (int v = 0; v < n; ++v) {
            if (dp[mask][v] >= INF || !(mask & (1u << v))) continue;
            for (int u = 0; u < n; ++u) {
                if (mask & (1u << u)) continue;
                std::uint32_t nm = mask | (1u << u);
                dp[nm][u] = std::min(dp[nm][u], dp[mask][v] + d[v][u]);
            }
        }
    int best = INF;
    for (int v = 1; v < n; ++v) best = std::min(best, dp[(1u << n) - 1][v] + d[v][0]);
    return n == 1 ? 0 : best;
}

int main() {
    static_assert(test_bit(0b1010, 1) && !test_bit(0b1010, 0), "test_bit");
    static_assert(set_bit(0b1000, 0) == 0b1001, "set_bit");
    static_assert(clear_bit(0b1011, 1) == 0b1001, "clear_bit");
    static_assert(toggle_bit(0b1011, 3) == 0b0011, "toggle_bit");
    static_assert(lowest_set(0b101000) == 0b1000, "lowest_set");
    static_assert(clear_lowest(0b101000) == 0b100000, "clear_lowest");
    static_assert(is_pow2(1) && is_pow2(1024) && !is_pow2(0) && !is_pow2(12), "is_pow2");

    std::mt19937 rng(1);
    for (int i = 0; i < 100000; ++i) {
        std::uint32_t x = static_cast<std::uint32_t>(rng());
        int p = popcount_loop(x);
        assert(popcount_kernighan(x) == p && popcount_builtin(x) == p);
        assert(static_cast<int>(std::bitset<32>(x).count()) == p);
        assert(reverse_bits(reverse_bits(x)) == x);
    }
    assert(reverse_bits(1u) == 0x80000000u);
    assert(reverse_bits(0b1011u) == 0xD0000000u);

    assert((count_bits(5) == std::vector<int>{0, 1, 1, 2, 1, 2}));
    assert(single_number({4, 1, 2, 1, 2}) == 4);
    assert(single_number_ii({2, 2, 3, 2}) == 3);
    assert(single_number_ii({0, 1, 0, 1, 0, 1, -99}) == -99);
    assert(missing_number({3, 0, 1}) == 2);
    assert(missing_number({9, 6, 4, 2, 3, 5, 7, 0, 1}) == 8);

    auto ss = subsets({1, 2, 3});
    assert(ss.size() == 8 && ss[0].empty() && (ss[5] == std::vector<int>{1, 3}));   // mask 101
    assert((submasks(0b101) == std::vector<std::uint32_t>{0b101, 0b100, 0b001, 0}));

    auto g = gray_code(4);
    for (std::size_t i = 1; i < g.size(); ++i) assert(popcount_builtin(g[i] ^ g[i - 1]) == 1);
    std::vector<std::uint32_t> sorted_g = g;
    std::sort(sorted_g.begin(), sorted_g.end());
    for (std::uint32_t i = 0; i < 16; ++i) assert(sorted_g[i] == i);   // a permutation of 0..15

    assert(count_primes_below<100>() == 25);
    assert(count_primes_below<1000000>() == 78498);

    DynBitset a(1000), b(1000);
    for (std::size_t i = 0; i < 1000; i += 3) a.set(i);
    for (std::size_t i = 0; i < 1000; i += 5) b.set(i);
    assert(a.count() == 334 && b.count() == 200);
    a &= b;                                                  // multiples of 15
    assert(a.count() == 67 && a.test(15) && !a.test(3));
    assert(a.next(1) == 15 && a.next(16) == 30 && a.next(991) == 1000);
    a.reset(15);
    assert(a.next(1) == 30);

    assert(tsp({{0, 10, 15, 20}, {10, 0, 35, 25}, {15, 35, 0, 30}, {20, 25, 30, 0}}) == 80);

    std::printf("all bit-manipulation checks passed\n");
    return 0;
}
