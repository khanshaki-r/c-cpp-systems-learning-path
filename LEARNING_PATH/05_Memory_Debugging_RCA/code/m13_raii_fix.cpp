// m13_raii_fix.cpp - the bugs from m04_use_after_free, m04_heap_overflow, m09_leak and
// m06_crash_example, written the way module 13 says they should be written.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -pthread
//             -g -O2 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -D_FORTIFY_SOURCE=2
//             -o build/m13_raii_fix m13_raii_fix.cpp
// Run:    ./build/m13_raii_fix
//
// Every ASan/Valgrind/TSan run of this file should be clean. That is the point.
//
// Warnings: none (builds clean with -Werror and the extra warnings above).
#include <cassert>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ---- 1. Ownership is explicit: the map owns sessions; others hold ids, not pointers. --------
struct Session {
    long        id;
    std::string user;
    long        balance = 100;
};

class SessionTable {
public:
    long open(const char *user)
    {
        long id = next_id_++;
        sessions_.emplace(id, std::make_unique<Session>(Session{id, user}));
        return id;
    }
    void close(long id) { sessions_.erase(id); }         // unique_ptr deletes; nothing dangles

    // Callers get a non-owning view that is valid only for this call. If they need to keep
    // something, they keep the id and look it up again.
    const Session *find(long id) const
    {
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second.get();
    }
private:
    std::unordered_map<long, std::unique_ptr<Session>> sessions_;
    long next_id_ = 1;
};

static void audit(const SessionTable &t, long id)
{
    if (const Session *s = t.find(id))                   // every lookup result is checked
        std::printf("audit: %ld %s %ld\n", s->id, s->user.c_str(), s->balance);
    else
        std::printf("audit: session %ld no longer exists\n", id);
}

// ---- 2. Containers know their size; bounds are checked in debug, loops are idiomatic. -------
static std::vector<int> fill_totals(std::size_t n)
{
    std::vector<int> totals(n);
    for (std::size_t i = 0; i < totals.size(); ++i)      // size() is the bound, nothing else
        totals.at(i) = static_cast<int>(i) * 100;        // .at(): throws instead of corrupting
    return totals;                                       // RVO; no new/delete anywhere
}

// ---- 3. Early returns cannot leak: the resource dies with the scope. ------------------------
struct Request { int id; std::string body; };

static bool handle_request(int id)
{
    auto r = std::make_unique<Request>(Request{id, std::string(64, 'x')});
    if (r->id % 4 == 0) return false;                    // unique_ptr frees r here
    return true;
}

// ---- 4. Sizes are size_t and checked before use. -------------------------------------------
static std::vector<char> allocate_table(std::size_t count)
{
    constexpr std::size_t kMax = 1u << 26;               // policy limit: 64 Mi entries
    if (count > kMax) throw std::length_error("table too large");
    return std::vector<char>(count * 4);                  // size_t arithmetic; cannot wrap below kMax
}

// ---- 5. Invariants are asserted where they matter. -----------------------------------------
static long apply_fee(long balance, long fee)
{
    assert(fee >= 0 && "fees are never negative");
    return balance - fee;
}

int main()
{
    SessionTable table;
    long a = table.open("shakir");
    long b = table.open("other");
    table.close(a);
    audit(table, a);                                     // safe: prints "no longer exists"
    audit(table, b);

    auto totals = fill_totals(10);
    long sum = 0; for (int v : totals) sum += v;
    std::printf("sum %ld\n", sum);

    int ok = 0; for (int i = 1; i <= 1000; ++i) ok += handle_request(i) ? 1 : 0;
    std::printf("accepted %d/1000, nothing leaked\n", ok);

    try { allocate_table(1u << 30); } catch (const std::length_error &e) { std::printf("rejected: %s\n", e.what()); }

    std::printf("fee applied: %ld\n", apply_fee(1000, 25));
    return 0;
}
