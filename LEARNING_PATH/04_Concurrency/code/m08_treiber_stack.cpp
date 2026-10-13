// m08_treiber_stack.cpp — lock-free stack. Two versions:
//   default : pooled nodes + 32-bit index + 32-bit tag packed in one 64-bit word. No ABA, no UAF.
//   -DBROKEN: the textbook naive version with new/delete. Has use-after-free and ABA.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m08_treiber_stack.cpp -o m08c
//        g++ -std=c++17 -g -O1 -DBROKEN -fsanitize=address -pthread m08_treiber_stack.cpp -o m08c_broken
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>

#ifdef BROKEN
// ---------------------------------------------------------------------------------------------
// Naive Treiber stack. pop() reads old->next after another thread may have deleted old (UAF),
// and its CAS can succeed on a recycled address with a different next (ABA).
// ---------------------------------------------------------------------------------------------
template <class T>
class Stack {
    struct Node { T value; Node* next; };
    std::atomic<Node*> head_{nullptr};
public:
    ~Stack() { T tmp; while (pop(tmp)) {} }
    bool push(T v) {
        Node* n = new Node{std::move(v), nullptr};
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
        return true;
    }
    bool pop(T& out) {
        Node* old = head_.load(std::memory_order_acquire);
        while (old && !head_.compare_exchange_weak(old, old->next,            // <-- reads freed memory
                       std::memory_order_acquire, std::memory_order_acquire)) {}
        if (!old) return false;
        out = std::move(old->value);
        delete old;                                                            // <-- the cause
        return true;
    }
};
using StackT = Stack<long>;
#else
// ---------------------------------------------------------------------------------------------
// Pooled, tagged Treiber stack. Nodes live in a fixed array and are never freed, so a stale
// reader always dereferences valid memory. Every CAS bumps a 32-bit tag, so a recycled index
// cannot satisfy a stale compare (ABA defeated). Two lists share the pool: the stack and the
// free list, both Treiber lists with the same protocol.
// ---------------------------------------------------------------------------------------------
template <class T, std::uint32_t Capacity>
class Stack {
    static constexpr std::uint32_t NIL = 0xFFFFFFFFu;
    struct Node {
        T value{};
        std::atomic<std::uint32_t> next{NIL};   // WHY atomic: a stale popper may read it concurrently
    };
    std::unique_ptr<Node[]> pool_;
    std::atomic<std::uint64_t> head_;   // packed {tag:32, index:32}
    std::atomic<std::uint64_t> free_;

    static std::uint64_t pack(std::uint32_t idx, std::uint32_t tag) {
        return (static_cast<std::uint64_t>(tag) << 32) | idx;
    }
    static std::uint32_t index_of(std::uint64_t p) { return static_cast<std::uint32_t>(p & 0xFFFFFFFFu); }
    static std::uint32_t tag_of(std::uint64_t p)   { return static_cast<std::uint32_t>(p >> 32); }

    std::uint32_t pop_list(std::atomic<std::uint64_t>& list) {
        std::uint64_t old = list.load(std::memory_order_acquire);
        for (;;) {
            std::uint32_t idx = index_of(old);
            if (idx == NIL) return NIL;
            std::uint32_t next = pool_[idx].next.load(std::memory_order_relaxed);   // valid memory always
            std::uint64_t desired = pack(next, tag_of(old) + 1);                      // WHY tag+1: defeats ABA
            if (list.compare_exchange_weak(old, desired, std::memory_order_acq_rel, std::memory_order_acquire))
                return idx;
        }
    }
    void push_list(std::atomic<std::uint64_t>& list, std::uint32_t idx) {
        std::uint64_t old = list.load(std::memory_order_relaxed);
        for (;;) {
            pool_[idx].next.store(index_of(old), std::memory_order_relaxed);
            std::uint64_t desired = pack(idx, tag_of(old) + 1);
            if (list.compare_exchange_weak(old, desired, std::memory_order_release, std::memory_order_relaxed))
                return;
        }
    }
public:
    static_assert(Capacity < NIL, "capacity must fit in 32 bits minus NIL");
    Stack() : pool_(new Node[Capacity]), head_(pack(NIL, 0)), free_(pack(0, 0)) {
        for (std::uint32_t i = 0; i < Capacity; ++i)
            pool_[i].next.store(i + 1 < Capacity ? i + 1 : NIL, std::memory_order_relaxed);
    }
    bool push(T v) {
        std::uint32_t idx = pop_list(free_);
        if (idx == NIL) return false;                       // pool exhausted
        pool_[idx].value = std::move(v);                    // exclusive: we own idx until we publish it
        push_list(head_, idx);
        return true;
    }
    bool pop(T& out) {
        std::uint32_t idx = pop_list(head_);
        if (idx == NIL) return false;
        out = std::move(pool_[idx].value);                  // exclusive: CAS success handed us idx
        push_list(free_, idx);
        return true;
    }
    static constexpr bool is_lock_free_head() { return std::atomic<std::uint64_t>::is_always_lock_free; }
};
using StackT = Stack<long, 1024>;
#endif

int main() {
    StackT st;
    const int threads = 4, per = 1'000'000;
    std::vector<long> pushed(threads), popped(threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            long ps = 0, pp = 0;
            for (int i = 0; i < per; ++i) {
                long v = static_cast<long>(t) * per + i;
                while (!st.push(v)) std::this_thread::yield();   // never full here: occupancy <= threads
                ps += v;
                long out;
                while (!st.pop(out)) std::this_thread::yield();  // never empty for long: we just pushed
                pp += out;                                       // WHY sums: with 4 threads interleaving,
            }                                                    // you pop others' values; totals must match
            pushed[t] = ps; popped[t] = pp;
        });
    for (auto& t : ts) t.join();
    long sp = 0, so = 0;
    for (int t = 0; t < threads; ++t) { sp += pushed[t]; so += popped[t]; }
    long leftover; bool empty = !st.pop(leftover);
    std::cout << "pushed sum=" << sp << " popped sum=" << so << (sp == so ? " (ok)" : " (MISMATCH)")
              << " stack empty at end: " << (empty ? "yes" : "no") << '\n';
#ifndef BROKEN
    std::cout << "head word always lock-free: " << (StackT::is_lock_free_head() ? "yes" : "no") << '\n';
#else
    std::cout << "(BROKEN build: run under -fsanitize=address to see heap-use-after-free)\n";
#endif
    return (sp == so && empty) ? 0 : 1;
}
