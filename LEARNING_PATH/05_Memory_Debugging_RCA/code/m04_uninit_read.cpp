// m04_uninit_read.cpp - a heap record is used before every field is set. The program's
// behaviour depends on whatever was in that memory before.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_uninit_read m04_uninit_read.cpp
// Run:    ./build/m04_uninit_read
//         Prints a different "flags" value run to run, or the same value by luck. Programs with
//         this bug typically work on the developer machine and fail on the client's.
//
// Valgrind (the tool for this; ASan does NOT detect uninitialised reads):
//         Conditional jump or move depends on uninitialised value(s)
//            at 0x...: is_privileged(Record const*) (m04_uninit_read.cpp:NN)
//            by 0x...: main (m04_uninit_read.cpp:NN)
//          Uninitialised value was created by a heap allocation
//            at 0x...: operator new(unsigned long)
//            by 0x...: make_record(long) (m04_uninit_read.cpp:NN)
//         (the last block needs --track-origins=yes)
//
// MemorySanitizer (clang only, not GCC) also catches it. ASan and UBSan do not.
//
// Warnings with -Wall -Wextra: none, because the object is on the heap. The same bug with a
// local variable (`int flags; if (flags & 1)`) does trigger -Wuninitialized / -Wmaybe-uninitialized,
// especially with -O2. That is a reason to compile with -O2 -Wall in CI even for debug code.
#include <cstdio>

struct Record {
    long id;
    long balance;
    int  flags;      // never initialised by make_record
};

static Record *make_record(long id)
{
    Record *r = new Record;   // no braces, no constructor -> members are indeterminate
    r->id = id;
    r->balance = 0;
    return r;                 // flags left as garbage
}

__attribute__((noinline)) static bool is_privileged(const Record *r)
{
    return (r->flags & 0x1) != 0;         // Valgrind: conditional jump depends on uninitialised value
}

int main()
{
    Record *r = make_record(7);
    std::printf("record %ld privileged=%d flags=0x%x\n", r->id, is_privileged(r) ? 1 : 0, r->flags);
    delete r;
    return 0;
}
