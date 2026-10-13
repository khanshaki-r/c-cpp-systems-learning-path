// m02_race_demo.cpp — deliberate data races: lost updates and torn reads.
// Build:       g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m02_race_demo.cpp -o race
// With TSan:   g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m02_race_demo.cpp -o race_tsan
// This program contains undefined behaviour ON PURPOSE. TSan will report both races.
#include <thread>
#include <iostream>
#include <vector>
#include <atomic>
#include <cstdint>

// Demo 1: lost updates. Two threads increment a plain int. Expected 2,000,000; you will get less.
int plain_counter = 0;

void bump_plain(int n) {
    for (int i = 0; i < n; ++i) ++plain_counter;   // load, add, store: three steps, interleavable
}

// Demo 2: tearing. A 16-byte record written as two 8-byte halves. Reader checks invariant a == b.
struct Record { std::int64_t a; std::int64_t b; };
Record rec{0, 0};
std::atomic<bool> stop{false};

void writer_tear() {
    std::int64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        ++v;
        rec.a = v;        // WHY two separate stores: this is what any struct assignment compiles to
        rec.b = v;        // reader may observe a == v, b == v-1
    }
}

long reader_tear() {
    long torn = 0;
    for (int i = 0; i < 5'000'000; ++i) {
        Record snapshot = rec;         // plain copy: itself racy; this is the demo
        if (snapshot.a != snapshot.b) ++torn;
    }
    return torn;
}

int main() {
    // Demo 1
    std::thread t1(bump_plain, 1'000'000);
    std::thread t2(bump_plain, 1'000'000);
    t1.join(); t2.join();
    std::cout << "plain counter: " << plain_counter << " (expected 2000000)\n";

    // Demo 2
    std::thread w(writer_tear);
    long torn = reader_tear();
    stop.store(true, std::memory_order_relaxed);
    w.join();
    std::cout << "torn reads observed: " << torn << '\n';

    // Both demos are undefined behaviour on purpose. Compile with -fsanitize=thread and it will
    // report both races with the two stacks involved. That is the point of this file.
    return 0;
}
