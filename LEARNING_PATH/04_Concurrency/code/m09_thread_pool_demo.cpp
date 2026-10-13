// m09_thread_pool_demo.cpp — results, exceptions across threads, backpressure, drain on shutdown,
// broken_promise on shutdown_now.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m09_thread_pool_demo.cpp -o m09
#include "m09_thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    // ---- 1. Typed results, exceptions, argument forwarding, backpressure --------------------
    {
        ThreadPool pool(4, /*max_queue=*/16);

        std::vector<std::future<long>> futs;
        for (long i = 0; i < 100; ++i) futs.push_back(pool.submit([i] { return i * i; }));
        long sum = 0;
        for (auto& f : futs) sum += f.get();
        std::cout << "sum of squares 0..99: " << sum << " (expected 328350)\n";

        auto bad = pool.submit([]() -> int { throw std::runtime_error("bad request"); });
        try {
            bad.get();
            std::cout << "no exception?\n";
        } catch (const std::runtime_error& e) {
            std::cout << "caught on submitter thread: " << e.what() << '\n';
        }
        auto good = pool.submit([] { return 7; });
        std::cout << "worker survived the throw: " << good.get() << '\n';

        auto f2 = pool.submit([](int a, int b) { return a + b; }, 20, 22);
        std::cout << "with forwarded args: " << f2.get() << '\n';

        std::atomic<int> started{0};
        std::vector<std::future<void>> slow;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 40; ++i)
            slow.push_back(pool.submit([&started] {
                started.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(5ms);
            }));
        auto submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
        std::cout << "40 x 5ms tasks, 4 workers, bound 16: submit loop blocked for ~" << submit_ms
                  << " ms (backpressure), pending now " << pool.pending() << '\n';
        for (auto& f : slow) f.get();
        std::cout << "all slow tasks ran: " << started.load() << '\n';
    }

    // ---- 2. Graceful shutdown drains everything that was submitted --------------------------
    {
        std::atomic<long> ran{0};
        ThreadPool pool(3);
        for (int i = 0; i < 1000; ++i)
            pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        pool.shutdown();   // destructor would do the same
        std::cout << "after shutdown: ran=" << ran.load() << " completed=" << pool.completed()
                  << " pending=" << pool.pending() << " (expected 1000, 1000, 0)\n";
    }

    // ---- 3. shutdown_now abandons queued work; futures report broken_promise ---------------
    {
        ThreadPool pool(1);
        std::promise<void> started;
        auto started_f = started.get_future();
        auto blocker = pool.submit([&started] {
            started.set_value();
            std::this_thread::sleep_for(50ms);
            return 1;
        });
        auto abandoned = pool.submit([] { return 2; });
        started_f.wait();          // WHY: be sure the worker is inside 'blocker' before we abandon
        pool.shutdown_now();
        std::cout << "running task finished with: " << blocker.get() << '\n';
        try {
            abandoned.get();
            std::cout << "abandoned task ran?\n";
        } catch (const std::future_error& e) {
            std::cout << "abandoned future: " << e.what()
                      << (e.code() == std::future_errc::broken_promise ? " [broken_promise]" : "") << '\n';
        }
    }

    // ---- 4. Submit after shutdown is refused ----------------------------------------------
    {
        ThreadPool pool(2);
        pool.shutdown();
        try {
            pool.submit([] { return 0; });
            std::cout << "submit after shutdown accepted?\n";
        } catch (const std::runtime_error& e) {
            std::cout << "submit after shutdown: " << e.what() << '\n';
        }
    }
    return 0;
}
