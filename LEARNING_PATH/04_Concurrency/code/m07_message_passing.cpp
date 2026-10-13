// m07_message_passing.cpp — publish data through a release/acquire flag, and the store-buffering
// litmus test that separates acquire/release from seq_cst.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m07_message_passing.cpp -o m07b
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>
#include <cassert>
#include <functional>

// Part 1: publish non-atomic data through an atomic flag with release/acquire.
struct Payload { int a; int b; long c; };
Payload payload{};                       // plain data, written by producer before publish
std::atomic<bool> ready{false};

void producer() {
    payload = Payload{1, 2, 3};                           // sequenced-before the release store
    ready.store(true, std::memory_order_release);        // seal the envelope
}
void consumer(long& out) {
    while (!ready.load(std::memory_order_acquire)) {}    // open the envelope
    out = payload.a + payload.b + payload.c;             // guaranteed 6; no data race
}

// Part 2: store buffering. Count how often both threads read 0.
template <std::memory_order StoreMO, std::memory_order LoadMO>
long store_buffering(int iterations) {
    long both_zero = 0;
    for (int it = 0; it < iterations; ++it) {
        std::atomic<int> x{0}, y{0};
        std::atomic<int> start{0};
        int r1 = -1, r2 = -1;
        std::thread t1([&] {
            start.fetch_add(1); while (start.load() < 2) {}     // WHY barrier: both start together
            x.store(1, StoreMO);
            r1 = y.load(LoadMO);
        });
        std::thread t2([&] {
            start.fetch_add(1); while (start.load() < 2) {}
            y.store(1, StoreMO);
            r2 = x.load(LoadMO);
        });
        t1.join(); t2.join();
        if (r1 == 0 && r2 == 0) ++both_zero;
    }
    return both_zero;
}

int main() {
    long sum = 0;
    std::thread c(consumer, std::ref(sum));
    std::thread p(producer);
    p.join(); c.join();
    std::cout << "published sum: " << sum << '\n';
    assert(sum == 6);

    const int iters = 20000;   // thread creation dominates; keep modest (exercise 3 improves this)
    std::cout << "store-buffering (release/acquire): both-zero "
              << store_buffering<std::memory_order_release, std::memory_order_acquire>(iters)
              << " / " << iters << "  (non-zero is legal and happens on x86)\n";
    std::cout << "store-buffering (seq_cst):         both-zero "
              << store_buffering<std::memory_order_seq_cst, std::memory_order_seq_cst>(iters)
              << " / " << iters << "  (must be 0)\n";
    return 0;
}
