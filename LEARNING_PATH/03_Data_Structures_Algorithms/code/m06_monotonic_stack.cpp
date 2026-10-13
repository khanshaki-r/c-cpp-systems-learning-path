// m06_monotonic_stack.cpp
// Monotonic stack and deque problems: next greater element, daily temperatures, largest
// rectangle in a histogram (with end sentinel), trapping rain water, sliding window maximum.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m06_monotonic_stack.cpp -o m06_mono

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

// Next greater element to the right, or -1. Stack holds indices awaiting their answer, with
// values decreasing from bottom to top. Each index is pushed once and popped at most once.
std::vector<int> next_greater(const std::vector<int>& v) {
    int n = static_cast<int>(v.size());
    std::vector<int> ans(static_cast<std::size_t>(n), -1), st;
    for (int i = 0; i < n; ++i) {
        while (!st.empty() && v[static_cast<std::size_t>(st.back())] < v[static_cast<std::size_t>(i)]) {
            ans[static_cast<std::size_t>(st.back())] = v[static_cast<std::size_t>(i)];
            st.pop_back();
        }
        st.push_back(i);
    }
    return ans;
}

// Days until a warmer temperature. Same stack; answer is an index difference.
std::vector<int> daily_temperatures(const std::vector<int>& t) {
    int n = static_cast<int>(t.size());
    std::vector<int> ans(static_cast<std::size_t>(n), 0), st;
    for (int i = 0; i < n; ++i) {
        while (!st.empty() && t[static_cast<std::size_t>(st.back())] < t[static_cast<std::size_t>(i)]) {
            ans[static_cast<std::size_t>(st.back())] = i - st.back();
            st.pop_back();
        }
        st.push_back(i);
    }
    return ans;
}

// Largest rectangle in a histogram. Increasing stack of indices. When a bar shorter than the
// top arrives, the top's rectangle is bounded on the right by i and on the left by the new
// top. The virtual 0-height bar at i == n flushes everything.
long long largest_rectangle(const std::vector<int>& h) {
    int n = static_cast<int>(h.size());
    std::vector<int> st;
    long long best = 0;
    for (int i = 0; i <= n; ++i) {
        int cur = (i == n) ? 0 : h[static_cast<std::size_t>(i)];
        while (!st.empty() && h[static_cast<std::size_t>(st.back())] >= cur) {
            int height = h[static_cast<std::size_t>(st.back())];
            st.pop_back();
            int left = st.empty() ? -1 : st.back();
            best = std::max(best, static_cast<long long>(height) * (i - left - 1));
        }
        st.push_back(i);
    }
    return best;
}

// Trapping rain water with a decreasing stack: when a taller bar arrives, the popped bar is
// the floor of a pool bounded by the new stack top (left wall) and i (right wall).
long long trap(const std::vector<int>& h) {
    std::vector<int> st;
    long long water = 0;
    for (int i = 0; i < static_cast<int>(h.size()); ++i) {
        while (!st.empty() && h[static_cast<std::size_t>(st.back())] < h[static_cast<std::size_t>(i)]) {
            int floor = st.back();
            st.pop_back();
            if (st.empty()) break;                               // no left wall
            int left = st.back();
            int width = i - left - 1;
            int depth = std::min(h[static_cast<std::size_t>(left)], h[static_cast<std::size_t>(i)]) -
                        h[static_cast<std::size_t>(floor)];
            water += static_cast<long long>(width) * depth;
        }
        st.push_back(i);
    }
    return water;
}

// Sliding window maximum with a deque of indices holding decreasing values.
std::vector<int> max_sliding_window(const std::vector<int>& v, int k) {
    std::deque<int> dq;
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        while (!dq.empty() && v[static_cast<std::size_t>(dq.back())] <= v[static_cast<std::size_t>(i)])
            dq.pop_back();                                       // dominated by v[i]: never the max again
        dq.push_back(i);
        if (dq.front() <= i - k) dq.pop_front();                 // slid out of the window
        if (i >= k - 1) out.push_back(v[static_cast<std::size_t>(dq.front())]);
    }
    return out;
}

void print(const char* label, const std::vector<int>& v) {
    std::printf("%-24s", label);
    for (int x : v) std::printf("%d ", x);
    std::printf("\n");
}

}  // namespace

int main() {
    auto ng = next_greater({2, 1, 2, 4, 3});
    print("next_greater", ng);
    assert((ng == std::vector<int>{4, 2, 4, -1, -1}));

    auto dt = daily_temperatures({73, 74, 75, 71, 69, 72, 76, 73});
    print("daily_temperatures", dt);
    assert((dt == std::vector<int>{1, 1, 4, 2, 1, 1, 0, 0}));

    assert(largest_rectangle({2, 1, 5, 6, 2, 3}) == 10);
    assert(largest_rectangle({2, 4}) == 4);
    assert(largest_rectangle({1, 1, 1, 1}) == 4);
    std::printf("largest_rectangle       = %lld\n", largest_rectangle({2, 1, 5, 6, 2, 3}));

    assert(trap({0, 1, 0, 2, 1, 0, 1, 3, 2, 1, 2, 1}) == 6);
    assert(trap({4, 2, 0, 3, 2, 5}) == 9);

    auto mw = max_sliding_window({1, 3, -1, -3, 5, 3, 6, 7}, 3);
    print("max_sliding_window(3)", mw);
    assert((mw == std::vector<int>{3, 3, 5, 5, 6, 7}));

    std::printf("all monotonic-stack checks passed\n");
    return 0;
}
