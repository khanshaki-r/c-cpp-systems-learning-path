// m10_bst.cpp
// A binary search tree owning its nodes with std::unique_ptr: insert, contains, min/max,
// erase (all three cases), height, size, in-order, validate, LCA, k-th smallest, build from a
// sorted array, serialise/deserialise (pre-order with '#'), iterative destructor. Plus an
// experiment showing the height of sorted vs shuffled insertion.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m10_bst.cpp -o m10_bst

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

class BST {
    struct Node {
        int key;
        std::unique_ptr<Node> left, right;
    };

public:
    BST() = default;
    BST(const BST&) = delete;
    BST& operator=(const BST&) = delete;

    // Post-order destruction with an explicit stack: a degenerate tree of 1e6 nodes would
    // overflow the call stack if we let unique_ptr's recursive destructor run.
    ~BST() {
        std::vector<std::unique_ptr<Node>> st;
        if (root_) st.push_back(std::move(root_));
        while (!st.empty()) {
            std::unique_ptr<Node> n = std::move(st.back());
            st.pop_back();
            if (n->left) st.push_back(std::move(n->left));
            if (n->right) st.push_back(std::move(n->right));
        }   // n dies here with both children already detached
    }

    std::size_t size() const { return size_; }

    bool insert(int key) {
        std::unique_ptr<Node>* link = &root_;        // the owner slot we will attach to
        while (*link) {
            if (key == (*link)->key) return false;   // no duplicates
            link = key < (*link)->key ? &(*link)->left : &(*link)->right;
        }
        *link = std::make_unique<Node>(Node{key, nullptr, nullptr});
        ++size_;
        return true;
    }

    bool contains(int key) const {
        const Node* n = root_.get();
        while (n && n->key != key) n = key < n->key ? n->left.get() : n->right.get();
        return n != nullptr;
    }

    std::optional<int> min() const {
        if (!root_) return std::nullopt;
        const Node* n = root_.get();
        while (n->left) n = n->left.get();
        return n->key;
    }
    std::optional<int> max() const {
        if (!root_) return std::nullopt;
        const Node* n = root_.get();
        while (n->right) n = n->right.get();
        return n->key;
    }

    bool erase(int key) {
        std::unique_ptr<Node>* link = &root_;
        while (*link && (*link)->key != key)
            link = key < (*link)->key ? &(*link)->left : &(*link)->right;
        if (!*link) return false;
        Node* n = link->get();
        if (n->left && n->right) {
            // Case 3: two children. Successor = leftmost node of the right subtree. Copy its
            // key up, then unlink the successor (it has no left child, so case 1 or 2 applies).
            std::unique_ptr<Node>* succ = &n->right;
            while ((*succ)->left) succ = &(*succ)->left;
            n->key = (*succ)->key;
            *succ = std::move((*succ)->right);
        } else {
            // Case 1 (leaf) and case 2 (one child): the owner slot takes the child (or null).
            // Move into a temporary first: *link owns n, and n owns the child we are moving.
            std::unique_ptr<Node> child = std::move(n->left ? n->left : n->right);
            *link = std::move(child);
        }
        --size_;
        return true;
    }

    int height() const { return height(root_.get()); }

    // -1 if unbalanced anywhere, else the height. One post-order pass.
    int balanced_height() const { return balanced_height(root_.get()); }

    std::vector<int> inorder() const {
        std::vector<int> out;
        out.reserve(size_);
        std::vector<const Node*> st;
        const Node* cur = root_.get();
        while (cur || !st.empty()) {
            while (cur) { st.push_back(cur); cur = cur->left.get(); }
            cur = st.back();
            st.pop_back();
            out.push_back(cur->key);
            cur = cur->right.get();
        }
        return out;
    }

    bool valid() const { return valid(root_.get(), LLONG_MIN_, LLONG_MAX_); }

    // LCA of two keys known to be present: walk down until they split.
    std::optional<int> lca(int a, int b) const {
        const Node* n = root_.get();
        while (n) {
            if (a < n->key && b < n->key) n = n->left.get();
            else if (a > n->key && b > n->key) n = n->right.get();
            else return n->key;
        }
        return std::nullopt;
    }

    // k-th smallest (1-based) by in-order walk with early stop: O(h + k).
    std::optional<int> kth_smallest(std::size_t k) const {
        std::vector<const Node*> st;
        const Node* cur = root_.get();
        while (cur || !st.empty()) {
            while (cur) { st.push_back(cur); cur = cur->left.get(); }
            cur = st.back();
            st.pop_back();
            if (--k == 0) return cur->key;
            cur = cur->right.get();
        }
        return std::nullopt;
    }

    // Balanced build from a sorted range: middle becomes the root. O(n).
    static BST from_sorted(const std::vector<int>& v) {
        BST t;
        t.root_ = build(v, 0, v.size());
        t.size_ = v.size();
        return t;
    }

    // Pre-order with '#' for null: uniquely reconstructible.
    std::string serialise() const {
        std::string out;
        serialise(root_.get(), out);
        return out;
    }
    static BST deserialise(const std::string& s) {
        std::istringstream in(s);
        BST t;
        t.root_ = deserialise(in, t.size_);
        return t;
    }

    BST(BST&&) noexcept = default;
    BST& operator=(BST&&) noexcept = default;

private:
    static constexpr long long LLONG_MIN_ = -(1LL << 62), LLONG_MAX_ = (1LL << 62);

