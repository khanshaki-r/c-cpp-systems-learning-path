// m12_tsan_demo.cpp — four seeded concurrency bugs, one per command-line mode, so each sanitizer
// report is small and readable. This file contains undefined behaviour ON PURPOSE.
//
// Build (plain, proves it compiles cleanly):
//   g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m12_tsan_demo.cpp -o m12
// ThreadSanitizer:
//   g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m12_tsan_demo.cpp -o tsan_demo
//   ./tsan_demo race      data race on an unlocked counter
//   ./tsan_demo order     lock-order-inversion (the threads run one after another: no real hang)
//   ./tsan_demo wakeup    race on a condition flag written outside the mutex
// AddressSanitizer:
//   g++ -std=c++17 -g -O1 -fsanitize=address -pthread m12_tsan_demo.cpp -o asan_demo
//   ./asan_demo uaf       heap-use-after-free across threads
// Race points (deterministic window widening, see module 12 section 2):
//   g++ -std=c++17 -g -O1 -DRACE_POINTS -pthread m12_tsan_demo.cpp -o rp_demo
//   RACE_POINTS=wakeup-gap ./rp_demo wakeup     forces the lost wakeup every run
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// ---- race-point machinery --------------------------------------------------------------------
// Returns true if `name` appears in the comma-separated RACE_POINTS environment variable.
[[maybe_unused]] static bool race_point_enabled(const char* name) {
    const char* env = std::getenv("RACE_POINTS");
    if (!env) return false;
    std::string list = std::string(",") + env + ",";
    return list.find(std::string(",") + name + ",") != std::string::npos;
}

#ifdef RACE_POINTS
#define RACE_POINT(name) do { if (race_point_enabled(name)) std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (0)
#else
#define RACE_POINT(name) do {} while (0)
#endif

// ---- Bug 1: unlocked counter -----------------------------------------------------------------
// Fix: std::atomic<long> or a mutex around ++.
long hits = 0;
std::mutex hits_mtx;   // exists, but one of the two threads "forgets" to use it

void demo_race() {
    std::thread a([] { for (int i = 0; i < 100000; ++i) { std::lock_guard<std::mutex> lk(hits_mtx); ++hits; } });
    std::thread b([] { for (int i = 0; i < 100000; ++i) ++hits; });   // BUG: no lock
    a.join(); b.join();
    std::cout << "hits = " << hits << " (expected 200000)\n";
}

// ---- Bug 2: lock-order inversion -------------------------------------------------------------
// T1 takes cache then conn; T2 takes conn then cache. They run sequentially here, so nothing
// hangs, but TSan records the lock graph and reports the cycle anyway. That is the point: it
// finds the deadlock that did not happen this time. Fix: one global order.
std::mutex m_cache, m_conn;

void demo_order() {
    std::thread t1([] {
        std::lock_guard<std::mutex> a(m_cache);
        std::lock_guard<std::mutex> b(m_conn);
    });
    t1.join();
    std::thread t2([] {
        std::lock_guard<std::mutex> a(m_conn);    // BUG: opposite order
        std::lock_guard<std::mutex> b(m_cache);
    });
    t2.join();
    std::cout << "order demo finished (no hang this run; TSan still reports the cycle)\n";
}

// ---- Bug 3: flag written outside the mutex (lost wakeup) -------------------------------------
// The setter writes `ready` and notifies WITHOUT holding the mutex. If it does both inside the
// gap between the waiter's predicate check and its block, the notify hits nobody and the waiter
// sleeps until its timeout. Fix: set the flag under the lock (notify may stay outside).
std::mutex wm;
std::condition_variable wcv;
bool ready = false;   // BUG: written without wm below

std::atomic<bool> waiter_has_lock{false};   // test scaffolding only: orders the demo's start

void demo_wakeup() {
    std::thread waiter([] {
        auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lk(wm);
        waiter_has_lock.store(true);
        bool timed_out = false;
        while (!ready) {                          // inlined predicate loop so we can place the race point
            RACE_POINT("wakeup-gap");             // window between "checked: not ready" and "blocked"
            if (wcv.wait_for(lk, std::chrono::milliseconds(500)) == std::cv_status::timeout) {
                timed_out = true;                 // with wait() instead of wait_for() this hangs forever
                break;
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        std::cout << (timed_out ? "LOST WAKEUP: notify missed, rescued only by the timeout"
                                : "woken normally")
                  << " after " << ms << " ms\n";
    });
    while (!waiter_has_lock.load()) std::this_thread::yield();
    ready = true;            // BUG: no lock_guard(wm)
    wcv.notify_one();
    waiter.join();
}

// ---- Bug 4: use-after-free across threads ----------------------------------------------------
// The owner deletes a request while a worker still holds a raw pointer to it. Fix: shared_ptr
// for shared ownership, or join/handshake before delete so ownership is unambiguous.
struct Request { char payload[64]; int id; };
Request* g_req = nullptr;   // raw pointer shared between threads: nobody "owns" it clearly

void demo_uaf() {
    g_req = new Request{};
    std::strcpy(g_req->payload, "transfer 100");
    g_req->id = 7;
    std::thread worker([] {
        Request* r = g_req;                                       // takes a copy of the raw pointer
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "worker sees id " << r->id << '\n';         // BUG: freed by main by now
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    delete g_req;                                                 // owner frees while worker holds it
    worker.join();
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "race")        demo_race();
    else if (mode == "order")  demo_order();
    else if (mode == "wakeup") demo_wakeup();
    else if (mode == "uaf")    demo_uaf();
    else {
        std::cerr << "usage: " << argv[0] << " race|order|wakeup|uaf\n"
                  << "  race, order, wakeup: build with -fsanitize=thread\n"
                  << "  uaf: build with -fsanitize=address\n";
        return 2;
    }
    return 0;
}
