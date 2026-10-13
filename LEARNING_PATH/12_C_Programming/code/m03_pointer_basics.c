/* m03_pointer_basics.c - addresses, * and &, and pointer arithmetic that is scaled by element size.
 *
 * Module: 12_C_Programming/03_pointers_from_the_ground_up.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_pointer_basics.c -o m03_pointer_basics
 *
 * Addresses change on every run (ASLR), so the program prints DIFFERENCES, which do not.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static void set_to_42(int *p) { *p = 42; }   /* the only "pass by reference" C has: pass the address */

struct account {
    int       id;
    long long balance_paise;
};

int main(void)
{
    int x = 7;
    int *p = &x;              /* p holds the address of x */
    *p = 8;                   /* write through p: x changes */
    assert(x == 8);
    set_to_42(&x);
    assert(x == 42 && *p == 42);
    printf("x = %d, *p = %d, p == &x is %s\n", x, *p, p == &x ? "true" : "false");

    int    ia[4] = {10, 20, 30, 40};
    double da[4] = {1.5, 2.5, 3.5, 4.5};
    char   ca[4] = "abc";

    /* p + 1 advances by sizeof(*p) BYTES. Casting to char * lets us measure in bytes. */
    printf("int *    p + 1 moves %td bytes\n", (char *)(ia + 1) - (char *)ia);
    printf("double * p + 1 moves %td bytes\n", (char *)(da + 1) - (char *)da);
    printf("char *   p + 1 moves %td bytes\n", (ca + 1) - ca);

    /* a[i] is DEFINED as *(a + i). */
    assert(ia[2] == *(ia + 2));
    assert(2[ia] == 30);      /* legal because *(2 + ia) is the same thing. Never write this. */

    /* Subtracting pointers counts ELEMENTS, and the result type is ptrdiff_t. */
    int *first = &ia[0];
    int *last = &ia[3];
    ptrdiff_t n = last - first;
    printf("&ia[3] - &ia[0] = %td elements (%td bytes)\n", n, (char *)last - (char *)first);
    assert(n == 3);

    /* The idiomatic pointer walk. ia + 4 is "one past the end": you may form it and compare
     * with it, but never dereference it. */
    int sum = 0;
    for (const int *it = ia; it != ia + 4; ++it)
        sum += *it;
    assert(sum == 100);
    printf("sum by pointer walk = %d\n", sum);

    /* p->field is shorthand for (*p).field */
    struct account acc = {1, 500};
    struct account *ap = &acc;
    ap->balance_paise += 100;
    assert((*ap).balance_paise == 600);

    /* NULL means "points to nothing". Check before dereferencing. */
    int *nothing = NULL;
    printf("nothing is %s\n", nothing == NULL ? "NULL" : "not NULL");

    /* An address as an integer: uintptr_t. Useful for alignment checks. */
    uintptr_t addr = (uintptr_t)(void *)&ia[1];
    printf("(uintptr_t)&ia[1] %% _Alignof(int) = %zu\n", (size_t)(addr % _Alignof(int)));
    assert(addr % _Alignof(int) == 0);
    return 0;
}
