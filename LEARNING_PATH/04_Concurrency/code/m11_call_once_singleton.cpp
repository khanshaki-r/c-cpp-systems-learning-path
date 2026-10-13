// m11_call_once_singleton.cpp — three correct lazy-initialisation forms, each stress-tested by
// 16 threads released at the same instant. Every form must print "constructed: 1".
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m11_call_once_singleton.cpp -o m11_once
// TSan:  g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m11_call_once_singleton.cpp -o m11_once_tsan
// The broken plain-pointer double-checked version is deliberately NOT here: exercise 1 asks you to
// write it yourself and watch TSan catch it.
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Counts constructions per form. Slow constructor widens the window in which a broken
// implementation would let a second thread in.
template <int Form>
struct Table {
    static std::atomic<int> constructed;
    long data[256];
    Table() {
        constructed.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (int i = 0; i < 256; ++i) data[i] = i;   // readers check this: proves publication
    }
    bool ok() const {
        for (int i = 0; i < 256; ++i) if (data[i] != i) return false;
        return true;
    }
};
template <int Form> std::atomic<int> Table<Form>::constructed{0};

// ---- Form 1: double-checked locking done right (atomic pointer, acquire/release) ----------------
std::atomic<Table<1>*> instance1{nullptr};
std::mutex init_mtx1;
Table<1>* get1() {
    Table<1>* p = instance1.load(std::memory_order_acquire);   // fast path: one load
    if (!p) {
        std::lock_guard<std::mutex> lk(init_mtx1);
        p = instance1.load(std::memory_order_relaxed);           // WHY relaxed: the mutex orders it
        if (!p) {
            p = new Table<1>();
            instance1.store(p, std::memory_order_release);       // publishes the constructed fields
        }
    }
    return p;
}

// ---- Form 2: std::call_once ---------------------------------------------------------------------
std::once_flag flag2;
Table<2>* instance2 = nullptr;   // plain pointer is fine: call_once gives happens-before to every caller
Table<2>* get2() {
    std::call_once(flag2, [] { instance2 = new Table<2>(); });
    return instance2;
}

// ---- Form 3: function-local static ("magic static") ---------------------------------------------
Table<3>& get3() {
    static Table<3> t;   // compiler emits __cxa_guard_acquire / release around this
    return t;
}

// Releases N threads at once (spin on an atomic go flag), each calls get() and checks the object.
template <class Get>
void stress(const char* name, Get get, int threads, std::atomic<int>& constructed) {
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (!get()) bad.fetch_add(1);
        });
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    std::cout << name << " constructed: " << constructed.load()
              << "  bad reads: " << bad.load() << '\n';
}

// ---- Bonus: call_once retries after an exception ------------------------------------------------
std::once_flag flag_retry;
int attempts = 0;
void init_that_fails_once() {
    std::call_once(flag_retry, [] {
        ++attempts;   // WHY no lock: call_once runs at most one initialiser at a time
        if (attempts == 1) throw std::runtime_error("first attempt fails");
    });
}

int main() {
    constexpr int kThreads = 16;
    stress("[1] atomic DCLP  ", [] { return get1()->ok(); }, kThreads, Table<1>::constructed);
    stress("[2] call_once    ", [] { return get2()->ok(); }, kThreads, Table<2>::constructed);
    stress("[3] magic static ", [] { return get3().ok(); }, kThreads, Table<3>::constructed);

#ifdef __SANITIZE_THREAD__
    // libstdc++ implements call_once on top of pthread_once. TSan replaces pthread_once with its
    // own version, which does not reset the flag when the initialiser exits by exception, so the
    // retry below would hang forever under TSan. This is the "call_once + exceptions" history the
    // module mentions, live. The plain build demonstrates the standard behaviour.
    std::cout << "[4] skipped under TSan (its pthread_once does not survive a throwing initialiser)\n";
#else
    try { init_that_fails_once(); } catch (const std::exception& e) {
        std::cout << "[4] first call threw: " << e.what() << '\n';
    }
    init_that_fails_once();   // flag still unset, so the initialiser runs again and succeeds
    std::cout << "[4] call_once attempts after retry: " << attempts << " (expected 2)\n";
#endif

    delete instance1.load();   // tidy for ASan/valgrind; real singletons usually leak on purpose
    delete instance2;
    return 0;
}
