// m07_memo_fib.cpp
// Naive vs memoised vs iterative Fibonacci (with call counts and timing), climbing stairs,
// unique grid paths with a 2D memo, and a recursion -> explicit-stack conversion of a tree
// walk that survives depth 1,000,000.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m07_memo_fib.cpp -o m07_memo

#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

long long fib_naive(int n, long long& calls) {
    ++calls;
    if (n < 2) return n;
    return fib_naive(n - 1, calls) + fib_naive(n - 2, calls);   // two branches: ~1.618^n calls
}

long long fib_memo(int n, std::vector<long long>& memo, long long& calls) {
    ++calls;
    if (n < 2) return n;
    std::size_t k = static_cast<std::size_t>(n);
    if (memo[k] != -1) return memo[k];                            // already computed: O(1)
    return memo[k] = fib_memo(n - 1, memo, calls) + fib_memo(n - 2, memo, calls);
}

long long fib_iter(int n) {
    long long a = 0, b = 1;                                       // fib(i-1), fib(i)
    for (int i = 0; i < n; ++i) { long long c = a + b; a = b; b = c; }
    return a;
}

// Climbing stairs: ways to reach step n with steps of 1 or 2. Same recurrence as Fibonacci.
long long climb(int n, std::vector<long long>& memo) {
    if (n <= 1) return 1;
    std::size_t k = static_cast<std::size_t>(n);
    if (memo[k]) return memo[k];
    return memo[k] = climb(n - 1, memo) + climb(n - 2, memo);
}

// Unique paths in an r x c grid moving only right or down. Without memo: C(r+c-2, r-1) calls,
// exponential. With memo: r*c states, O(1) each.
long long grid_paths(int r, int c, std::vector<std::vector<long long>>& memo) {
    if (r == 1 || c == 1) return 1;
    long long& slot = memo[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
    if (slot) return slot;
    return slot = grid_paths(r - 1, c, memo) + grid_paths(r, c - 1, memo);
}

// ---- recursion to explicit stack ---------------------------------------------------------
struct Node {
    int val;
    std::unique_ptr<Node> left, right;
};

// Recursive sum: depth = tree height. On a degenerate (path-shaped) tree of 1e6 nodes this
// would overflow the stack, so we do not call it on that tree.
long long sum_recursive(const Node* n) {
    if (!n) return 0;
    return n->val + sum_recursive(n->left.get()) + sum_recursive(n->right.get());
}

// Iterative: the vector IS the call stack, but on the heap, so depth is bounded by memory.
long long sum_iterative(const Node* root) {
    long long total = 0;
    std::vector<const Node*> stack;
    if (root) stack.push_back(root);
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        total += n->val;
        if (n->right) stack.push_back(n->right.get());
        if (n->left) stack.push_back(n->left.get());
    }
    return total;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
    for (int n : {10, 20, 30}) {
        long long calls_naive = 0, calls_memo = 0;
        auto t0 = std::chrono::steady_clock::now();
        long long a = fib_naive(n, calls_naive);
        double t_naive = ms_since(t0);

        std::vector<long long> memo(static_cast<std::size_t>(n) + 1, -1);
        t0 = std::chrono::steady_clock::now();
        long long b = fib_memo(n, memo, calls_memo);
        double t_memo = ms_since(t0);

        long long c = fib_iter(n);
        assert(a == b && b == c);
        std::printf("fib(%2d) = %8lld | naive: %10lld calls %8.3f ms | memo: %3lld calls %.3f ms\n",
                    n, a, calls_naive, t_naive, calls_memo, t_memo);
    }
    assert(fib_iter(90) == 2880067194370816120LL);

    std::vector<long long> stairs(46, 0);
    assert(climb(45, stairs) == 1836311903LL);

    std::vector<std::vector<long long>> gp(20, std::vector<long long>(20, 0));
    assert(grid_paths(3, 7, gp) == 28);
    assert(grid_paths(18, 18, gp) == 2333606220LL);

    // Build a path-shaped tree of depth 1,000,000: every node has only a right child.
    const int depth = 1'000'000;
    auto root = std::make_unique<Node>(Node{1, nullptr, nullptr});
    Node* cur = root.get();
    for (int i = 2; i <= depth; ++i) {
        cur->right = std::make_unique<Node>(Node{1, nullptr, nullptr});
        cur = cur->right.get();
    }
    assert(sum_iterative(root.get()) == depth);
    std::printf("iterative DFS summed a depth-%d tree; recursive would have overflowed the stack\n", depth);

    // A small balanced-ish tree where recursion is fine, to show both agree.
    auto small = std::make_unique<Node>(Node{1, nullptr, nullptr});
    small->left = std::make_unique<Node>(Node{2, nullptr, nullptr});
    small->right = std::make_unique<Node>(Node{3, nullptr, nullptr});
    small->left->left = std::make_unique<Node>(Node{4, nullptr, nullptr});
    assert(sum_recursive(small.get()) == 10 && sum_iterative(small.get()) == 10);

    // Destroy the deep tree iteratively: the default recursive destructor would overflow too.
    while (root) root = std::move(root->right);

    std::printf("all memo/recursion checks passed\n");
    return 0;
}