    static int height(const Node* n) {
        if (!n) return -1;
        return 1 + std::max(height(n->left.get()), height(n->right.get()));
    }
    static int balanced_height(const Node* n) {
        if (!n) return 0;
        int l = balanced_height(n->left.get());
        if (l < 0) return -1;
        int r = balanced_height(n->right.get());
        if (r < 0) return -1;
        if (std::abs(l - r) > 1) return -1;
        return 1 + std::max(l, r);
    }
    static bool valid(const Node* n, long long lo, long long hi) {
        if (!n) return true;
        if (n->key <= lo || n->key >= hi) return false;        // must lie strictly inside (lo, hi)
        return valid(n->left.get(), lo, n->key) && valid(n->right.get(), n->key, hi);
    }
    static std::unique_ptr<Node> build(const std::vector<int>& v, std::size_t lo, std::size_t hi) {
        if (lo >= hi) return nullptr;
        std::size_t mid = lo + (hi - lo) / 2;
        auto n = std::make_unique<Node>(Node{v[mid], nullptr, nullptr});
        n->left = build(v, lo, mid);
        n->right = build(v, mid + 1, hi);
        return n;
    }
    static void serialise(const Node* n, std::string& out) {
        if (!n) { out += "# "; return; }
        out += std::to_string(n->key);
        out += ' ';
        serialise(n->left.get(), out);
        serialise(n->right.get(), out);
    }
    static std::unique_ptr<Node> deserialise(std::istringstream& in, std::size_t& count) {
        std::string tok;
        if (!(in >> tok) || tok == "#") return nullptr;
        auto n = std::make_unique<Node>(Node{std::stoi(tok), nullptr, nullptr});
        ++count;
        n->left = deserialise(in, count);
        n->right = deserialise(in, count);
        return n;
    }

    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
};

int main() {
    BST t;
    for (int k : {50, 30, 70, 20, 40, 60, 80, 35, 45, 65}) assert(t.insert(k));
    assert(!t.insert(50));
    assert(t.size() == 10 && t.contains(45) && !t.contains(99));
    assert(t.min() == 20 && t.max() == 80);
    assert((t.inorder() == std::vector<int>{20, 30, 35, 40, 45, 50, 60, 65, 70, 80}));
    assert(t.valid() && t.height() == 3 && t.balanced_height() == 4);
    assert(t.lca(35, 45) == 40 && t.lca(20, 80) == 50 && t.lca(60, 80) == 70);
    assert(t.kth_smallest(1) == 20 && t.kth_smallest(6) == 50 && !t.kth_smallest(11).has_value());

    // Delete: leaf, one child, two children (root), missing.
    assert(t.erase(20));                     // leaf
    assert(t.erase(70));                     // two children (60, 80) -> successor 80
    assert(t.erase(60));                     // one child (65)
    assert(t.erase(50));                     // root with two children -> successor 65
    assert(!t.erase(999));
    assert((t.inorder() == std::vector<int>{30, 35, 40, 45, 65, 80}) && t.valid() && t.size() == 6);

    // Round trip through serialisation preserves structure (same pre-order string).
    std::string s = t.serialise();
    BST u = BST::deserialise(s);
    assert(u.serialise() == s && u.size() == t.size() && u.inorder() == t.inorder());
    std::printf("serialised: %s\n", s.c_str());

    // Random differential test against a sorted vector.
    std::mt19937 rng(11);
    BST r;
    std::vector<int> mirror;
    for (int step = 0; step < 50'000; ++step) {
        int k = static_cast<int>(rng() % 2000);
        if (rng() % 3) {
            bool ins = r.insert(k);
            auto it = std::lower_bound(mirror.begin(), mirror.end(), k);
            bool fresh = (it == mirror.end() || *it != k);
            assert(ins == fresh);
            if (fresh) mirror.insert(it, k);
        } else {
            bool er = r.erase(k);
            auto it = std::lower_bound(mirror.begin(), mirror.end(), k);
            bool present = (it != mirror.end() && *it == k);
            assert(er == present);
            if (present) mirror.erase(it);
        }
    }
    assert(r.inorder() == mirror && r.valid() && r.size() == mirror.size());

    // Balanced build.
    std::vector<int> sorted(1023);
    for (std::size_t i = 0; i < sorted.size(); ++i) sorted[i] = static_cast<int>(i);
    BST b = BST::from_sorted(sorted);
    assert(b.height() == 9 && b.balanced_height() > 0 && b.valid());

    // The experiment: sorted insertion vs shuffled insertion. n is kept at 20k because the
    // sorted case is O(n^2) total (each insert walks the whole chain) - that is the point.
    {
        const int n = 20'000;
        BST sorted_tree;
        for (int i = 0; i < n; ++i) sorted_tree.insert(i);
        std::vector<int> keys(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) keys[static_cast<std::size_t>(i)] = i;
        std::shuffle(keys.begin(), keys.end(), rng);
        BST shuffled_tree;
        for (int k : keys) shuffled_tree.insert(k);
        std::printf("n=%d: height after sorted insertion = %d (a linked list), after shuffled = %d (~2.99 log2 n = %.0f)\n",
                    n, sorted_tree.height(), shuffled_tree.height(), 2.99 * std::log2(static_cast<double>(n)));
        assert(sorted_tree.height() == n - 1);
        assert(shuffled_tree.height() < 60);
    }   // both trees destroyed iteratively here; the sorted one is 100k deep
    std::printf("all BST checks passed\n");
    return 0;
}
