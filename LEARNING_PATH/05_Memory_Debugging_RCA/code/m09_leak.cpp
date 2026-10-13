// m09_leak.cpp - a long-running server loop that leaks on one path only (the error path).
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m09_leak m09_leak.cpp
// Run:    ./build/m09_leak 1000
//
// LeakSanitizer (part of ASan on Linux, see build_debug.sh):
//         ERROR: LeakSanitizer: detected memory leaks
//         Direct leak of 32000 byte(s) in 250 object(s) allocated from:
//             #0 operator new(unsigned long)
//             #1 handle_request(int) m09_leak.cpp:NN
//         Indirect leak of ... byte(s) in 250 object(s) allocated from:   <- the std::string buffers
//         SUMMARY: AddressSanitizer: NNN byte(s) leaked in NNN allocation(s).
//         "Direct" = nothing points at it. "Indirect" = only reachable from a direct leak.
//
// Valgrind:  valgrind --leak-check=full --show-leak-kinds=all ./build/m09_leak 1000
//         definitely lost: 32,000 bytes in 250 blocks      <- the Request objects
//         indirectly lost: ...                             <- their string buffers
//         Also try --xtree-leak=yes for a callgrind-format tree of leak sites.
//
// heaptrack:  heaptrack ./build/m09_leak 100000 ; heaptrack_print heaptrack.m09_leak.*.zst | less
//         Look at "MOST CALLS TO ALLOCATION FUNCTIONS" and "MEMORY LEAKED" sections.
//
// Warnings with -Wall -Wextra: none.
#include <cstdio>
#include <cstdlib>
#include <string>

struct Request {
    int         id;
    std::string body;       // heap-allocated buffer when longer than ~15 chars (SSO limit)
    char        scratch[16];
};

static bool validate(const Request *r)
{
    return r->id % 4 != 0;  // every 4th request fails validation
}

static void handle_request(int id)
{
    Request *r = new Request{id, std::string(64, 'x'), {}};
    if (!validate(r)) {
        std::fprintf(stderr, "request %d rejected\n", id);
        return;             // BUG: early return without delete -> leak on the error path only
    }
    // ... do work ...
    delete r;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    for (int i = 1; i <= n; ++i) handle_request(i);
    std::printf("handled %d requests\n", n);
    return 0;
}
