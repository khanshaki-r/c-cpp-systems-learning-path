/* m16_modern_c_features.c - C99 and C11 features you are expected to know, each with a check,
 * plus the "static in three contexts", const-pointer and volatile questions as running code.
 * (C23 features are in the module text; they need -std=c2x, and this file is -std=c17.)
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m16_modern_c_features m16_modern_c_features.c
 * Run:    ./m16_modern_c_features
 */
#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---- C11: _Static_assert (static_assert in <assert.h>): checked at compile time ---- */
_Static_assert(sizeof(int32_t) == 4, "int32_t is exactly 32 bits by definition");
/* A C, C++ difference you can check: a character literal has type int in C, char in C++. */
_Static_assert(sizeof('a') == sizeof(int), "in C, 'a' is an int");

/* ---- C99: designated initialisers; C11: anonymous struct/union members ---- */
struct msg {
    uint16_t type;
    uint16_t flags;
    union {                     /* anonymous union: members accessed as m.amount, m.text */
        int64_t amount;
        char    text[8];
    };
};

/* ---- C99: flexible array member: header and payload in ONE allocation ---- */
struct packet {
    uint32_t len;
    unsigned char data[];       /* must be last; sizeof(struct packet) excludes it */
};

static struct packet *packet_new(const void *src, uint32_t len)
{
    struct packet *p = malloc(sizeof *p + len);
    if (p) {
        p->len = len;
        memcpy(p->data, src, len);
    }
    return p;
}

/* ---- C11: _Generic, compile-time selection on the type of an expression ---- */
#define TYPE_NAME(x) _Generic((x),                                              \
    int: "int", unsigned: "unsigned", long: "long", double: "double",           \
    char *: "char *", const char *: "const char *", int *: "int *",              \
    const int *: "const int *", default: "other")

/* ---- static, context 1: a local that keeps its value between calls (static storage
 * duration, initialised once, before main). Not thread-safe without a lock or an atomic. */
static int next_id(void)
{
    static int counter = 1000;
    return ++counter;
}

/* ---- static, context 2: at file scope, internal linkage. next_id above is invisible to
 * other .c files, so two files can each have their own next_id without a link error. */

/* ---- static, context 3 (C99): in an array parameter, a promise that the caller passes at
 * least 4 elements. The compiler may use it and can warn when a call breaks it. */
static int sum4(const int a[static 4])
{
    return a[0] + a[1] + a[2] + a[3];
}

/* ---- C99: variable length arrays. Optional since C11 (__STDC_NO_VLA__); the size lives on
 * the stack, so an attacker-controlled n is a stack overflow. Most codebases ban them. ---- */
static size_t vla_bytes(size_t n)
{
    int scratch[n];             /* allocated on the stack at run time */
    for (size_t i = 0; i < n; i++)
        scratch[i] = (int)i;
    assert(scratch[n - 1] == (int)n - 1);
    return sizeof scratch;      /* for a VLA, sizeof is evaluated at run time */
}

/* ---- volatile: the one correct use in hosted C - a flag written by a signal handler.
 * volatile sig_atomic_t is the only object type the standard lets a handler write. ---- */
static volatile sig_atomic_t g_got_signal;

static void on_signal(int sig)
{
    g_got_signal = sig;
}

/* ---- C11: _Noreturn ---- */
static _Noreturn void die(const char *why)
{
    fprintf(stderr, "fatal: %s\n", why);
    exit(1);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 99)
        die("too many arguments");   /* never taken; shows _Noreturn in use */

    /* designated initialisers: order-free, unnamed members zeroed */
    struct msg m = { .flags = 0x2, .amount = 250000 };
    assert(m.type == 0 && m.flags == 2 && m.amount == 250000);
    int table[10] = { [0] = 1, [9] = 9 };       /* array designators: rest are 0 */
    assert(table[5] == 0 && table[9] == 9);

    /* compound literal: an unnamed object, here passed by address without a variable */
    assert(sum4((int[]){ 1, 2, 3, 4 }) == 10);

    /* flexible array member */
    struct packet *pk = packet_new("ACC-1001", 8);
    assert(pk && pk->len == 8 && memcmp(pk->data, "ACC-1001", 8) == 0);
    assert(sizeof(struct packet) == 4);
    free(pk);

    /* _Generic */
    const char *cs = "x";
    int i = 0;
    const int *pci = &i;            /* pointer to const int: *pci = 1 would not compile */
    int *const cpi = &i;            /* const pointer to int: cpi = NULL would not compile */
    *cpi = 5;                       /* ... but the int it points to can change */
    assert(i == 5 && *pci == 5);
    assert(strcmp(TYPE_NAME(42), "int") == 0);
    assert(strcmp(TYPE_NAME(42u), "unsigned") == 0);
    assert(strcmp(TYPE_NAME(4.2), "double") == 0);
    assert(strcmp(TYPE_NAME(cs), "const char *") == 0);
    assert(strcmp(TYPE_NAME(pci), "const int *") == 0);
    /* The top-level const of cpi is dropped when it is used as a value, so it selects int *. */
    assert(strcmp(TYPE_NAME(cpi), "int *") == 0);
    /* Integer promotion: char + char is int. */
    char c1 = 'a', c2 = 'b';
    assert(strcmp(TYPE_NAME(c1 + c2), "int") == 0);

    /* bool: any non-zero value converts to exactly 1 */
    bool b = 5;
    assert(b == 1 && sizeof(bool) == 1);

    /* fixed-width integers and their printf macros */
    int64_t big = INT64_C(9000000000);
    char out[32];
    snprintf(out, sizeof out, "%" PRId64, big);
    assert(strcmp(out, "9000000000") == 0);

    /* alignment */
    static alignas(64) unsigned char cache_line[64];
    assert((uintptr_t)cache_line % 64 == 0);
    assert(alignof(double) == 8 && alignof(max_align_t) >= 8);

    /* static local */
    assert(next_id() == 1001 && next_id() == 1002);

    /* VLA */
    assert(vla_bytes(10) == 10 * sizeof(int));

    /* integer division truncates toward zero since C99 */
    assert(-7 / 2 == -3 && -7 % 2 == -1 && 7 % -2 == 1);

    /* volatile flag set from a signal handler */
    signal(SIGINT, on_signal);
    raise(SIGINT);                  /* handler runs synchronously before raise returns */
    assert(g_got_signal == SIGINT);

    printf("__STDC_VERSION__ = %ldL, TYPE_NAME(cpi) = %s, __func__ = %s\n",
           (long)__STDC_VERSION__, TYPE_NAME(cpi), __func__);
    puts("all checks passed");
    return 0;
}
