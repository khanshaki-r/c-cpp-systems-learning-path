/* m03_arrays_and_decay.c - arrays are not pointers; they DECAY to pointers. The sizeof traps.
 *
 * Module: 12_C_Programming/03_pointers_from_the_ground_up.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_arrays_and_decay.c -o m03_arrays_and_decay
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))   /* only valid on a real array, never on a pointer */

/* Writing the parameter as "int arr[10]" would change nothing: it is still an int *.
 * (GCC even warns: "sizeof on array function parameter will return size of 'int *'".) */
static size_t size_seen_by_function(const int *arr)
{
    (void)arr;
    return sizeof arr;
}

static int sum(const int *arr, size_t n)   /* so every array-taking function also takes a length */
{
    int s = 0;
    for (size_t i = 0; i < n; ++i) s += arr[i];
    return s;
}

int main(void)
{
    int a[10] = {1, 2, 3};            /* the rest are zero */

    printf("sizeof a in main           = %zu (10 ints)\n", sizeof a);
    printf("sizeof a inside a function = %zu (just a pointer)\n", size_seen_by_function(a));
    printf("ARRAY_LEN(a)               = %zu\n", ARRAY_LEN(a));
    assert(sizeof a == 10 * sizeof(int));
    assert(sum(a, ARRAY_LEN(a)) == 6);

    /* a, &a[0] and &a have the same address but different TYPES, so +1 means different things. */
    printf("a  + 1 moves %td bytes   (a decays to int *)\n", (char *)(a + 1) - (char *)a);
    printf("&a + 1 moves %td bytes  (&a is int (*)[10])\n", (char *)(&a + 1) - (char *)&a);
    assert((void *)a == (void *)&a);

    /* Array initialised from a literal vs pointer to a literal. */
    char s1[] = "hello";              /* an array of 6 chars: a writable COPY of the literal */
    const char *s2 = "hello";         /* a pointer to the literal itself, which is read-only */
    printf("sizeof s1 = %zu, sizeof s2 = %zu, strlen of both = %zu\n", sizeof s1, sizeof s2, strlen(s2));
    s1[0] = 'H';                      /* fine */
    /* ((char *)s2)[0] = 'H';  compiles, then crashes: literals live in a read-only page. */
    assert(strcmp(s1, "Hello") == 0);

    /* Arrays cannot be assigned or returned, but a struct holding one can. */
    struct ten { int v[10]; } x = {{1, 2, 3}}, y;
    y = x;                            /* copies all 40 bytes */
    assert(y.v[2] == 3 && sizeof y == sizeof x.v);

    /* String literal arrays: an array of pointers. Each element is a pointer (8 bytes here). */
    const char *names[] = {"debit", "credit", "reversal"};
    printf("names has %zu elements of %zu bytes; names[2] = \"%s\"\n", ARRAY_LEN(names),
           sizeof names[0], names[2]);
    assert(ARRAY_LEN(names) == 3);
    return 0;
}
