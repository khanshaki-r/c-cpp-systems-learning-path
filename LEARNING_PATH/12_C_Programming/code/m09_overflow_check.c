/* m09_overflow_check.c - an overflow check the compiler is allowed to delete,
 * and four correct ways to write it.
 *
 * DELIBERATE UB: naive_add_overflows() computes a + b BEFORE checking. When the sum does not
 * fit in an int that is signed overflow, which is undefined behaviour. GCC therefore assumes it
 * never happens and folds "a + b < a" into "b < 0" - even at -O0, because the folding happens
 * in the front end, not in the optimiser.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m09_overflow_check m09_overflow_check.c
 * Run:    ./m09_overflow_check
 *
 * Compare (same source, different contracts with the compiler):
 *   gcc -std=c17 -O2 -fwrapv -o m09_wrapv m09_overflow_check.c && ./m09_wrapv
 *       -fwrapv defines signed overflow as two's-complement wrap: the naive check now "works".
 *   gcc -std=c17 -O0 -g -fsanitize=undefined -o m09_ubsan m09_overflow_check.c && ./m09_ubsan
 *       UBSan reports "signed integer overflow: 2147483647 + 1 cannot be represented".
 *   gcc -std=c17 -O2 -S -o - m09_overflow_check.c | sed -n '/^naive_add_overflows:/,/ret/p'
 *       shows the function body is "xorl %eax,%eax; ret" - it always returns false.
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* WRONG: by the time we compare, the overflow (UB) has already happened. Not static, and
 * noinline, so you can find it by name in the assembly. */
__attribute__((noinline)) bool naive_add_overflows(int a, int b)
{
    return b > 0 && a + b < a;
}

/* RIGHT 1: compare against the limit BEFORE adding. Works on every compiler. */
static bool add_overflows_precheck(int a, int b)
{
    if (b > 0)
        return a > INT_MAX - b;   /* INT_MAX - b cannot overflow when b > 0 */
    if (b < 0)
        return a < INT_MIN - b;   /* INT_MIN - b cannot overflow when b < 0 */
    return false;
}

/* RIGHT 2: let the compiler do it (GCC 5+, Clang). C23 standardises this as ckd_add()
 * in <stdckdint.h>, which GCC ships from version 14. */
static bool add_overflows_builtin(int a, int b, int *sum)
{
    return __builtin_add_overflow(a, b, sum);
}

/* RIGHT 3: do the arithmetic in a wider type, then range-check. Fine when a wider type exists. */
static bool add_overflows_wide(int a, int b)
{
    long long s = (long long)a + b;
    return s > INT_MAX || s < INT_MIN;
}

/* RIGHT 4: the size_t version you need before every malloc(count * size). Unsigned
 * arithmetic wraps (defined, not UB), but a wrapped size is still a security bug. */
static bool mul_size_overflows(size_t count, size_t size)
{
    return size != 0 && count > SIZE_MAX / size;
}

int main(void)
{
    /* volatile: stops the compiler from constant-folding main() itself, so each function
     * really receives INT_MAX at run time. */
    volatile int big = INT_MAX;
    int a = big;
    int sum = 0;

    printf("naive_add_overflows(INT_MAX, 1)    = %d   <- 0 means the check was folded away\n",
           naive_add_overflows(a, 1));
    printf("add_overflows_precheck(INT_MAX, 1) = %d\n", add_overflows_precheck(a, 1));
    printf("add_overflows_builtin(INT_MAX, 1)  = %d\n", add_overflows_builtin(a, 1, &sum));
    printf("add_overflows_wide(INT_MAX, 1)     = %d\n", add_overflows_wide(a, 1));

    /* The correct checks must agree with each other on edge cases. */
    assert(add_overflows_precheck(INT_MAX, 1));
    assert(add_overflows_precheck(INT_MIN, -1));
    assert(!add_overflows_precheck(INT_MAX, 0));
    assert(!add_overflows_precheck(INT_MIN, 0));
    assert(!add_overflows_precheck(-5, 3));
    assert(!add_overflows_precheck(INT_MAX, INT_MIN));

    assert(add_overflows_builtin(INT_MAX, 1, &sum));
    assert(!add_overflows_builtin(40, 2, &sum) && sum == 42);
    assert(add_overflows_builtin(INT_MIN, -1, &sum));

    assert(add_overflows_wide(INT_MIN, -1));
    assert(!add_overflows_wide(INT_MAX - 1, 1));

    for (int x = -3; x <= 3; x++) {
        int edges[] = { INT_MIN, INT_MIN + 1, -1, 0, 1, INT_MAX - 1, INT_MAX };
        for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
            int s;
            bool p = add_overflows_precheck(edges[i], x);
            bool b = add_overflows_builtin(edges[i], x, &s);
            bool w = add_overflows_wide(edges[i], x);
            assert(p == b && b == w);
        }
    }

    assert(mul_size_overflows(SIZE_MAX / 2 + 1, 2));
    assert(!mul_size_overflows(1000, 8));
    assert(!mul_size_overflows(SIZE_MAX, 0));

    puts("all checks passed");
    return 0;
}
