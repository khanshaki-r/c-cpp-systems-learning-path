// m01_threads_basics.cpp — create, join, detach, pass-by-reference, thread_local.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m01_threads_basics.cpp -o m01
#include <thread>
#include <vector>
#include <iostream>
#include <mutex>
#include <chrono>
#include <functional>
#include <utility>

// A tiny RAII wrapper so a thread can never be destroyed while joinable.
// C++20 has std::jthread; in C++17 we write this ourselves.
class joining_thread {
    std::thread t_;
public:
    template <class F, class... Args>
    explicit joining_thread(F&& f, Args&&... args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...) {}
    ~joining_thread() { if (t_.joinable()) t_.join(); }   // WHY: never terminate() by accident
    joining_thread(const joining_thread&) = delete;        // threads are move-only
    joining_thread& operator=(const joining_thread&) = delete;
    joining_thread(joining_thread&&) = default;
    joining_thread& operator=(joining_thread&&) = default;
    std::thread& get() { return t_; }
};

// thread_local: one instance per thread, constructed on first use in that thread.
thread_local int call_count = 0;

std::mutex cout_mtx;   // std::cout will not interleave characters, but lines can; serialize.

void worker(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) ++call_count;   // no race: call_count is per-thread
    std::lock_guard<std::mutex> lk(cout_mtx);
    std::cout << "worker " << id << " os-tid " << std::this_thread::get_id()
              << " local count " << call_count << '\n';
}

int main() {
    unsigned hw = std::thread::hardware_concurrency();   // 0 means "unknown"
    std::cout << "hardware threads: " << hw << '\n';

    // 1. Create and join. Arguments are copied into the new thread.
    std::vector<joining_thread> pool;
    for (int i = 0; i < 4; ++i) pool.emplace_back(worker, i, 1000 * (i + 1));
    pool.clear();   // joins all four; destructors run here

    // 2. Pass by reference on purpose with std::ref. Without it, the thread gets a copy of
    //    'total' and the main thread never sees the result.
    long total = 0;
    std::thread t([](long& out) {
        for (int i = 1; i <= 100; ++i) out += i;
    }, std::ref(total));
    t.join();
    std::cout << "sum via ref: " << total << '\n';   // 5050

    // 3. Detach: fire and forget. The thread must not touch anything that main might destroy.
    //    Here it only uses its own stack and a copy of a value, which is the only safe pattern.
    std::thread bg([](int v) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)v;   // pretend to do work
    }, 42);
    bg.detach();
    // main could exit before bg finishes; the process ends and bg is killed. Acceptable only
    // because bg owns nothing.

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout << "main thread local count: " << call_count << '\n';   // 0: never incremented here
    return 0;
}
