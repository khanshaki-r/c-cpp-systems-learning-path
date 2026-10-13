// m04_integer_overflow_size.cpp - a size computed from untrusted input overflows, the
// allocation is far too small, and the following copy writes past it.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_int_overflow m04_integer_overflow_size.cpp
// Run:    ./build/m04_int_overflow 10          -> fine
//         ./build/m04_int_overflow 1073741824  -> count * 4 overflows a 32-bit int
//
// UBSan build (-fsanitize=undefined):
//         m04_integer_overflow_size.cpp:NN:NN: runtime error: signed integer overflow:
//         1073741824 * 4 cannot be represented in type 'int'
//
// ASan build: heap-buffer-overflow at the memset (if the program gets that far).
//
// Warnings with -Wall -Wextra: none (values are runtime). -Wconversion would warn about the
// int -> size_t narrowing below, which is a hint that the arithmetic type is wrong.
#include <cstdio>
#include <cstdlib>
#include <cstring>

static char *allocate_table(int count)
{
    int bytes = count * 4;                     // BUG: int arithmetic; 2^30 * 4 overflows to 0
    std::printf("count=%d bytes=%d\n", count, bytes);
    if (bytes < 0) return nullptr;             // a naive guard that the overflow slips through
    char *p = static_cast<char *>(std::malloc(static_cast<size_t>(bytes)));
    if (!p) return nullptr;
    std::memset(p, 0, static_cast<size_t>(count) * 4);   // writes the INTENDED size -> overflow
    return p;
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? std::atoi(argv[1]) : 10;
    char *t = allocate_table(count);
    std::printf("table %p\n", static_cast<void *>(t));
    std::free(t);
    return 0;
}
