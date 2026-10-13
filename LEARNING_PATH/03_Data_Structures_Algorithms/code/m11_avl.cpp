// m11_avl.cpp
// AVL tree with std::unique_ptr: insert and erase with the four rotation cases, cached
// heights, rotation counter, invariant validation, differential test against std::set, and a
// height-bound check (height <= 1.44 log2(n+2)) after sorted insertion.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m11_avl.cpp -o m11_avl

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>
#include <vector>

class AVL {
    struct Node {
        int key;
        int height = 1;                     // leaf = 1; null = 0
        std::unique_ptr<Node> left, right;
        explicit Node(int k) : key(k) {}
    };
    using Link = std::unique_ptr<Node>;

public:
    std::size_t size() const { return size_; }
    int height() const { return h(root_); }
    long long rotations() const { return rotations_; }

    bool insert(int key) {
        bool inserted = false;
        insert(root_, key, inserted);
        if (inserted) ++size_;
        return inserted;
    }
    bool erase(int key) {
        bool erased = false;
        erase(root_, key, erased);
        if (erased) --size_;
        return erased;
    }
    bool contains(int key) const {
        const Node* n = root_.get();
        while (n && n->key != key) n = key < n->key ? n->left.get() : n->right.get();
        return n != nullptr;
    }
    std::vector<int> inorder() const {
        std::vector<int> out;
        out.reserve(size_);
        std::vector<const Node*> st;
        const Node* cur = root_.get();
        while (cur || !st.empty()) {
            while (cur) { st.push_back(cur); cur = cur->left.get(); }
            cur = st.back(); st.pop_back();
            out.push_back(cur->key);
            cur = cur->right.get();
        }
        return out;
    }
    // Checks BST order, stored heights, and |balance| <= 1 at every node.
    bool valid() const { return check(root_.get(), -(1LL << 40), (1LL << 40)) >= 0; }

    ~AVL() {   // iterative teardown (height is O(log n) here, but keep the habit)
        std::vector<Link> st;
        if (root_) st.push_back(std::move(root_));
        while (!st.empty()) {
            Link n = std::move(st.back()); st.pop_back();
            if (n->left) st.push_back(std::move(n->left));
            if (n->right) st.push_back(std::move(n->right));
        }
    }

private:
    static int h(const Link& n) { return n ? n->height : 0; }
    static void update(Link& n) { n->height = 1 + std::max(h(n->left), h(n->right)); }
    static int balance(const Link& n) { return n ? h(n->left) - h(n->right) : 0; }

    // Right rotation: link owns y, y->left is x. After: link owns x, y is x->right.
    void rotate_right(Link& link) {
        Link y = std::move(link);
        Link x = std::move(y->left);
        y->left = std::move(x->right);      // B moves under y
        update(y);
        x->right = std::move(y);
        update(x);
        link = std::move(x);
        ++rotations_;
    }
    void rotate_left(Link& link) {
        Link x = std::move(link);
        Link y = std::move(x->right);
        x->right = std::move(y->left);
        update(x);
        y->left = std::move(x);
        update(y);
        link = std::move(y);
        ++rotations_;
    }

    // Restore the AVL invariant at `link` assuming its subtrees are already AVL and heights
    // of children are correct. The four cases.
    void rebalance(Link& link) {
        update(link);
        int b = balance(link);
        if (b > 1) {                                       // left heavy
            if (balance(link->left) < 0) rotate_left(link->left);   // Left-Right -> Left-Left
            rotate_right(link);
        } else if (b < -1) {                               // right heavy
            if (balance(link->right) > 0) rotate_right(link->right); // Right-Left -> Right-Right
            rotate_left(link);
        }
    }

    void insert(Link& link, int key, bool& inserted) {
        if (!link) { link = std::make_unique<Node>(key); inserted = true; return; }
        if (key == link->key) return;                      // no duplicates
        insert(key < link->key ? link->left : link->right, key, inserted);
        if (inserted) rebalance(link);                     // fix on the way back up
    }

