// m09_growth_not_leak.cpp - RSS stays high after everything is freed. Not a leak: it is the
// allocator keeping freed memory, plus fragmentation. Leak tools report nothing.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m09_growth m09_growth_not_leak.cpp
// Run:    ./build/m09_growth
//         Prints VmRSS at four points: start, after allocating ~200 MiB in small blocks, after
//         freeing all but one block per 1000, after malloc_trim(0).
//
// What to expect (typical, glibc 2.39):
//   - after free: RSS barely drops. Freed small chunks go to bins; the heap cannot be
//     trimmed below the highest still-live chunk, and every page holding one live 64-byte block
//     stays resident.
//   - after malloc_trim(0): RSS drops a lot. glibc 2.8+ can madvise(MADV_DONTNEED) whole free
//     pages in the middle of the heap, not just trim the top.
//   - Valgrind/LSan at exit: zero leaks. Everything is reachable or freed.
//
// Lesson: "memory went up and never came down" is not proof of a leak. Measure live bytes
// (malloc_stats / mallinfo2 / heaptrack) and not just RSS.
//
// Warnings with -Wall -Wextra: none.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>     // malloc_trim, malloc_stats (glibc)
#include <vector>

static long rss_kib()
{
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long kib = -1;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "VmRSS: %ld kB", &kib) == 1) break;
    std::fclose(f);
    return kib;
}

static void report(const char *stage)
{
    std::printf("%-28s VmRSS = %6ld MiB\n", stage, rss_kib() / 1024);
}

int main()
{
    report("start");

    const size_t count = 3'000'000, size = 64;            // ~200 MiB of 64-byte blocks
    std::vector<char *> blocks(count);
    for (size_t i = 0; i < count; ++i) {
        blocks[i] = static_cast<char *>(std::malloc(size));
        std::memset(blocks[i], 1, size);                   // touch, so pages become resident
    }
    report("after 3M x 64B malloc");

    for (size_t i = 0; i < count; ++i)
        if (i % 1000 != 0) { std::free(blocks[i]); blocks[i] = nullptr; }   // keep 1 in 1000 alive
    report("after freeing 99.9%");

    malloc_trim(0);                                        // ask glibc to give pages back
    report("after malloc_trim(0)");

    std::puts("\nmalloc_stats():");
    malloc_stats();                                        // "in use bytes" vs "system bytes"

    for (char *p : blocks) std::free(p);
    return 0;
}
