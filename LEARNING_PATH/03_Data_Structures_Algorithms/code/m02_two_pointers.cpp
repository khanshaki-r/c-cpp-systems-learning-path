// m02_two_pointers.cpp
// Two-pointer problems: two-sum on sorted, remove duplicates, move zeroes, container with
// most water, merge sorted arrays from the back, rotate by three reversals, sorted squares.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_two_pointers.cpp -o m02_tp

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

// 1. Two Sum on sorted input. Each step discards one index for good, so O(n).
std::pair<int, int> two_sum_sorted(const std::vector<int>& v, int target) {
    int l = 0, r = static_cast<int>(v.size()) - 1;
    while (l < r) {
        long long s = static_cast<long long>(v[l]) + v[r];  // widen before adding
        if (s == target) return {l, r};
        if (s < target) ++l;   // only moving l can increase the sum
        else --r;              // only moving r can decrease it
    }
    return {-1, -1};
}

// 2. Remove duplicates in place from sorted array. v[0..write) is the unique prefix.
int remove_duplicates(std::vector<int>& v) {
    if (v.empty()) return 0;
    int write = 1;
    for (int read = 1; read < static_cast<int>(v.size()); ++read)
        if (v[read] != v[write - 1]) v[write++] = v[read];
    return write;
}

// 3. Move zeroes to the end keeping order of non-zeroes. Swap keeps it to one pass and
//    never writes a non-zero twice.
void move_zeroes(std::vector<int>& v) {
    int write = 0;
    for (int read = 0; read < static_cast<int>(v.size()); ++read)
        if (v[read] != 0) std::swap(v[write++], v[read]);
}

// 4. Container with most water. Always move the shorter wall: the taller one cannot be the
//    limiting factor of any better container with the current shorter wall.
long long max_area(const std::vector<int>& h) {
    int l = 0, r = static_cast<int>(h.size()) - 1;
    long long best = 0;
    while (l < r) {
        long long area = static_cast<long long>(r - l) * std::min(h[l], h[r]);
        best = std::max(best, area);
        if (h[l] < h[r]) ++l;
        else --r;
    }
    return best;
}

// 5. Merge sorted b into a, where a has size m + b.size() and a[0..m) holds data.
//    Filling from the back means we never overwrite an unread element of a.
void merge_into(std::vector<int>& a, int m, const std::vector<int>& b) {
    int i = m - 1, j = static_cast<int>(b.size()) - 1, w = m + static_cast<int>(b.size()) - 1;
    while (j >= 0) {                     // once b is exhausted, the rest of a is already in place
        if (i >= 0 && a[i] > b[j]) a[w--] = a[i--];
        else a[w--] = b[j--];
    }
}

// 6. Rotate right by k with three reversals. reverse(all), reverse(first k), reverse(rest).
void rotate_right(std::vector<int>& v, int k) {
    int n = static_cast<int>(v.size());
    if (n == 0) return;
    k %= n;
    std::reverse(v.begin(), v.end());
    std::reverse(v.begin(), v.begin() + k);
    std::reverse(v.begin() + k, v.end());
}

// 7. Squares of a sorted array (with negatives) in sorted order. The largest square is at one
//    of the two ends; fill the output from the back.
std::vector<int> sorted_squares(const std::vector<int>& v) {
    int n = static_cast<int>(v.size());
    std::vector<int> out(static_cast<std::size_t>(n));
    int l = 0, r = n - 1;
    for (int w = n - 1; w >= 0; --w) {
        if (std::abs(v[l]) > std::abs(v[r])) { out[static_cast<std::size_t>(w)] = v[l] * v[l]; ++l; }
        else                                 { out[static_cast<std::size_t>(w)] = v[r] * v[r]; --r; }
    }
    return out;
}

void print(const char* label, const std::vector<int>& v, int upto = -1) {
    std::printf("%-26s", label);
    int n = upto < 0 ? static_cast<int>(v.size()) : upto;
    for (int i = 0; i < n; ++i) std::printf("%d ", v[static_cast<std::size_t>(i)]);
    std::printf("\n");
}

}  // namespace

int main() {
    {
        std::vector<int> v = {1, 2, 4, 7, 11, 15};
        auto [i, j] = two_sum_sorted(v, 15);   // structured binding: unpack the pair
        std::printf("two_sum_sorted -> (%d, %d)\n", i, j);
        assert(i == 1 && j == 4);
    }
    {
        std::vector<int> v = {0, 0, 1, 1, 1, 2, 2, 3, 3, 4};
        int k = remove_duplicates(v);
        print("remove_duplicates", v, k);
        assert(k == 5);
    }
    {
        std::vector<int> v = {0, 1, 0, 3, 12};
        move_zeroes(v);
        print("move_zeroes", v);
        assert((v == std::vector<int>{1, 3, 12, 0, 0}));
    }
    {
        std::vector<int> h = {1, 8, 6, 2, 5, 4, 8, 3, 7};
        long long a = max_area(h);
        std::printf("max_area -> %lld\n", a);
        assert(a == 49);
    }
    {
        std::vector<int> a = {1, 3, 5, 0, 0, 0};
        std::vector<int> b = {2, 4, 6};
        merge_into(a, 3, b);
        print("merge_into", a);
        assert((a == std::vector<int>{1, 2, 3, 4, 5, 6}));
    }
    {
        std::vector<int> v = {1, 2, 3, 4, 5, 6, 7};
        rotate_right(v, 3);
        print("rotate_right(3)", v);
        assert((v == std::vector<int>{5, 6, 7, 1, 2, 3, 4}));
    }
    {
        std::vector<int> v = {-4, -1, 0, 3, 10};
        auto s = sorted_squares(v);
        print("sorted_squares", s);
        assert((s == std::vector<int>{0, 1, 9, 16, 100}));
    }
    std::printf("all two-pointer checks passed\n");
    return 0;
}
