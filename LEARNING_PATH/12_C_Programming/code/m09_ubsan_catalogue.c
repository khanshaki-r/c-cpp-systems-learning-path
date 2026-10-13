/* m09_ubsan_catalogue.c - trigger one undefined behaviour at a time and let UBSan name it,
 * next to the safe version of each operation.
 *
 * DELIBERATE UB, but ONLY when you pass an argument. With no argument the program runs the
 * safe helpers and their self-checks and has no UB.
 *
 * Build (plain):  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g -o m09_ubsan_catalogue m09_ubsan_catalogue.c
 * Build (UBSan):  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g -fsanitize=undefined -o m09_ubsan_catalogue_ub m09_ubsan_catalogue.c
 * Run:
 *   ./m09_ubsan_catalogue                 safe self-checks only
 *   ./m09_ubsan_catalogue_ub all          each non-crashing UB in turn; one report per line
 *   ./m09_ubsan_catalogue_ub null         null dereference: report, then SIGSEGV
 *   ./m09_ubsan_catalogue_ub div0         integer division by zero: report, then SIGFPE
 * Make the first report fatal (what you want in CI):
 *   add -fno-sanitize-recover=all to the UBSan build line.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* volatile inputs: the compiler cannot see the values, so it cannot fold or warn at
 * compile time. The UB happens at run time, where UBSan can see it. */
static volatile int v_int_max = INT_MAX;
static volatile int v_int_min = INT_MIN;
static volatile int v_zero = 0;
static volatile int v_minus1 = -1;
static volatile unsigned v_32 = 32;
static volatile int v_neg_shift = -1;
static volatile int v_index = 4;

/* ---------------- the UB, one function per class ---------------- */
static int ub_add_overflow(void)    { return v_int_max + 1; }
static int ub_mul_overflow(void)    { return v_int_max * 2; }
static int ub_negate_int_min(void)  { return -v_int_min; }
static unsigned ub_shift_too_wide(void) { return 1u << v_32; }
static int ub_shift_negative(void)  { return 1 << v_neg_shift; }
static int ub_shift_neg_value(void) { return v_minus1 << 1; }   /* left shift of a negative value */
static int ub_out_of_bounds(void)
{
    int arr[4] = { 1, 2, 3, 4 };
    return arr[v_index];                                        /* index 4 of int[4] */
}
static int ub_misaligned(void)
{
    _Alignas(int) unsigned char buf[8] = { 0 };
    int *p = (int *)(void *)(buf + 1);                          /* not 4-byte aligned */
    return *p;
}
static int ub_null_deref(void)
{
    int *volatile p = NULL;
    return *p;
}
static int ub_div_zero(void)        { return 10 / v_zero; }
static int ub_int_min_div(void)     { return v_int_min / v_minus1; }

/* ---------------- the safe versions ---------------- */
static int checked_add(int a, int b, int *out)
{
    return __builtin_add_overflow(a, b, out) ? ERANGE : 0;
}

static unsigned safe_shl(unsigned x, unsigned n)
{
    return n >= sizeof x * CHAR_BIT ? 0u : x << n;              /* define the edge yourself */
}

static int safe_div(int a, int b, int *out)
{
    if (b == 0) return EDOM;
    if (a == INT_MIN && b == -1) return ERANGE;                  /* the one overflowing division */
    *out = a / b;
    return 0;
}

static int safe_get(const int *arr, size_t n, size_t i, int *out)
{
    if (i >= n) return ERANGE;
    *out = arr[i];
    return 0;
}

static int load_int_unaligned(const unsigned char *p)
{
    int v;
    memcpy(&v, p, sizeof v);                                     /* memcpy has no alignment rule */
    return v;
}

struct ub_case {
    const char *name;
    int (*run)(void);
    int crashes;   /* 1: the hardware traps after UBSan reports, so it is not in "all" */
};

static int run_shift_too_wide(void) { return (int)ub_shift_too_wide(); }

static const struct ub_case CASES[] = {
    { "add",        ub_add_overflow,     0 },
    { "mul",        ub_mul_overflow,     0 },
    { "negate",     ub_negate_int_min,   0 },
    { "shift_wide", run_shift_too_wide,  0 },
    { "shift_neg",  ub_shift_negative,   0 },
    { "shift_lneg", ub_shift_neg_value,  0 },
    { "bounds",     ub_out_of_bounds,    0 },
    { "misaligned", ub_misaligned,       0 },
    { "null",       ub_null_deref,       1 },
    { "div0",       ub_div_zero,         1 },
    { "intmin_div", ub_int_min_div,      1 },
};
#define NCASES (sizeof CASES / sizeof CASES[0])

static void self_checks(void)
{
    int out = 0;
    assert(checked_add(INT_MAX, 1, &out) == ERANGE);
    assert(checked_add(2, 3, &out) == 0 && out == 5);

    assert(safe_shl(1u, 31) == 0x80000000u);
    assert(safe_shl(1u, 32) == 0u);
    assert(safe_shl(1u, 1000) == 0u);

    assert(safe_div(10, 0, &out) == EDOM);
    assert(safe_div(INT_MIN, -1, &out) == ERANGE);
    assert(safe_div(-7, 2, &out) == 0 && out == -3);            /* C99+: truncates toward zero */
    assert(-7 % 2 == -1);                                        /* sign of % follows the dividend */

    int arr[4] = { 1, 2, 3, 4 };
    assert(safe_get(arr, 4, 3, &out) == 0 && out == 4);
    assert(safe_get(arr, 4, 4, &out) == ERANGE);

    unsigned char bytes[8] = { 0 };
    int forty_two = 42;
    memcpy(bytes + 1, &forty_two, sizeof forty_two);
    assert(load_int_unaligned(bytes + 1) == 42);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        self_checks();
        puts("safe self-checks passed; pass 'all' or a case name to trigger UB:");
        for (size_t i = 0; i < NCASES; i++)
            printf("  %s%s\n", CASES[i].name, CASES[i].crashes ? "  (crashes after the report)" : "");
        return 0;
    }

    int all = strcmp(argv[1], "all") == 0;
    int ran = 0;
    for (size_t i = 0; i < NCASES; i++) {
        if ((all && !CASES[i].crashes) || strcmp(argv[1], CASES[i].name) == 0) {
            printf("--- %s\n", CASES[i].name);
            fflush(stdout);                     /* keep our line before UBSan's stderr line */
            int r = CASES[i].run();
            printf("    returned %d\n", r);     /* whatever the hardware happened to do */
            fflush(stdout);
            ran++;
        }
    }
    if (!ran) {
        fprintf(stderr, "unknown case '%s'\n", argv[1]);
        return 2;
    }
    return 0;
}
