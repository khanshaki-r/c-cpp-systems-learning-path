/* m03_void_ptr_and_restrict.c - generic code with void *, and what restrict promises.
 *
 * Module: 12_C_Programming/03_pointers_from_the_ground_up.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_void_ptr_and_restrict.c -o m03_void_ptr_and_restrict
 * Look at the restrict effect: gcc -std=c17 -O2 -S m03_void_ptr_and_restrict.c and compare
 * update_twice with update_twice_restrict in the .s file.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Swap two objects of any type. void * converts to and from any object pointer without a cast
 * in C. Arithmetic on void * is NOT allowed (GCC allows it as an extension; -Wpedantic flags it),
 * so we convert to unsigned char * to walk bytes. */
static void swap_any(void *a, void *b, size_t size)
{
    unsigned char *p = a, *q = b;
    for (size_t i = 0; i < size; ++i) {
        unsigned char t = p[i];
        p[i] = q[i];
        q[i] = t;
    }
}

/* Linear search over any array: base address, count, element size, and an equality callback.
 * This is the same shape as the standard qsort and bsearch. */
static void *find_any(const void *base, size_t n, size_t size, const void *key,
                      int (*equal)(const void *elem, const void *key))
{
    const unsigned char *p = base;
    for (size_t i = 0; i < n; ++i, p += size)
        if (equal(p, key)) return (void *)p;     /* like bsearch: returns non-const */
    return NULL;
}

struct txn {
    int       id;
    long long amount_paise;
};

static int txn_has_id(const void *elem, const void *key)
{
    const struct txn *t = elem;                  /* the callback knows the real type */
    return t->id == *(const int *)key;
}

/* Without restrict the compiler must assume a and b may point to the same int, so it reloads
 * *a after writing *b. With restrict you promise they do not, and it may just return 1. */
static int update_twice(int *a, int *b)
{
    *a = 1;
    *b = 2;
    return *a;
}

static int update_twice_restrict(int *restrict a, int *restrict b)
{
    *a = 1;
    *b = 2;
    return *a;
}

/* memcpy is declared with restrict parameters: overlapping source and destination break the
 * promise, which is why overlapping copies must use memmove. */
static void add_into(int *restrict dst, const int *restrict src, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] += src[i];
}

int main(void)
{
    double x = 1.5, y = 2.5;
    swap_any(&x, &y, sizeof x);
    struct txn t1 = {1, 100}, t2 = {2, 200};
    swap_any(&t1, &t2, sizeof t1);
    printf("after swap_any: x = %.1f, y = %.1f, t1.id = %d, t2.id = %d\n", x, y, t1.id, t2.id);
    assert(x == 2.5 && y == 1.5 && t1.id == 2 && t2.id == 1);

    struct txn book[] = {{10, 500}, {11, -250}, {12, 999}};
    int wanted = 11;
    struct txn *hit = find_any(book, 3, sizeof book[0], &wanted, txn_has_id);
    assert(hit != NULL && hit->amount_paise == -250);
    printf("find_any id 11 -> amount %lld\n", hit->amount_paise);
    wanted = 99;
    assert(find_any(book, 3, sizeof book[0], &wanted, txn_has_id) == NULL);

    int same = 0;
    printf("update_twice(&same, &same) = %d (aliasing handled)\n", update_twice(&same, &same));
    int a = 0, b = 0;
    printf("update_twice_restrict(&a, &b) = %d (no aliasing, promise kept)\n",
           update_twice_restrict(&a, &b));
    /* update_twice_restrict(&same, &same) would break the promise: undefined behaviour. */

    int dst[4] = {1, 1, 1, 1};
    const int src[4] = {1, 2, 3, 4};
    add_into(dst, src, 4);
    assert(dst[0] == 2 && dst[3] == 5);

    /* void * holds any OBJECT pointer. ISO C does not allow converting a FUNCTION pointer to
     * void * (POSIX dlsym needs it anyway, so POSIX requires it to work). */
    void *anything = &x;
    assert(*(double *)anything == 2.5);
    return 0;
}
