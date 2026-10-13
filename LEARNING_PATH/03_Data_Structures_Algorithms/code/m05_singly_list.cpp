// m05_singly_list.cpp
// An owning singly linked list with std::unique_ptr: push_front/back, pop_front, insert_after,
// erase_first (pointer-to-owner walk, no head special case), reverse, find, iteration, and an
// iterative destructor that does not blow the stack on long lists.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m05_singly_list.cpp -o m05_list

#include <cassert>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

class SinglyList {
    struct Node {
        int val;
        std::unique_ptr<Node> next;   // owns the rest of the chain
    };

public:
    SinglyList() = default;

    // unique_ptr's default destructor would recurse: ~Node -> ~next -> ~Node ... one frame per
    // node. Pop iteratively instead: each assignment destroys exactly one node.
    ~SinglyList() {
        while (head_) head_ = std::move(head_->next);
    }

    SinglyList(const SinglyList&) = delete;
    SinglyList& operator=(const SinglyList&) = delete;
    // Move: steal the chain and reset the source so its destructor has nothing to do.
    SinglyList(SinglyList&& o) noexcept
        : head_(std::move(o.head_)), tail_(std::exchange(o.tail_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    SinglyList& operator=(SinglyList&& o) noexcept {
        if (this != &o) {
            while (head_) head_ = std::move(head_->next);
            head_ = std::move(o.head_);
            tail_ = std::exchange(o.tail_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    void push_front(int v) {
        // Steal the current head into the new node's next, then make the new node the head.
        auto n = std::make_unique<Node>(Node{v, std::move(head_)});
        if (tail_ == nullptr) tail_ = n.get();
        head_ = std::move(n);
        ++size_;
    }

    void push_back(int v) {
        auto n = std::make_unique<Node>(Node{v, nullptr});
        Node* raw = n.get();                     // observe before we give up ownership
        if (tail_) tail_->next = std::move(n);
        else head_ = std::move(n);
        tail_ = raw;
        ++size_;
    }

    bool pop_front(int* out) {
        if (!head_) return false;
        if (out) *out = head_->val;
        head_ = std::move(head_->next);          // old head destroyed here
        if (!head_) tail_ = nullptr;
        --size_;
        return true;
    }

    // Insert after the first node equal to `after`. O(n) to find, O(1) to link.
    bool insert_after(int after, int v) {
        for (Node* p = head_.get(); p; p = p->next.get()) {
            if (p->val == after) {
                auto n = std::make_unique<Node>(Node{v, std::move(p->next)});  // new->next first
                if (p == tail_) tail_ = n.get();
                p->next = std::move(n);
                ++size_;
                return true;
            }
        }
        return false;
    }

    bool erase_first(int v) {
        // `link` points at the owner slot leading to the current node (head_ or some next).
        // Unlinking the head is therefore identical to unlinking any other node.
        std::unique_ptr<Node>* link = &head_;
        Node* prev = nullptr;
        while (*link) {
            if ((*link)->val == v) {
                if (link->get() == tail_) tail_ = prev;
                *link = std::move((*link)->next);   // the removed node dies at the end of this statement
                --size_;
                return true;
            }
            prev = link->get();
            link = &(*link)->next;
        }
        return false;
    }

    bool contains(int v) const {
        for (const Node* p = head_.get(); p; p = p->next.get())
            if (p->val == v) return true;
        return false;
    }

    // Reverse by moving ownership: prev <- cur <- next, one node at a time.
    void reverse() {
        std::unique_ptr<Node> prev;
        std::unique_ptr<Node> cur = std::move(head_);
        tail_ = cur.get();                       // old head becomes the tail
        while (cur) {
            std::unique_ptr<Node> next = std::move(cur->next);
            cur->next = std::move(prev);
            prev = std::move(cur);
            cur = std::move(next);
        }
        head_ = std::move(prev);
    }

    std::vector<int> to_vector() const {
        std::vector<int> out;
        out.reserve(size_);
        for (const Node* p = head_.get(); p; p = p->next.get()) out.push_back(p->val);
        return out;
    }

private:
    std::unique_ptr<Node> head_;
    Node* tail_ = nullptr;   // raw observer; the node is owned by the chain
    std::size_t size_ = 0;
};

namespace {
void print(const char* label, const SinglyList& l) {
    std::printf("%-22s", label);
    for (int v : l.to_vector()) std::printf("%d ", v);
    std::printf(" (size %zu)\n", l.size());
}
}  // namespace

int main() {
    SinglyList l;
    for (int v : {3, 7, 1}) l.push_back(v);
    l.push_front(9);
    print("build", l);
    assert((l.to_vector() == std::vector<int>{9, 3, 7, 1}));

    assert(l.insert_after(7, 8));
    assert(l.insert_after(1, 5));            // after the tail: tail must update
    print("insert_after", l);
    assert((l.to_vector() == std::vector<int>{9, 3, 7, 8, 1, 5}));

    assert(l.erase_first(9));                // head
    assert(l.erase_first(5));                // tail
    assert(l.erase_first(8));                // middle
    assert(!l.erase_first(42));
    print("erase_first x3", l);
    assert((l.to_vector() == std::vector<int>{3, 7, 1}));

    l.push_back(2);                          // tail pointer still right after tail erase?
    print("push_back after erase", l);
    assert((l.to_vector() == std::vector<int>{3, 7, 1, 2}));

    l.reverse();
    print("reverse", l);
    assert((l.to_vector() == std::vector<int>{2, 1, 7, 3}));
    l.push_back(0);                          // tail must be valid after reverse
    assert((l.to_vector() == std::vector<int>{2, 1, 7, 3, 0}));

    int x = 0;
    assert(l.pop_front(&x) && x == 2);
    assert(l.contains(7) && !l.contains(2));

    // The destructor test: 2 million nodes would overflow the stack with a recursive destructor.
    {
        SinglyList big;
        for (int i = 0; i < 2'000'000; ++i) big.push_front(i);
        assert(big.size() == 2'000'000);
        big.reverse();
    }   // destroyed iteratively here
    std::printf("2M-node list built, reversed and destroyed without stack overflow\n");
    std::printf("all singly-list checks passed\n");
    return 0;
}
