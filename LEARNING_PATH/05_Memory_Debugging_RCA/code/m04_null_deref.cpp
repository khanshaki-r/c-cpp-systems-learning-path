// m04_null_deref.cpp - the most common crash in the world: dereferencing a null pointer
// that came from a lookup that failed.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_null_deref m04_null_deref.cpp
// Run:    ./build/m04_null_deref            -> works (account exists)
//         ./build/m04_null_deref ACC-9999   -> Segmentation fault (core dumped)
//
// gdb:    gdb --args ./build/m04_null_deref ACC-9999
//         (gdb) run
//         Program received signal SIGSEGV, Segmentation fault.
//         apply_fee (acct=0x0, fee=25) at m04_null_deref.cpp:NN
//         (gdb) p $_siginfo._sifields._sigfault.si_addr   -> (void *) 0x10
//         The faulting address is 0x10 = offsetof(Account, balance). A small faulting address
//         (< 4096) almost always means "null pointer plus a field offset".
//         (gdb) bt   -> apply_fee <- process_request <- main. The bug is in process_request:
//         it never checked the result of find_account.
//
// ASan:   reports "SEGV on unknown address 0x000000000010 ... The signal is caused by a READ
//         memory access. Hint: address points to the zero page."
// Valgrind: "Invalid read of size 8 ... Address 0x10 is not stack'd, malloc'd or (recently) free'd"
//
// Warnings with -Wall -Wextra: none. GCC's -Wnull-dereference (not in -Wall) cannot see this
// either because the null comes from a runtime lookup.
#include <cstdio>
#include <cstring>
#include <vector>

struct Account {
    char id[16];      // offset 0
    long balance;     // offset 16 = 0x10  <- this is the faulting address you will see
};

static std::vector<Account> g_ledger = {
    {"ACC-0001", 1000}, {"ACC-0002", 2500}, {"ACC-0003", 75},
};

// Returns nullptr when not found. The caller must check. It does not.
static Account *find_account(const char *id)
{
    for (auto &a : g_ledger)
        if (std::strcmp(a.id, id) == 0) return &a;
    return nullptr;
}

__attribute__((noinline)) static void apply_fee(Account *acct, long fee)
{
    acct->balance -= fee;          // SIGSEGV here when acct == nullptr: reads [acct + 0x10]
    std::printf("%s -> %ld\n", acct->id, acct->balance);
}

static void process_request(const char *id)
{
    Account *acct = find_account(id);   // BUG: result not checked
    apply_fee(acct, 25);
}

int main(int argc, char **argv)
{
    process_request(argc > 1 ? argv[1] : "ACC-0002");
    return 0;
}
