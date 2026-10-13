// m06_crash_example.cpp - the worked example for module 06 (reading a core dump cold).
//
// A tiny "statement batch" processor, modelled on an RPC server handler: parse requests, look up
// accounts, apply them. One request names an account that was closed (removed from the ledger)
// earlier in the same batch; the handler keeps using the pointer it cached before the removal.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O2 -fno-omit-frame-pointer
//             -o build/m06_crash_example m06_crash_example.cpp
//         (-O2 on purpose: production builds are optimised, and module 06 teaches you to read
//          an optimised core. Build an -O0 version too and compare.)
//
// Produce a core:
//         ulimit -c unlimited
//         cat /proc/sys/kernel/core_pattern        # if it starts with '|' see module 06, section 2
//         ./build/m06_crash_example batch.txt       # batch.txt is written by the program if missing
//         -> Segmentation fault (core dumped)
//         gdb ./build/m06_crash_example core.<pid>   # or: coredumpctl debug
//
// What the core should show (walked through in module 06):
//         signal SIGSEGV, si_code SEGV_MAPERR, si_addr = a small or garbage address
//         #0 post_entry        <- faulting instruction dereferences `acct`
//         #1 process_batch
//         #2 main
//         `acct` points into a freed vector element (ledger.erase shifted the elements and the
//         buffer may have been reallocated), or holds a stale copy of a now-shifted record.
//
// Warnings with -Wall -Wextra: none.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Account {
    char id[12];
    long balance;
    long pending;
};

struct Entry {
    char kind;           // 'D' debit, 'C' credit, 'X' close account
    char id[12];
    long amount;
};

static std::vector<Account> g_ledger;

static Account *lookup(const char *id)
{
    for (auto &a : g_ledger)
        if (std::strcmp(a.id, id) == 0) return &a;
    return nullptr;
}

static void close_account(const char *id)
{
    for (auto it = g_ledger.begin(); it != g_ledger.end(); ++it)
        if (std::strcmp(it->id, id) == 0) { g_ledger.erase(it); return; }
    // After erase(), every pointer into g_ledger at or after `it` is invalid.
}

__attribute__((noinline)) static void post_entry(Account *acct, const Entry &e)
{
    if (e.kind == 'D') acct->balance -= e.amount;      // faults here when acct is stale/null
    else               acct->balance += e.amount;
    acct->pending = 0;
}

static std::vector<Entry> parse(const char *path)
{
    std::vector<Entry> out;
    std::ifstream in(path);
    std::string kind, id; long amount;
    while (in >> kind >> id >> amount) {
        Entry e{};
        e.kind = kind[0];
        std::strncpy(e.id, id.c_str(), sizeof e.id - 1);
        e.amount = amount;
        out.push_back(e);
    }
    return out;
}

static void process_batch(const std::vector<Entry> &batch)
{
    // "Optimisation": resolve all accounts once up front, then post. The cache of raw pointers
    // is the bug: the 'X' entry mutates the vector and invalidates the cached pointers.
    std::vector<Account *> resolved;
    resolved.reserve(batch.size());
    for (const Entry &e : batch) resolved.push_back(lookup(e.id));

    for (size_t i = 0; i < batch.size(); ++i) {
        const Entry &e = batch[i];
        if (e.kind == 'X') { close_account(e.id); continue; }
        post_entry(resolved[i], e);                   // resolved[i] may be dangling or nullptr
    }
}

static void write_sample_batch(const char *path)
{
    std::ofstream out(path);
    out << "D ACC-1 100\n"
           "C ACC-2 50\n"
           "X ACC-1 0\n"          // closes ACC-1: ledger shrinks, elements shift
           "D ACC-3 20\n"         // cached pointer for ACC-3 now points one slot too far
           "D ACC-9 5\n";         // ACC-9 does not exist: lookup returned nullptr
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "batch.txt";
    if (!std::ifstream(path)) write_sample_batch(path);

    g_ledger = { {"ACC-1", 1000, 0}, {"ACC-2", 2000, 0}, {"ACC-3", 3000, 0} };
    g_ledger.shrink_to_fit();   // capacity == size: an erase will not reallocate, but shifts elements

    std::vector<Entry> batch = parse(path);
    std::printf("processing %zu entries\n", batch.size());
    process_batch(batch);

    for (const auto &a : g_ledger) std::printf("%s %ld\n", a.id, a.balance);
    return 0;
}
