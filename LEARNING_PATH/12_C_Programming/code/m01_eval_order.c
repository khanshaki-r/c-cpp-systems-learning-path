/* m01_eval_order.c - what the C abstract machine promises about ORDER, and what it leaves open.
 *
 * Module: 12_C_Programming/01_how_c_really_works.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m01_eval_order.c -o m01_eval_order
 *
 * Nothing here is undefined behaviour. The order in which the operands of + and the arguments
 * of a function call are evaluated is UNSPECIFIED: the compiler chooses, may choose differently
 * at -O2 or on another platform, and does not have to document it. The program prints what GCC
 * chose and asserts only on what the standard guarantees.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char trace[8];   /* records the order in which note() was called */
static int  pos;

static void reset(void) { pos = 0; trace[0] = '\0'; }

static int note(char c, int value)
{
    trace[pos++] = c;
    trace[pos] = '\0';
    return value;
}

static int three(int a, int b, int c) { return a * 100 + b * 10 + c; }

int main(void)
{
    reset();
    int sum = note('a', 1) + note('b', 2);              /* order of the two calls: unspecified */
    printf("note(a) + note(b)     called in order \"%s\", value %d\n", trace, sum);
    assert(sum == 3);                                    /* the VALUE is defined, the order is not */

    reset();
    int packed = three(note('x', 1), note('y', 2), note('z', 3));   /* argument order: unspecified */
    printf("three(x, y, z)        called in order \"%s\", value %d\n", trace, packed);
    assert(packed == 123);

    reset();
    int r = note('p', 0) && note('q', 1);               /* &&: left first; right only if needed */
    printf("p && q                called \"%s\", value %d\n", trace, r);
    assert(strcmp(trace, "p") == 0 && r == 0);

    reset();
    r = note('m', 1) || note('n', 1);                   /* ||: left first; right skipped if true */
    printf("m || n                called \"%s\", value %d\n", trace, r);
    assert(strcmp(trace, "m") == 0 && r == 1);

    reset();
    r = (note('s', 1), note('t', 2));                   /* comma operator: s, then t; value of t */
    printf("(s, t)                called \"%s\", value %d\n", trace, r);
    assert(strcmp(trace, "st") == 0 && r == 2);

    reset();
    r = note('c', 1) ? note('d', 10) : note('e', 20);   /* ?: condition first, then ONE branch */
    printf("c ? d : e             called \"%s\", value %d\n", trace, r);
    assert(strcmp(trace, "cd") == 0 && r == 10);

    puts("all guaranteed orderings held");
    return 0;
}
