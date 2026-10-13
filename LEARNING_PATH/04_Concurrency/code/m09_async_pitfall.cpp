// m09_async_pitfall.cpp — three std::async surprises: the blocking destructor of a discarded
// future, the deferred policy that never runs until get(), and one-thread-per-call with no pool.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m09_async_pitfall.cpp -o m09_async
// Run:   ./m09_async
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static long ms_since(Clock::time_point t0) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

static void slow_job(int id) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // stand-in for real work
    std::cout << "  job " << id << " finished\n";
}

int main() {
    // ---- Pitfall 1: discarding the future makes the call synchronous --------------------------
    // The temporary future is destroyed at the semicolon. A future that came from
    // std::async(launch::async) joins its thread in the destructor. So three "background" jobs
    // run one after another: ~600 ms, not ~200 ms.
    std::cout << "[1] discarded futures (looks parallel, is sequential)\n";
    auto t0 = Clock::now();
    for (int i = 0; i < 3; ++i) {
        // WHY the cast: GCC marks std::async [[nodiscard]]; the cast keeps -Werror quiet while
        // keeping the exact bug we want to show (the temporary still dies here).
        static_cast<void>(std::async(std::launch::async, slow_job, i));
    }
    std::cout << "  elapsed: " << ms_since(t0) << " ms (expected ~600: sequential)\n";

    // The fix: keep the futures alive until you actually want to wait.
    std::cout << "[1b] futures kept alive (really parallel)\n";
    t0 = Clock::now();
    {
        std::vector<std::future<void>> keep;
        for (int i = 0; i < 3; ++i) keep.push_back(std::async(std::launch::async, slow_job, i));
        for (auto& f : keep) f.get();   // WHY get(): also rethrows any exception from the job
    }
    std::cout << "  elapsed: " << ms_since(t0) << " ms (expected ~200: parallel)\n";

    // ---- Pitfall 2: launch::deferred never runs on its own -------------------------------------
    // A wait_for loop polling for ready() spins forever: the status is 'deferred', not 'timeout'.
    // We bound the loop so the demo terminates, and show the status we get.
    std::cout << "[2] deferred policy\n";
    std::atomic<bool> ran{false};
    auto d = std::async(std::launch::deferred, [&ran] { ran = true; return 42; });
    int polls = 0;
    std::future_status st = std::future_status::timeout;
    while (polls < 5) {
        st = d.wait_for(std::chrono::milliseconds(10));
        if (st == std::future_status::ready) break;
        if (st == std::future_status::deferred) {
            std::cout << "  poll " << polls << ": status=deferred, task has not run (ran="
                      << ran.load() << ")\n";
        }
        ++polls;
    }
    // Correct handling: a deferred status means "nobody will run this unless you call get()".
    int v = d.get();   // runs the lambda now, on THIS thread
    std::cout << "  after get(): value=" << v << " ran=" << ran.load() << '\n';

    // ---- Pitfall 3: no pool, no bound ----------------------------------------------------------
    // libstdc++ creates a brand-new thread for each launch::async call. Count distinct thread ids.
    std::cout << "[3] one thread per call\n";
    std::mutex m;
    std::set<std::thread::id> ids;
    {
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 32; ++i)
            fs.push_back(std::async(std::launch::async, [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));   // overlap them
                std::lock_guard<std::mutex> lk(m);
                ids.insert(std::this_thread::get_id());
            }));
        for (auto& f : fs) f.get();
    }
    std::cout << "  32 calls ran on " << ids.size()
              << " distinct threads (a pool of 4 would show 4)\n";
    std::cout << "  lesson: std::async is fine for a one-off in a tool; a server needs a pool\n";
    return 0;
}
