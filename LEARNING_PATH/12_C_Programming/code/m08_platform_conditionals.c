/* m08_platform_conditionals.c - one source for Windows and POSIX: conditional compilation done tidily.
 *
 * Module: 12_C_Programming/08_the_preprocessor_and_macros.md
 * Build (Linux): gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m08_platform_conditionals.c -o m08_platform_conditionals
 * Build (MSVC):  cl /std:c17 /W4 /WX m08_platform_conditionals.c
 *
 * Rule: keep #ifdef in ONE small portability layer (this top block). The rest of the program
 * calls plat_* functions and never mentions a platform.
 */

/* Feature-test macros must come before the FIRST #include. _WIN32 needs no header: the
 * compiler predefines it (also on 64-bit Windows; _WIN64 is defined in addition). */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define PLAT_NAME "Windows"
#  define PLAT_PATH_SEP '\\'
static void plat_sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned long plat_pid(void) { return (unsigned long)GetCurrentProcessId(); }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  include <time.h>
#  include <unistd.h>
#  define PLAT_NAME "POSIX"
#  define PLAT_PATH_SEP '/'
static void plat_sleep_ms(unsigned ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { /* signal: sleep the remainder */ }
}
static unsigned long plat_pid(void) { return (unsigned long)getpid(); }
#else
#  error "unsupported platform: add a branch to the portability layer"
#endif

/* Compiler differences, same idea. C11 already gives _Noreturn and _Alignas portably. */
#if defined(_MSC_VER)
#  define PRINTF_LIKE(fmt_idx, arg_idx)
#  define LIKELY(x) (x)
#elif defined(__GNUC__) || defined(__clang__)
#  define PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#  define LIKELY(x) __builtin_expect(!!(x), 1)
#else
#  define PRINTF_LIKE(fmt_idx, arg_idx)
#  define LIKELY(x) (x)
#endif

/* Data model check at preprocessing time: long is 32-bit on Windows, 64-bit on Linux. */
#if LONG_MAX == 2147483647L
#  define DATA_MODEL "long is 32-bit (LLP64 or ILP32)"
#else
#  define DATA_MODEL "long is 64-bit (LP64)"
#endif

/* __has_include is standard in C23 and an extension in GCC 5+ and Clang: test for it first. */
#if defined(__has_include)
#  if __has_include(<threads.h>)
#    define HAVE_C11_THREADS 1
#  endif
#endif
#ifndef HAVE_C11_THREADS
#  define HAVE_C11_THREADS 0
#endif

PRINTF_LIKE(1, 2) static void say(const char *fmt, ...);

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[plat] ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

int main(void)
{
    say("platform: %s, path separator '%c'", PLAT_NAME, PLAT_PATH_SEP);
    say("%s", DATA_MODEL);
    say("<threads.h> available: %s", HAVE_C11_THREADS ? "yes" : "no");
    unsigned long pid = plat_pid();
    say("pid is positive: %s", LIKELY(pid > 0) ? "yes" : "no");
    plat_sleep_ms(20);
    say("slept 20 ms through the portability layer");
    assert(pid > 0);
    return 0;
}
