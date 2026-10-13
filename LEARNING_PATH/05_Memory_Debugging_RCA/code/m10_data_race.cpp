// m10_data_race.cpp - two threads update a shared counter and a shared std::map without a lock.
// The counter gives wrong totals; the map eventually crashes with a garbage pointer.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O1 -o build/m10_data_race m10_data_race.cpp
// Run:    ./build/m10_data_race
//         "expected 2000000 got 1xxxxxx" most runs. Sometimes a crash inside
//         std::_Rb_tree_insert_and_rebalance or free(): the tree got two writers.
//
// TSan build (see build_debug.sh; cannot combine with ASan):
//         WARNING: ThreadSanitizer: data race (pid=NNN)
//           Write of size 8 at 0x... by thread T2:
//             #0 worker(int) m10_data_race.cpp:NN
//           Previous write of size 8 at 0x... by thread T1:
//             #0 worker(int) m10_data_race.cpp:NN
//           Location is global 'g_total' of size 8 at 0x...
//           Thread T2 (tid=NNN, running) created by main thread at: ...
//         SUMMARY: ThreadSanitizer: data race m10_data_race.cpp:NN in worker(int)
//
// Helgrind:  valgrind --tool=helgrind ./build/m10_data_race
//         "Possible data race during write of size 8 ... This conflicts with a previous write"
//
// Fix: std::atomic<long> for the counter (or a mutex), and a mutex around the map. See
// 04_Concurrency for the full treatment.
//
// Warnings with -Wall -Wextra: none.
#include <cstdio>
#include <map>
#include <thread>

static long                g_total = 0;        // shared, unprotected
static std::map<int, long> g_by_key;           // shared, unprotected; not thread-safe for writes

static void worker(int tid)
{
    for (int i = 0; i < 1'000'000; ++i) {
        g_total += 1;                          // read-modify-write, not atomic -> lost updates
        if (i % 1000 == 0)
            g_by_key[(tid * 7 + i) % 97] += 1; // two threads mutate one red-black tree
    }
}

int main()
{
    std::thread a(worker, 1), b(worker, 2);
    a.join(); b.join();
    std::printf("expected 2000000 got %ld  (map size %zu)\n", g_total, g_by_key.size());
    return 0;
}
