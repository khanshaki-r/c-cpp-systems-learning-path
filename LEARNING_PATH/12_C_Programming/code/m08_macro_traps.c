/* m08_macro_traps.c - the classic macro bugs, each next to its fix.
 *
 * Module: 12_C_Programming/08_the_preprocessor_and_macros.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m08_macro_traps.c -o m08_macro_traps
 * One trap is silenced locally with #pragma GCC diagnostic (explained at the spot).
 * See the expansions: gcc -std=c17 -E -P m08_macro_traps.c | tail -60
 */
#include <assert.h>
#include <stdio.h>

/* Trap 1: precedence. The argument and the whole body need parentheses. */
#define SQUARE_BAD(x) x * x
#define SQUARE(x)     ((x) * (x))

#define DOUBLE_BAD(x) (x) + (x)
#define DOUBLE(x)     ((x) + (x))

/* Trap 2: double evaluation. The argument text is pasted twice, so i++ runs twice. */
#define MAX_BAD(a, b) ((a) > (b) ? (a) : (b))

static inline int max_int(int a, int b) { return a > b ? a : b; }   /* evaluates each once */

/* Trap 3: multi-statement macros. Without a wrapper, only the first statement is under the if. */
#define BUMP_BOTH_BAD(a, b) (a)++; (b)++
#define BUMP_BOTH(a, b)     do { (a)++; (b)++; } while (0)

/* Why do { } while (0) and not just { }? Because "{ ... };" before an else is a syntax error:
 *   if (x) SWAP_BRACES(a, b); else ...   ->  "if (x) { ... } ; else"  ->  else without if.
 * do { } while (0) is ONE statement that needs exactly the semicolon the caller writes. */

/* Stringising (#) and token pasting (##). */
#define STR(x)  #x
#define XSTR(x) STR(x)          /* expand x first, then stringise the result */
#define CAT(a, b) a##b

static int counter_debit;
static int counter_credit;

int main(void)
{
    int bad = SQUARE_BAD(2 + 3);        /* 2 + 3 * 2 + 3 */
    int good = SQUARE(2 + 3);
    printf("SQUARE_BAD(2 + 3) = %d, SQUARE(2 + 3) = %d\n", bad, good);
    assert(bad == 11 && good == 25);

    int bad2 = 10 * DOUBLE_BAD(3);      /* 10 * (3) + (3) */
    int good2 = 10 * DOUBLE(3);
    printf("10 * DOUBLE_BAD(3) = %d, 10 * DOUBLE(3) = %d\n", bad2, good2);
    assert(bad2 == 33 && good2 == 60);

    int i = 5;
    int m = MAX_BAD(i++, 3);            /* ((i++) > (3) ? (i++) : (3)): i++ runs twice */
    printf("MAX_BAD(i++, 3) = %d and i is now %d   (i was incremented twice)\n", m, i);
    assert(m == 6 && i == 7);
    i = 5;
    m = max_int(i++, 3);
    printf("max_int(i++, 3) = %d and i is now %d\n", m, i);
    assert(m == 5 && i == 6);

    int a = 0, b = 0;
    int enabled = 0;
    /* GCC 7+ catches this one (-Wmultistatement-macros, in -Wall), so it is silenced here only
     * to show what happens. Older compilers and MSVC say nothing. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmultistatement-macros"
    if (enabled)
        BUMP_BOTH_BAD(a, b);            /* expands to: if (enabled) (a)++; (b)++; */
#pragma GCC diagnostic pop
    printf("BUMP_BOTH_BAD under a false if: a = %d, b = %d   (b changed anyway)\n", a, b);
    assert(a == 0 && b == 1);
    a = b = 0;
    if (enabled)
        BUMP_BOTH(a, b);
    else
        a = -1;                         /* compiles: the do-while is a single statement */
    assert(a == -1 && b == 0);

    printf("STR(__LINE__) = %s, XSTR(__LINE__) = %s\n", STR(__LINE__), XSTR(__LINE__));
    CAT(counter_, debit) += 2;          /* becomes counter_debit += 2 */
    CAT(counter_, credit) += 1;
    assert(counter_debit == 2 && counter_credit == 1);
    printf("CAT(counter_, debit) -> counter_debit = %d\n", counter_debit);
    return 0;
}