    // Detach and return the minimum node of the subtree, rebalancing on the way up.
    Link take_min(Link& link) {
        if (!link->left) {
            Link m = std::move(link);
            link = std::move(m->right);
            return m;
        }
        Link m = take_min(link->left);
        rebalance(link);
        return m;
    }

    void erase(Link& link, int key, bool& erased) {
        if (!link) return;
        if (key < link->key) erase(link->left, key, erased);
        else if (key > link->key) erase(link->right, key, erased);
        else {
            erased = true;
            if (!link->left || !link->right) {
                link = std::move(link->left ? link->left : link->right);   // 0 or 1 child
                return;                                                   // nothing to rebalance here
            }
            // Two children: replace this node by its in-order successor node (not just its key),
            // which keeps the unique_ptr ownership simple.
            Link succ = take_min(link->right);
            succ->left = std::move(link->left);
            succ->right = std::move(link->right);
            link = std::move(succ);
        }
        if (link) rebalance(link);
    }

    // Returns the height if valid, else -1.
    static int check(const Node* n, long long lo, long long hi) {
        if (!n) return 0;
        if (n->key <= lo || n->key >= hi) return -1;
        int l = check(n->left.get(), lo, n->key);
        if (l < 0) return -1;
        int r = check(n->right.get(), n->key, hi);
        if (r < 0) return -1;
        if (std::abs(l - r) > 1) return -1;
        if (n->height != 1 + std::max(l, r)) return -1;
        return 1 + std::max(l, r);
    }

    Link root_;
    std::size_t size_ = 0;
    long long rotations_ = 0;
};

int main() {
    // Each of the four cases, by hand.
    { AVL t; for (int k : {30, 20, 10}) t.insert(k); assert(t.valid() && t.height() == 2 && t.rotations() == 1); }  // LL
    { AVL t; for (int k : {10, 20, 30}) t.insert(k); assert(t.valid() && t.height() == 2 && t.rotations() == 1); }  // RR
    { AVL t; for (int k : {30, 10, 20}) t.insert(k); assert(t.valid() && t.height() == 2 && t.rotations() == 2); }  // LR
    { AVL t; for (int k : {10, 30, 20}) t.insert(k); assert(t.valid() && t.height() == 2 && t.rotations() == 2); }  // RL

    // Sorted insertion: the plain BST's worst case is the AVL's everyday case.
    {
        const int n = 1'000'000;
        AVL t;
        for (int i = 0; i < n; ++i) t.insert(i);
        double bound = 1.4405 * std::log2(n + 2.0);
        std::printf("AVL after inserting 1..%d in order: height %d (bound %.1f, perfect %.1f), rotations %lld\n",
                    n, t.height(), bound, std::log2(n + 1.0), t.rotations());
        assert(t.valid() && t.height() <= static_cast<int>(bound));
        assert(t.contains(0) && t.contains(n - 1) && !t.contains(n));
    }

    // Differential test against std::set with mixed insert/erase.
    {
        std::mt19937 rng(2024);
        AVL t;
        std::set<int> ref;
        for (int step = 0; step < 300'000; ++step) {
            int k = static_cast<int>(rng() % 5000);
            if (rng() % 5 < 3) assert(t.insert(k) == ref.insert(k).second);
            else                assert(t.erase(k) == (ref.erase(k) == 1));
            if (step % 50'000 == 0) assert(t.valid());
        }
        assert(t.valid() && t.size() == ref.size());
        std::vector<int> got = t.inorder();
        assert(std::equal(got.begin(), got.end(), ref.begin(), ref.end()));
        std::printf("AVL vs std::set: %zu keys after 300k mixed ops, height %d, valid\n", t.size(), t.height());
    }
    std::printf("all AVL checks passed\n");
    return 0;
}
