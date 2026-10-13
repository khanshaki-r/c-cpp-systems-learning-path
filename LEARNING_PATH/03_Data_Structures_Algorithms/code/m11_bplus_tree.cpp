// m11_bplus_tree.cpp
// An in-memory B+tree: search, insert (leaf split copies the first key up; internal split
// moves the median up), erase (borrow from a sibling, else merge; root collapse), range scan,
// a Cursor with seek/next (the DB_SET_RANGE / DB_NEXT pair), invariant validation, and a
// differential test against std::map. kMaxKeys is small so that splits are visible.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m11_bplus_tree.cpp -o m11_bpt

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

template <typename K, typename V, std::size_t kMaxKeys = 4>
class BPlusTree {
    static_assert(kMaxKeys >= 3, "need at least 3 keys per node for borrow/merge to work");
    static constexpr std::size_t kMinKeys = kMaxKeys / 2;   // non-root nodes keep at least this many

    struct Node {
        bool leaf = true;
        std::vector<K> keys;                              // sorted
        std::vector<std::unique_ptr<Node>> children;     // internal only: keys.size() + 1
        std::vector<V> values;                            // leaf only: keys.size()
        Node* next = nullptr;                             // leaf only: right sibling
    };
    struct Split {
        K separator;
        std::unique_ptr<Node> right;
    };

public:
    BPlusTree() : root_(std::make_unique<Node>()) {}

    // ---- Cursor: a position in the leaf chain ------------------------------------------------
    class Cursor {
    public:
        Cursor() = default;
        bool valid() const { return leaf_ != nullptr && pos_ < leaf_->keys.size(); }
        const K& key() const { return leaf_->keys[pos_]; }
        const V& value() const { return leaf_->values[pos_]; }
        void next() {                                     // DB_NEXT
            if (!valid()) return;
            if (++pos_ >= leaf_->keys.size()) { leaf_ = leaf_->next; pos_ = 0; }
            // An empty leaf cannot exist (except an empty root), so no loop is needed here.
        }
    private:
        friend class BPlusTree;
        Cursor(const Node* leaf, std::size_t pos) : leaf_(leaf), pos_(pos) {}
        const Node* leaf_ = nullptr;
        std::size_t pos_ = 0;
    };

    std::size_t size() const { return size_; }
    int height() const {                                   // number of levels
        int h = 1;
        for (const Node* n = root_.get(); !n->leaf; n = n->children[0].get()) ++h;
        return h;
    }

    const V* find(const K& key) const {
        const Node* n = descend(key);
        auto it = std::lower_bound(n->keys.begin(), n->keys.end(), key);
        if (it == n->keys.end() || *it != key) return nullptr;
        return &n->values[static_cast<std::size_t>(it - n->keys.begin())];
    }

