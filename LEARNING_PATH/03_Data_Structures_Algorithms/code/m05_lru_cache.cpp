// m05_lru_cache.cpp
// LRU cache two ways: (1) std::list + std::unordered_map using splice for O(1) move-to-front;
// (2) hand-rolled doubly linked nodes with a sentinel, which is what you write when the
// interviewer says "no std::list". Both run the same test sequence.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m05_lru_cache.cpp -o m05_lru

#include <cassert>
#include <cstdio>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// ---- Version 1: standard library -------------------------------------------------------------
class LRUCache {
    using Entry = std::pair<int, int>;   // key, value
public:
    explicit LRUCache(std::size_t capacity) : cap_(capacity) {}

    std::optional<int> get(int key) {
        auto it = pos_.find(key);
        if (it == pos_.end()) return std::nullopt;
        // splice relinks the node at the front: O(1), no allocation, iterator in the map stays valid.
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    void put(int key, int value) {
        auto it = pos_.find(key);
        if (it != pos_.end()) {
            it->second->second = value;
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        if (order_.size() == cap_) {
            pos_.erase(order_.back().first);   // evict least recently used: the back
            order_.pop_back();
        }
        order_.emplace_front(key, value);
        pos_[key] = order_.begin();
    }

    std::size_t size() const { return order_.size(); }

private:
    std::size_t cap_;
    std::list<Entry> order_;                                   // front = most recently used
    std::unordered_map<int, std::list<Entry>::iterator> pos_;  // key -> node
};

// ---- Version 2: hand-rolled doubly linked list with sentinel --------------------------------
class LRUCacheManual {
    struct Node {
        int key, value;
        Node* prev;
        Node* next;
    };
public:
    explicit LRUCacheManual(std::size_t capacity) : cap_(capacity) {
        // Circular sentinel: head_.next is most recent, head_.prev is least recent.
        // Empty list <=> head_.next == &head_. No null checks anywhere below.
        head_.prev = head_.next = &head_;
    }
    ~LRUCacheManual() {
        for (Node* p = head_.next; p != &head_;) { Node* n = p->next; delete p; p = n; }
    }
    LRUCacheManual(const LRUCacheManual&) = delete;
    LRUCacheManual& operator=(const LRUCacheManual&) = delete;

    std::optional<int> get(int key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        move_to_front(it->second);
        return it->second->value;
    }

    void put(int key, int value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = value;
            move_to_front(it->second);
            return;
        }
        if (map_.size() == cap_) {
            Node* lru = head_.prev;        // least recently used is just before the sentinel
            unlink(lru);
            map_.erase(lru->key);
            delete lru;
        }
        Node* n = new Node{key, value, nullptr, nullptr};
        link_front(n);
        map_[key] = n;
    }

    std::size_t size() const { return map_.size(); }

private:
    void unlink(Node* n) {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }
    void link_front(Node* n) {
        n->next = head_.next;
        n->prev = &head_;
        head_.next->prev = n;
        head_.next = n;
    }
    void move_to_front(Node* n) {
        unlink(n);
        link_front(n);
    }

    std::size_t cap_;
    Node head_{0, 0, nullptr, nullptr};   // sentinel, never holds data
    std::unordered_map<int, Node*> map_;
};

// ---- Shared test sequence -------------------------------------------------------------------
template <typename Cache>
void run_test(const char* name) {
    Cache c(2);
    c.put(1, 1);
    c.put(2, 2);
    assert(c.get(1) == 1);          // order: 1, 2
    c.put(3, 3);                    // evicts 2
    assert(!c.get(2).has_value());
    c.put(4, 4);                    // evicts 1
    assert(!c.get(1).has_value());
    assert(c.get(3) == 3);
    assert(c.get(4) == 4);
    c.put(4, 40);                   // update existing: value changes, no eviction
    assert(c.get(4) == 40 && c.size() == 2);
    c.put(5, 5);                    // evicts 3 (4 was touched more recently)
    assert(!c.get(3).has_value());
    assert(c.get(5) == 5);

    // Larger scenario: capacity 100, 10k touches with skewed keys; everything hot must stay.
    Cache big(100);
    for (int i = 0; i < 100; ++i) big.put(i, i * 10);
    for (int round = 0; round < 100; ++round)
        for (int k = 0; k < 50; ++k) assert(big.get(k) == k * 10);   // keep 0..49 hot
    for (int k = 100; k < 150; ++k) big.put(k, k);                   // 50 inserts evict 50..99
    for (int k = 0; k < 50; ++k) assert(big.get(k).has_value());
    for (int k = 50; k < 100; ++k) assert(!big.get(k).has_value());
    assert(big.size() == 100);
    std::printf("%s: all checks passed\n", name);
}

int main() {
    run_test<LRUCache>("LRUCache (std::list + unordered_map)");
    run_test<LRUCacheManual>("LRUCacheManual (hand-rolled nodes)");
    return 0;
}
