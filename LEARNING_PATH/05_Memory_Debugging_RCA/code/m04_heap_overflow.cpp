// m04_heap_overflow.cpp - classic off-by-one: a loop uses "<=" instead of "<" and writes one
// element past the end of a heap array.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_heap_overflow m04_heap_overflow.cpp
// Run:    ./build/m04_heap_overflow 10
//         Often prints "done" and exits normally, or aborts inside free() with a glibc message
//         such as "free(): invalid next size" - the symptom appears at deallocation time, not
//         at the moment of the bad write. That delay is the whole lesson of module 03.
//
// ASan build (see build_debug.sh): reports immediately at the bad write:
//         ERROR: AddressSanitizer: heap-buffer-overflow on address ...
//         WRITE of size 4 at ... thread T0
//             #0 fill_totals m04_heap_overflow.cpp:NN
//         0x... is located 0 bytes after 40-byte region [...) allocated by thread T0 here:
//             #0 operator new[](unsigned long)
//             #1 main m04_heap_overflow.cpp:NN
//
// Valgrind: "Invalid write of size 4 ... Address ... is 0 bytes after a block of size 40 alloc'd"
//
// Warnings with -Wall -Wextra: none, because the count comes from argv. If you replace `n` with
// the literal 10 and build with -O2, GCC's -Warray-bounds (part of -Wall) does catch it.
#include <cstdio>
#include <cstdlib>

static void fill_totals(int *totals, int n)
{
    for (int i = 0; i <= n; ++i)        // BUG: should be i < n
        totals[i] = i * 100;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? std::atoi(argv[1]) : 10;
    int *totals = new int[n];
    int *other  = new int[n];           // neighbouring allocation: the stray write may land here

    fill_totals(totals, n);

    long sum = 0;
    for (int i = 0; i < n; ++i) sum += totals[i];
    std::printf("sum %ld\n", sum);

    delete[] other;                     // glibc may detect inconsistent bookkeeping here
    delete[] totals;
    std::puts("done");
    return 0;
}