    // First entry with key >= `key` (DB_SET_RANGE). Invalid cursor if none.
    Cursor seek(const K& key) const {
        const Node* n = descend(key);
        std::size_t pos = static_cast<std::size_t>(std::lower_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
        Cursor c(n, pos);
        if (pos == n->keys.size()) { c.leaf_ = n->next; c.pos_ = 0; }   // key is larger than everything in this leaf
        return c;
    }
    Cursor begin() const {                                 // DB_FIRST
        const Node* n = root_.get();
        while (!n->leaf) n = n->children[0].get();
        return Cursor(n, 0);
    }

    // Returns true if a new key was inserted, false if an existing key's value was replaced.
    bool insert(const K& key, const V& value) {
        bool inserted = false;
        std::optional<Split> s = insert_rec(root_.get(), key, value, inserted);
        if (s) {                                           // root overflowed: grow at the top
            auto new_root = std::make_unique<Node>();
            new_root->leaf = false;
            new_root->keys.push_back(std::move(s->separator));
            new_root->children.push_back(std::move(root_));
            new_root->children.push_back(std::move(s->right));
            root_ = std::move(new_root);
        }
        if (inserted) ++size_;
        return inserted;
    }

    bool erase(const K& key) {
        bool erased = erase_rec(root_.get(), key);
        if (erased) --size_;
        if (!root_->leaf && root_->keys.empty()) {         // root has a single child: shrink at the top
            std::unique_ptr<Node> child = std::move(root_->children[0]);
            root_ = std::move(child);
        }
        return erased;
    }

    // Range scan [lo, hi) through the leaf chain: one descent, then sequential.
    std::vector<std::pair<K, V>> range(const K& lo, const K& hi) const {
        std::vector<std::pair<K, V>> out;
        for (Cursor c = seek(lo); c.valid() && c.key() < hi; c.next()) out.emplace_back(c.key(), c.value());
        return out;
    }

    // Structural invariants: ordering, key counts, same depth for all leaves, children = keys+1,
    // separators bound their subtrees, and the leaf chain visits exactly the leaves in order.
    bool valid() const {
        int leaf_depth = -1;
        std::vector<const Node*> leaves_by_tree;
        if (!check(root_.get(), 0, nullptr, nullptr, leaf_depth, leaves_by_tree, true)) return false;
        const Node* n = root_.get();
        while (!n->leaf) n = n->children[0].get();
        std::size_t i = 0, count = 0;
        for (; n; n = n->next, ++i) {
            if (i >= leaves_by_tree.size() || leaves_by_tree[i] != n) return false;
            count += n->keys.size();
        }
        return i == leaves_by_tree.size() && count == size_;
    }

    // Prints the tree one level per line (keys only) - for small trees.
    void print() const {
        std::vector<const Node*> level{root_.get()};
        int depth = 0;
        while (!level.empty()) {
            std::printf("  L%d:", depth++);
            std::vector<const Node*> nxt;
            for (const Node* n : level) {
                std::printf(" [");
                for (std::size_t i = 0; i < n->keys.size(); ++i) std::printf("%s%s", i ? " " : "", std::to_string(n->keys[i]).c_str());
                std::printf("]");
                for (const auto& c : n->children) nxt.push_back(c.get());
            }
            std::printf("\n");
            level = std::move(nxt);
        }
    }

private:
    const Node* descend(const K& key) const {
        const Node* n = root_.get();
        while (!n->leaf) {
            // upper_bound: index of the first separator > key == index of the child to follow.
            std::size_t i = static_cast<std::size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
            n = n->children[i].get();
        }
        return n;
    }

    std::optional<Split> insert_rec(Node* n, const K& key, const V& value, bool& inserted) {
        if (n->leaf) {
            auto pos = std::lower_bound(n->keys.begin(), n->keys.end(), key);
            std::size_t i = static_cast<std::size_t>(pos - n->keys.begin());
            if (pos != n->keys.end() && *pos == key) {
                n->values[i] = value;
                inserted = false;
                return std::nullopt;
            }
            n->keys.insert(pos, key);
            n->values.insert(n->values.begin() + static_cast<std::ptrdiff_t>(i), value);
            inserted = true;
            if (n->keys.size() <= kMaxKeys) return std::nullopt;
            return split_leaf(n);
        }
        std::size_t i = static_cast<std::size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
        std::optional<Split> s = insert_rec(n->children[i].get(), key, value, inserted);
        if (!s) return std::nullopt;
        // Child i split: its new right sibling goes at i+1, separated by s->separator at i.
        n->keys.insert(n->keys.begin() + static_cast<std::ptrdiff_t>(i), std::move(s->separator));
        n->children.insert(n->children.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(s->right));
        if (n->keys.size() <= kMaxKeys) return std::nullopt;
        return split_internal(n);
    }

    Split split_leaf(Node* n) {
        std::size_t mid = n->keys.size() / 2;
        auto right = std::make_unique<Node>();
        right->leaf = true;
        right->keys.assign(n->keys.begin() + static_cast<std::ptrdiff_t>(mid), n->keys.end());
        right->values.assign(n->values.begin() + static_cast<std::ptrdiff_t>(mid), n->values.end());
        n->keys.resize(mid);
        n->values.resize(mid);
        right->next = n->next;                             // splice into the leaf chain
        n->next = right.get();
        K sep = right->keys.front();                       // COPY: the key stays in the leaf
        return Split{sep, std::move(right)};
    }

    Split split_internal(Node* n) {
        std::size_t mid = n->keys.size() / 2;
        K sep = n->keys[mid];                              // MOVES up: separators only guide
        auto right = std::make_unique<Node>();
        right->leaf = false;
        right->keys.assign(n->keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1, n->keys.end());
        for (std::size_t j = mid + 1; j < n->children.size(); ++j) right->children.push_back(std::move(n->children[j]));
        n->keys.resize(mid);
        n->children.resize(mid + 1);
        return Split{sep, std::move(right)};
    }

    bool erase_rec(Node* n, const K& key) {
        if (n->leaf) {
            auto pos = std::lower_bound(n->keys.begin(), n->keys.end(), key);
            if (pos == n->keys.end() || *pos != key) return false;
            std::size_t i = static_cast<std::size_t>(pos - n->keys.begin());
            n->keys.erase(pos);
            n->values.erase(n->values.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        std::size_t i = static_cast<std::size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
        bool erased = erase_rec(n->children[i].get(), key);
        if (erased && n->children[i]->keys.size() < kMinKeys) fix_underflow(n, i);
        return erased;
    }

    // Child i of parent p has too few keys. Borrow from a sibling if it can spare one,
    // otherwise merge with a sibling and drop one separator from p.
    void fix_underflow(Node* p, std::size_t i) {
        Node* c = p->children[i].get();
        Node* left = i > 0 ? p->children[i - 1].get() : nullptr;
        Node* right = i + 1 < p->children.size() ? p->children[i + 1].get() : nullptr;

        if (left && left->keys.size() > kMinKeys) {        // borrow from the left sibling
            if (c->leaf) {
                c->keys.insert(c->keys.begin(), left->keys.back());
                c->values.insert(c->values.begin(), left->values.back());
                left->keys.pop_back();
                left->values.pop_back();
                p->keys[i - 1] = c->keys.front();          // separator = first key of right side
            } else {
                c->keys.insert(c->keys.begin(), p->keys[i - 1]);       // separator rotates down
                c->children.insert(c->children.begin(), std::move(left->children.back()));
                left->children.pop_back();
                p->keys[i - 1] = left->keys.back();                    // left's last key rotates up
                left->keys.pop_back();
            }
            return;
        }
        if (right && right->keys.size() > kMinKeys) {      // borrow from the right sibling
            if (c->leaf) {
                c->keys.push_back(right->keys.front());
                c->values.push_back(right->values.front());
                right->keys.erase(right->keys.begin());
                right->values.erase(right->values.begin());
                p->keys[i] = right->keys.front();
            } else {
                c->keys.push_back(p->keys[i]);
                c->children.push_back(std::move(right->children.front()));
                right->children.erase(right->children.begin());
                p->keys[i] = right->keys.front();
                right->keys.erase(right->keys.begin());
            }
            return;
        }
        // Merge: always merge the pair (left, c) or (c, right) into the left-hand node.
        std::size_t li = left ? i - 1 : i;                 // index of the left node of the pair
        Node* a = p->children[li].get();
        Node* b = p->children[li + 1].get();
        if (a->leaf) {
            a->keys.insert(a->keys.end(), b->keys.begin(), b->keys.end());
            a->values.insert(a->values.end(), b->values.begin(), b->values.end());
            a->next = b->next;                             // unlink b from the chain
        } else {
            a->keys.push_back(p->keys[li]);                // separator comes down between them
            a->keys.insert(a->keys.end(), b->keys.begin(), b->keys.end());
            for (auto& ch : b->children) a->children.push_back(std::move(ch));
        }
        p->keys.erase(p->keys.begin() + static_cast<std::ptrdiff_t>(li));
        p->children.erase(p->children.begin() + static_cast<std::ptrdiff_t>(li) + 1);   // destroys b
    }

    bool check(const Node* n, int depth, const K* lo, const K* hi, int& leaf_depth,
               std::vector<const Node*>& leaves, bool is_root) const {
        if (!std::is_sorted(n->keys.begin(), n->keys.end())) return false;
        if (std::adjacent_find(n->keys.begin(), n->keys.end()) != n->keys.end()) return false;
        if (!is_root && n->keys.size() < kMinKeys) return false;
        if (n->keys.size() > kMaxKeys) return false;
        for (const K& k : n->keys) {
            if (lo && k < *lo) return false;              // keys in subtree >= lower separator
            if (hi && !(k < *hi)) return false;           // and < upper separator
        }
        if (n->leaf) {
            if (n->values.size() != n->keys.size() || !n->children.empty()) return false;
            if (leaf_depth < 0) leaf_depth = depth;
            if (leaf_depth != depth) return false;
            leaves.push_back(n);
            return true;
        }
        if (n->children.size() != n->keys.size() + 1 || !n->values.empty()) return false;
        for (std::size_t i = 0; i < n->children.size(); ++i) {
            const K* clo = i == 0 ? lo : &n->keys[i - 1];
            const K* chi = i == n->keys.size() ? hi : &n->keys[i];
            if (!check(n->children[i].get(), depth + 1, clo, chi, leaf_depth, leaves, false)) return false;
        }
        return true;
    }

    std::unique_ptr<Node> root_;
    std::size_t size_ = 0;
};

int main() {
    // ---- watch splits happen with a tiny fan-out ---------------------------------------------
    {
        BPlusTree<int, std::string, 3> t;
        for (int k = 1; k <= 10; ++k) {
            t.insert(k, "v" + std::to_string(k));
            assert(t.valid());
        }
        std::printf("B+tree (max 3 keys) after inserting 1..10:\n");
        t.print();
        assert(t.height() == 3 && t.size() == 10);
        assert(*t.find(7) == "v7" && t.find(11) == nullptr);

        auto r = t.range(4, 8);                                // 4 5 6 7
        assert(r.size() == 4 && r.front().first == 4 && r.back().first == 7);

        // Cursor walk from DB_SET_RANGE(6) to the end crosses leaf boundaries.
        std::printf("cursor from seek(6): ");
        int expect = 6;
        for (auto c = t.seek(6); c.valid(); c.next()) { std::printf("%d ", c.key()); assert(c.key() == expect++); }
        std::printf("\n");
        assert(expect == 11);
        assert(!t.seek(99).valid());

        for (int k : {1, 2, 3, 10, 5}) { assert(t.erase(k)); assert(t.valid()); }
        assert(!t.erase(42));
        std::printf("after erasing 1 2 3 10 5:\n");
        t.print();
        assert(t.size() == 5 && t.find(4) && !t.find(5));
        for (int k : {4, 6, 7, 8, 9}) assert(t.erase(k) && t.valid());
        assert(t.size() == 0 && t.height() == 1 && !t.begin().valid());
    }

    // ---- differential test against std::map: random insert / overwrite / erase / range ------
    {
        std::mt19937 rng(77);
        BPlusTree<int, int, 4> t;
        std::map<int, int> ref;
        for (int step = 0; step < 200'000; ++step) {
            int k = static_cast<int>(rng() % 3000);
            unsigned op = rng() % 10;
            if (op < 5) {
                bool fresh = t.insert(k, step);
                assert(fresh == (ref.find(k) == ref.end()));
                ref[k] = step;
            } else if (op < 9) {
                assert(t.erase(k) == (ref.erase(k) == 1));
            } else {
                int hi = k + static_cast<int>(rng() % 200);
                auto got = t.range(k, hi);
                auto it = ref.lower_bound(k);
                std::size_t n = 0;
                for (; it != ref.end() && it->first < hi; ++it, ++n) {
                    assert(n < got.size() && got[n].first == it->first && got[n].second == it->second);
                }
                assert(n == got.size());
            }
            if (step % 20'000 == 0) assert(t.valid());
        }
        assert(t.valid() && t.size() == ref.size());
        std::size_t walked = 0;
        auto it = ref.begin();
        for (auto c = t.begin(); c.valid(); c.next(), ++it, ++walked) assert(c.key() == it->first && c.value() == it->second);
        assert(walked == ref.size());
        std::printf("B+tree vs std::map: %zu keys after 200k ops, height %d, full cursor walk matches\n",
                    t.size(), t.height());
    }

    // ---- realistic fan-out: height stays tiny -------------------------------------------------
    {
        BPlusTree<int, int, 256> t;
        const int n = 1'000'000;
        for (int i = 0; i < n; ++i) t.insert(i, i);          // sequential: always the rightmost leaf
        assert(t.valid());
        std::printf("B+tree (max 256 keys) with %d sequential keys: height %d\n", n, t.height());
        assert(t.height() <= 4);
    }
    std::printf("all B+tree checks passed\n");
    return 0;
}
