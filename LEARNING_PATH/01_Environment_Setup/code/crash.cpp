// crash.cpp - produces a core dump on purpose.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 crash.cpp -o crash
// Run  : ulimit -c unlimited && ./crash     -> "Segmentation fault (core dumped)"
// Read : gdb -q ./crash core.*              -> (gdb) bt
#include <cstdio>

struct Account {
    long balance;
};

void write_through_null(Account* acct) {
    acct->balance = 100;  // acct is nullptr: SIGSEGV writing to address 0
}

int main() {
    Account* acct = nullptr;
    std::puts("about to crash");
    write_through_null(acct);
    std::puts("never printed");
    return 0;
}
