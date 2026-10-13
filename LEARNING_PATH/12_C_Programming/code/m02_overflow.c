/* m02_overflow.c - signed overflow is undefined, unsigned overflow wraps, and how to check safely.
 *
 * Module: 12_C_Programming/02_types_integers_and_conversions.md
 *
 * WARNING - DELIBERATE UNDEFINED BEHAVIOUR: naive_increment_overflows(INT_MAX) evaluates
 * INT_MAX + 1, which is undefined. It is here to show that the compiler assumes signed overflow
 * never happens and deletes the check. The program asserts nothing about that result.
 * It still compiles cleanly with -Werror (no warning is issued, which is the scary part).
 *
 * Build:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m02_overflow.c -o m02_overflow
 * Compare: add -fwrapv (signed overflow defined to wrap): the first line changes from 0 to 1.
 * Catch:   gcc -std=c17 -g -fsanitize=undefined m02_overflow.c -o m02_overflow_ubsan && ./m02_overflow_ubsan
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* UB when x == INT_MAX. GCC folds "x + 1 < x" to 0 (false) even at -O0. */
static int naive_increment_overflows(int x) { return x + 1 < x; }

/* Correct: decide BEFORE doing the arithmetic, using only operations that cannot overflow. */
static bool add_i64_checked(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *out = a + b;
    return true;
}

static bool mul_size_checked(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}

int main(void)
{
    printf("naive_increment_overflows(INT_MAX) = %d   <- UB: 0 means the check was deleted\n",
           naive_increment_overflows(INT_MAX));

    unsigned int u = UINT_MAX;
    u = u + 1;                                      /* defined: unsigned wraps modulo 2^32 */
    printf("UINT_MAX + 1u = %u                       <- defined: unsigned arithmetic wraps\n", u);
    assert(u == 0);

    int64_t total = 0;
    bool ok = add_i64_checked(INT64_MAX - 10, 5, &total);
    assert(ok && total == INT64_MAX - 5);
    ok = add_i64_checked(INT64_MAX - 10, 50, &total);
    printf("add_i64_checked(INT64_MAX - 10, 50)   -> %s\n", ok ? "ok" : "rejected, would overflow");
    assert(!ok);

    /* GCC and Clang builtins do the same in one instruction plus a flag test (jo). C23 adds
     * the portable spelling ckd_add / ckd_mul in <stdckdint.h>. MSVC has neither builtin. */
    long long r;
    bool overflowed = __builtin_add_overflow(LLONG_MAX, 1, &r);
    printf("__builtin_add_overflow(LLONG_MAX, 1)  -> %s\n", overflowed ? "overflow detected" : "ok");
    assert(overflowed);

    /* The malloc(count * size) bug: the multiplication wraps and you allocate a tiny buffer. */
    size_t bytes = 0;
    ok = mul_size_checked((size_t)1 << 62, 8, &bytes);
    printf("mul_size_checked(2^62, 8)             -> %s\n", ok ? "ok" : "rejected, would wrap to 0");
    assert(!ok);
    printf("unchecked (size_t)2^62 * 8            =  %zu\n", ((size_t)1 << 62) * 8);

    /* The other signed overflows people forget: -INT_MIN, INT_MIN / -1, abs(INT_MIN). */
    printf("INT_MIN = %d has no positive partner: INT_MAX = %d\n", INT_MIN, INT_MAX);
    return 0;
}
