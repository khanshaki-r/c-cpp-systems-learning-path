// m04_use_after_free.cpp - a session object is deleted by one code path while another path
// still holds a raw pointer to it. The stale pointer is then used.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_use_after_free m04_use_after_free.cpp
// Run:    ./build/m04_use_after_free
//         Usually prints garbage for the user name or a wrong balance and does NOT crash.
//         Freed memory is still mapped, so the hardware does not fault. This is why UAF is the
//         most dangerous class: it silently corrupts data.
//
// ASan:   ERROR: AddressSanitizer: heap-use-after-free on address ...
//         READ of size 8 at ... thread T0
//             #0 audit_session m04_use_after_free.cpp:NN
//         0x... is located 8 bytes inside of 48-byte region ...
//         freed by thread T0 here:   #0 operator delete  #1 close_session ...
//         previously allocated by thread T0 here:  #0 operator new  #1 open_session ...
//         ASan gives you all three stacks: use, free and allocation. That is the fastest path
//         to the owner-confusion bug behind every UAF.
//
// Valgrind: "Invalid read of size 8 ... Address ... is 8 bytes inside a block of size 48 free'd"
//           followed by the free and alloc stacks.
//
// Warnings with -Wall -Wextra: none. GCC's -Wuse-after-free (in -Wall since GCC 12) catches
// `delete p; p->x;` inside ONE function, but not when free and use are in different functions,
// which is how real bugs look.
#include <cstdio>
#include <string>
#include <unordered_map>

struct Session {
    long        id;
    std::string user;
    long        balance;
};

static std::unordered_map<long, Session *> g_sessions;   // owns the Session objects

static Session *open_session(long id, const char *user)
{
    Session *s = new Session{id, user, 100};
    g_sessions[id] = s;
    return s;
}

static void close_session(long id)
{
    auto it = g_sessions.find(id);
    if (it == g_sessions.end()) return;
    delete it->second;                 // the object dies here...
    g_sessions.erase(it);
}

__attribute__((noinline)) static void audit_session(const Session *s)
{
    // ...but the caller still has the pointer. Reads freed memory.
    std::printf("audit: session %ld user=%s balance=%ld\n", s->id, s->user.c_str(), s->balance);
}

int main()
{
    Session *current = open_session(42, "shakir");   // a "current session" cached in a raw pointer
    open_session(43, "someone-else");

    close_session(42);                               // a timeout handler closes it...
    audit_session(current);                          // ...and later code uses the cached pointer

    close_session(43);
    return 0;
}
