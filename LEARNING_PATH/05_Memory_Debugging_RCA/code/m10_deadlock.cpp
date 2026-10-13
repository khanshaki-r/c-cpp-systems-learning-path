// m10_deadlock.cpp - lock-order inversion between two mutexes. The program hangs forever.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m10_deadlock m10_deadlock.cpp
// Run:    ./build/m10_deadlock      -> prints a few lines, then hangs. Ctrl-C to kill.
//
// Diagnose a live hang:
//         ./build/m10_deadlock &  ;  gdb -p $!            (ptrace_scope: see module 05)
//         (gdb) thread apply all bt
//           Thread 3: ... __lll_lock_wait <- pthread_mutex_lock <- std::mutex::lock <- transfer_b_to_a
//           Thread 2: ... __lll_lock_wait <- pthread_mutex_lock <- std::mutex::lock <- transfer_a_to_b
//         (gdb) thread 2 ; frame <N where the mutex is visible> ; p g_sessions_mtx
//           $1 = { _M_mutex = { __data = { __lock = 2, __owner = 12345, ... } } }
//           __owner is the kernel TID (LWP) of the thread that HOLDS it. Compare with
//         (gdb) info threads        -> "Thread 0x7f... (LWP 12345)" is the holder.
//         Thread 2 holds sessions and waits for connections; thread 3 holds connections and
//         waits for sessions: a cycle. That is a deadlock, and the dump proves it.
//
// Or get a core of the hang without killing it:  gcore -o hang $!   then gdb ./m10_deadlock hang.*
//
// TSan: with TSAN_OPTIONS=detect_deadlocks=1 it reports "potential deadlock: lock-order-inversion
//       (potential deadlock)" with both acquisition orders - even on a run that did not hang.
//
// Fix: one global lock order (always sessions then connections), or std::scoped_lock(m1, m2)
// which locks both with a deadlock-avoidance algorithm. See 04_Concurrency.
//
// Warnings with -Wall -Wextra: none.
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>     // getpid

static std::mutex g_sessions_mtx;       // protects the session table
static std::mutex g_connections_mtx;    // protects the connection list

static void transfer_a_to_b()
{
    for (int i = 0; i < 100000; ++i) {
        std::lock_guard<std::mutex> l1(g_sessions_mtx);        // order: sessions -> connections
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        std::lock_guard<std::mutex> l2(g_connections_mtx);
        if (i % 20000 == 0) std::printf("a->b %d\n", i);
    }
}

static void transfer_b_to_a()
{
    for (int i = 0; i < 100000; ++i) {
        std::lock_guard<std::mutex> l1(g_connections_mtx);     // order: connections -> sessions (!)
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        std::lock_guard<std::mutex> l2(g_sessions_mtx);
        if (i % 20000 == 0) std::printf("b->a %d\n", i);
    }
}

int main()
{
    std::printf("pid %d - attach with: gdb -p %d\n", (int)getpid(), (int)getpid());
    std::thread t1(transfer_a_to_b), t2(transfer_b_to_a);
    t1.join(); t2.join();
    std::puts("finished (you will rarely see this)");
    return 0;
}
