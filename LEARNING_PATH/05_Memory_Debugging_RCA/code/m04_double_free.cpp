// m04_double_free.cpp - two "owners" of the same raw pointer both release it.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_double_free m04_double_free.cpp
// Run:    ./build/m04_double_free
//         glibc 2.39 prints:  free(): double free detected in tcache 2
//                             Aborted (core dumped)
//         The signal is SIGABRT, not SIGSEGV. The backtrace goes
//         raise <- abort <- __libc_message <- malloc_printerr <- _int_free <- free <- release_buffer
//         <- ... your code. The frame you care about is the first one in your own code.
//
// gdb:    (gdb) run
//         (gdb) bt                 # find the first frame in your code, then
//         (gdb) frame N ; info locals
//         Note: glibc detects THIS double free because the chunk is still in the thread cache.
//         Not every double free is detected; if the block was reallocated in between, the second
//         free destroys a live object and you get a use-after-free elsewhere instead.
//
// ASan:   ERROR: AddressSanitizer: attempting double-free on 0x... in thread T0
//         with the stacks of both frees and of the allocation.
// Valgrind: "Invalid free() / delete / delete[] / realloc() ... Address ... is 0 bytes inside a
//           block of size 256 free'd" plus the first free's stack.
//
// Warnings with -Wall -Wextra: none (the two frees are in different functions).
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Request {
    char *payload;
    int   len;
};

static void release_buffer(char *p) { std::free(p); }

static void handle(Request *r)
{
    std::printf("handling %d bytes\n", r->len);
    release_buffer(r->payload);        // handler thinks it owns the payload
}

int main()
{
    Request r;
    r.len = 256;
    r.payload = static_cast<char *>(std::malloc(r.len));
    std::memset(r.payload, 'x', r.len);

    handle(&r);
    release_buffer(r.payload);         // caller also thinks it owns the payload -> double free
    std::puts("done");
    return 0;
}
