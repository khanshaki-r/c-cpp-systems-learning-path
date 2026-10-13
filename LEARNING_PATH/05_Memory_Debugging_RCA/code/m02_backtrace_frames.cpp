// m02_backtrace_frames.cpp - look at real stack frames from inside the program and from gdb.
//
// Build (debug):     g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 -fno-omit-frame-pointer
//                        -o build/m02_frames_O0 m02_backtrace_frames.cpp
// Build (optimised): g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O2
//                        -o build/m02_frames_O2 m02_backtrace_frames.cpp
//
// Try in gdb:
//   gdb ./build/m02_frames_O0
//   (gdb) break level3
//   (gdb) run
//   (gdb) bt                      # four frames: level3, level2, level1, main
//   (gdb) info frame              # saved rip (return address), saved rbp, frame address
//   (gdb) x/4xg $rbp              # [rbp] = saved rbp of caller, [rbp+8] = return address
//   (gdb) p $rbp  /  p $rsp
//   (gdb) up ; info locals        # locals of level2
//   (gdb) finish                  # run until level3 returns; prints "Value returned is ..."
//
// Then the same with the -O2 build: level2/level3 are usually inlined; gdb shows them as
// inlined frames (from DWARF DW_TAG_inlined_subroutine) and many locals are <optimized out>.
//
// Warnings: none.
#include <cstdio>
#include <cstdint>
#include <execinfo.h>   // backtrace(), backtrace_symbols_fd() - glibc, Linux only

static void print_native_backtrace()
{
    void *frames[32];
    int n = backtrace(frames, 32);                 // walks the stack using unwind info
    std::puts("--- backtrace() from inside the process ---");
    backtrace_symbols_fd(frames, n, 1);            // 1 = stdout; async-signal-safe-ish, no malloc
}

// Each level has its own locals so you can see them disappear/appear with `up` and `down`.
__attribute__((noinline)) static long level3(long c)
{
    volatile long local3 = c * 3;                  // volatile: forces a real stack slot even at -O2
    std::printf("level3: &local3 = %p\n", (void *)&local3);
    print_native_backtrace();
    return local3 + 1;
}

static long level2(long b)
{
    long local2 = b * 2;
    std::printf("level2: &local2 = %p\n", (void *)&local2);
    return level3(local2) + 1;
}

static long level1(long a)
{
    long local1 = a + 1;
    std::printf("level1: &local1 = %p\n", (void *)&local1);
    return level2(local1) + 1;
}

int main(int argc, char **)
{
    // Stack grows DOWN: &local1 > &local2 > &local3. Each printed address is lower than the last.
    long r = level1(argc);
    std::printf("result %ld\n", r);
    return 0;
}
