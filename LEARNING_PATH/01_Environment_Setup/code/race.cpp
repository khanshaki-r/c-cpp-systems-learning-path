// race.cpp - a textbook data race for ThreadSanitizer.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=thread race.cpp -o race_tsan
// Run  : ./race_tsan        (expect "WARNING: ThreadSanitizer: data race")
// Fix  : uncomment the lock_guard line, rebuild, rerun: no warning, counter == 2000000.
#include <iostream>
#include <mutex>
#include <thread>

int counter = 0;
std::mutex counter_mutex;

void bump(int times) {
    for (int i = 0; i < times; ++i) {
        // std::lock_guard<std::mutex> guard(counter_mutex);   // <- the fix
        ++counter;  // read-modify-write from two threads without synchronisation = data race (UB)
    }
}

int main() {
    std::thread a(bump, 1'000'000);
    std::thread b(bump, 1'000'000);
    a.join();
    b.join();
    std::cout << "counter = " << counter << " (expected 2000000)\n";
    return 0;
}
