// m04_stack_overflow.cpp - unbounded recursion runs off the end of the stack.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_stack_overflow m04_stack_overflow.cpp
// Run:    ./build/m04_stack_overflow           -> Segmentation fault (core dumped)
//         ulimit -s                             -> 8192 (KiB) is the usual main-thread stack limit
//         ./build/m04_stack_overflow 100        -> fine: depth is bounded
//
// gdb:    (gdb) run
//         Program received signal SIGSEGV
//         (gdb) bt -10                 # show only the OUTERMOST 10 frames: main at the bottom
//         (gdb) bt 5                   # the innermost 5: thousands of identical frames above
//         (gdb) info frame             # note how low $rsp is
//         (gdb) p $_siginfo._sifields._sigfault.si_addr
//         (gdb) info proc mappings     # compare si_addr with the [stack] mapping: it is just BELOW it
//         A fault address just below the stack mapping, plus a backtrace with thousands of frames,
//         is the signature of stack overflow. On Windows the exception is 0xC00000FD.
//
// ASan:   "ERROR: AddressSanitizer: stack-overflow on address 0x7ff... (pc ... bp ... sp ...)"
//
// Warnings with -Wall -Wextra: none. GCC's -Winfinite-recursion (in -Wall since GCC 12) only
// fires when every path recurses; here the base case exists but is unreachable for the default
// argument, so the compiler stays quiet. Real bugs look exactly like this.
#include <cstdio>
#include <cstdlib>
#include <cstring>

// A realistic shape: recursive parser/tree walk with a large local buffer per frame.
__attribute__((noinline)) static long walk(long depth, long limit)
{
    char scratch[4096];                       // 4 KiB per frame -> ~2000 frames fill 8 MiB
    std::memset(scratch, (int)(depth & 0xff), sizeof scratch);
    if (depth >= limit)                       // base case: never reached when limit is huge
        return scratch[0];
    long r = walk(depth + 1, limit);          // not a tail call: result is used below
    return r + scratch[depth & 0xfff];        // keeps scratch alive across the call
}

int main(int argc, char **argv)
{
    long limit = argc > 1 ? std::atol(argv[1]) : 1L << 40;   // default: effectively infinite
    std::printf("recursing to depth %ld...\n", limit);
    std::printf("result %ld\n", walk(0, limit));
    return 0;
}
