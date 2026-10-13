// m04_hash_map_chaining.cpp
// A complete separately-chained hash map: find/insert/erase/operator[], resize at load
// factor 1.0, iteration via a visitor, move semantics, destructor. Tested against
// std::unordered_map with random operations.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_hash_map_chaining.cpp -o m04_chain

#include <cassert>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

template <typename K, typename V, typename Hash = std::hash<K>>
class HashMap {
    struct Node {
        K key;
        V value;
        Node* next;
    };

public:
    explicit HashMap(std::size_t initial_buckets = 8) : buckets_(initial_buckets, nullptr) {}

    ~HashMap() { clear(); }

    // Copying would need a deep copy of every chain; we do not need it here, so forbid it
    // rather than let the compiler generate a shallow copy that double-frees.
    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept
        : buckets_(std::move(other.buckets_)), size_(other.size_), hash_(std::move(other.hash_)) {
        other.size_ = 0;
        other.buckets_.assign(8, nullptr);   // leave the source valid and empty
    }
    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            clear();
            buckets_ = std::move(other.buckets_);
            size_ = other.size_;
            other.size_ = 0;
            other.buckets_.assign(8, nullptr);
        }
        return *this;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::size_t bucket_count() const { return buckets_.size(); }
    double load_factor() const { return static_cast<double>(size_) / static_cast<double>(buckets_.size()); }

    // Returns pointer to the value or nullptr. A pointer (not an iterator) keeps the
    // interface small; it stays valid until that key is erased, even across rehash, because
    // nodes are relinked, never copied.
    V* find(const K& key) {
        for (Node* p = buckets_[index_of(key)]; p != nullptr; p = p->next)
            if (p->key == key) return &p->value;
        return nullptr;
    }
    const V* find(const K& key) const {
        for (const Node* p = buckets_[index_of(key)]; p != nullptr; p = p->next)
            if (p->key == key) return &p->value;
        return nullptr;
    }

    // Insert or overwrite. Returns true if a new key was added.
    bool insert(const K& key, const V& value) {
        if (V* existing = find(key)) {
            *existing = value;
            return false;
        }
        grow_if_needed();
        std::size_t i = index_of(key);
        buckets_[i] = new Node{key, value, buckets_[i]};   // push front: O(1)
        ++size_;
        return true;
    }

    // Like std::unordered_map::operator[]: inserts a default V if absent.
    V& operator[](const K& key) {
        if (V* existing = find(key)) return *existing;
        grow_if_needed();
        std::size_t i = index_of(key);
        buckets_[i] = new Node{key, V{}, buckets_[i]};
        ++size_;
        return buckets_[i]->value;
    }

    bool erase(const K& key) {
        // Pointer-to-pointer walk: `pp` always points at the link that leads to the current
        // node, so unlinking the head needs no special case.
        Node** pp = &buckets_[index_of(key)];
        while (*pp != nullptr) {
            if ((*pp)->key == key) {
                Node* dead = *pp;
                *pp = dead->next;
                delete dead;
                --size_;
                return true;
            }
            pp = &(*pp)->next;
        }
        return false;
    }

    void clear() {
        for (Node*& head : buckets_) {
            while (head != nullptr) {
                Node* next = head->next;
                delete head;
                head = next;
            }
        }
        size_ = 0;
    }

    // Visit every (key, value). Simpler than writing an iterator class; good enough here.
    template <typename F>
    void for_each(F&& f) const {
        for (const Node* head : buckets_)
            for (const Node* p = head; p != nullptr; p = p->next) f(p->key, p->value);
    }

    std::size_t longest_chain() const {
        std::size_t best = 0;
        for (const Node* head : buckets_) {
            std::size_t len = 0;
            for (const Node* p = head; p != nullptr; p = p->next) ++len;
            if (len > best) best = len;
        }
        return best;
    }

private:
    static constexpr double kMaxLoad = 1.0;

    std::size_t index_of(const K& key) const { return hash_(key) % buckets_.size(); }

    void grow_if_needed() {
        if (static_cast<double>(size_ + 1) > kMaxLoad * static_cast<double>(buckets_.size()))
            rehash(buckets_.size() * 2);
    }

    void rehash(std::size_t new_count) {
        std::vector<Node*> nb(new_count, nullptr);
        for (Node* head : buckets_) {
            while (head != nullptr) {
                Node* next = head->next;
                std::size_t i = hash_(head->key) % new_count;
                head->next = nb[i];   // relink the existing node; no allocation, no copy
                nb[i] = head;
                head = next;
            }
        }
        buckets_.swap(nb);
    }

    std::vector<Node*> buckets_;
    std::size_t size_ = 0;
    Hash hash_;
};

int main() {
    // ---- basic behaviour ----------------------------------------------------------------
    HashMap<std::string, int> m;
    assert(m.insert("one", 1));
    assert(m.insert("two", 2));
    assert(!m.insert("two", 22));            // overwrite, not a new key
    assert(*m.find("two") == 22);
    assert(m.find("three") == nullptr);
    m["three"] = 3;
    assert(m.size() == 3);
    assert(m.erase("one") && !m.erase("one"));
    assert(m.size() == 2);

    // ---- growth: watch bucket count double while load stays <= 1 -------------------------
    HashMap<int, int> g;
    std::size_t last = g.bucket_count();
    for (int i = 0; i < 100'000; ++i) {
        g.insert(i * 7919, i);
        if (g.bucket_count() != last) {
            last = g.bucket_count();
            std::printf("size=%7zu buckets=%7zu load=%.2f longest_chain=%zu\n", g.size(), last,
                        g.load_factor(), g.longest_chain());
        }
    }

    // ---- randomised differential test against std::unordered_map ------------------------
    std::mt19937 rng(42);
    std::unordered_map<int, int> ref;
    HashMap<int, int> mine;
    for (int step = 0; step < 200'000; ++step) {
        int key = static_cast<int>(rng() % 5000);
        switch (rng() % 3) {
            case 0: ref[key] = step; mine.insert(key, step); break;
            case 1: ref.erase(key); mine.erase(key); break;
            default: {
                auto it = ref.find(key);
                int* p = mine.find(key);
                assert((it == ref.end()) == (p == nullptr));
                if (p) assert(*p == it->second);
            }
        }
    }
    assert(ref.size() == mine.size());
    std::size_t visited = 0;
    mine.for_each([&](const int& k, const int& v) {
        ++visited;
        assert(ref.at(k) == v);
    });
    assert(visited == ref.size());

    // ---- move semantics -----------------------------------------------------------------
    HashMap<int, int> moved = std::move(mine);
    assert(moved.size() == ref.size() && mine.empty());

    std::printf("chained HashMap: all checks passed (%zu entries, %zu buckets)\n", moved.size(),
                moved.bucket_count());
    return 0;
}
