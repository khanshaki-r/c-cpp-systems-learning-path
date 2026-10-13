// m11_crash_handler.cpp - a production crash handler: on SIGSEGV/SIGABRT/SIGBUS/SIGFPE write
// the build id, the current request id and a backtrace to stderr using only async-signal-safe
// calls, then re-raise the signal so the kernel still writes the core dump.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fno-omit-frame-pointer -rdynamic
//             -o build/m11_crash_handler m11_crash_handler.cpp
//         (-rdynamic exports symbols so backtrace_symbols_fd can print function names)
// Run:    ./build/m11_crash_handler
//         *** FATAL signal 11 (SIGSEGV) fault addr 0x0000000000000010
//         build  = 2026.10.03-rel-1
//         thread = 12345  request = REQ-00042
//         backtrace:
//         ./build/m11_crash_handler(+0x...) ...
//         Segmentation fault (core dumped)      <- the core is still produced
//
// Rules this file obeys (module 11 explains why):
//   - No printf, malloc, iostream, std::string, locks in the handler. Only write(2) and
//     integer-to-text done by hand. printf takes a lock; if the crash happened inside printf
//     the handler deadlocks. malloc may be the thing that is corrupted.
//   - backtrace() can call malloc on first use (it dlopen's libgcc_s). We call it once at
//     startup to warm it up, which is the standard workaround.
//   - Use sigaltstack: if the crash is a stack overflow the handler needs its own stack.
//   - Restore SIG_DFL and raise() again so the default action (core dump) happens.
//
// Warnings: none (builds clean with -Werror).
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <initializer_list>
#include <unistd.h>
#include <sys/syscall.h>

static const char kBuildId[] = "2026.10.03-rel-1";    // in real code: generated at build time
static thread_local char t_request_id[32] = "none";   // set by the request dispatcher

static void write_str(const char *s) { (void)!write(2, s, std::strlen(s)); }

static void write_hex(unsigned long v)   // async-signal-safe hex formatting
{
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i)
        buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
    buf[18] = '\0';
    write_str(buf);
}

static void write_dec(long v)
{
    char buf[24]; int i = 23; buf[i] = '\0';
    bool neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { buf[--i] = char('0' + u % 10); u /= 10; } while (u);
    if (neg) buf[--i] = '-';
    write_str(buf + i);
}

static void crash_handler(int sig, siginfo_t *info, void *)
{
    write_str("\n*** FATAL signal "); write_dec(sig);
    write_str(sig == SIGSEGV ? " (SIGSEGV)" : sig == SIGABRT ? " (SIGABRT)" :
              sig == SIGBUS  ? " (SIGBUS)"  : sig == SIGFPE  ? " (SIGFPE)"  : "");
    if (sig == SIGSEGV || sig == SIGBUS) { write_str(" fault addr "); write_hex((unsigned long)info->si_addr); }
    write_str("\nbuild  = "); write_str(kBuildId);
    write_str("\nthread = "); write_dec((long)syscall(SYS_gettid));
    write_str("  request = "); write_str(t_request_id);
    write_str("\nbacktrace:\n");

    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);          // writes directly to fd 2, no malloc

    // Re-raise with the default action so the kernel writes the core dump.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler()
{
    // Alternate stack so a stack overflow can still run the handler.
    static char altstack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    // Warm up backtrace() so its one-time dlopen/malloc happens now, not inside the handler.
    void *warm[4]; backtrace(warm, 4);

    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) sigaction(s, &sa, nullptr);
}

struct Account { char id[16]; long balance; };

__attribute__((noinline)) static void handle(Account *a)
{
    a->balance -= 1;                              // a == nullptr -> SIGSEGV at address 0x10
}

int main()
{
    install_crash_handler();
    std::strcpy(t_request_id, "REQ-00042");       // the dispatcher would set this per request
    handle(nullptr);
    return 0;
}
