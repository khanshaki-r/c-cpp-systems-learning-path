/* m09_null_check_deleted.c - the compiler deletes a null check that comes AFTER the pointer
 * was already used.
 *
 * DELIBERATE UB: main() calls copy_then_check(dst, NULL, 0). Passing a null pointer to memcpy
 * is undefined even when the length is zero (C17 7.24.1p2), and glibc declares memcpy's
 * pointer parameters nonnull. GCC uses that to conclude "src is not NULL" after the call and
 * removes the check below it.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m09_null_check_deleted m09_null_check_deleted.c
 * Run:    ./m09_null_check_deleted
 * Compare:
 *   build with -O0, or with -O2 -fno-delete-null-pointer-checks, and run again: the check
 *   survives and copy_then_check returns -1.
 * See the deleted check in record_len():
 *   gcc -std=c17 -O2 -S -o - m09_null_check_deleted.c | sed -n '/^record_len:/,/ret/p'
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct record {
    int  len;
    char data[60];
};

/* The CVE-2009-1897 shape (Linux tun driver): use the pointer first, check it later.
 * If r were NULL, line A is already UB, so the compiler may assume r != NULL and drop
 * line B. At -O2 GCC 13 compiles this to a single load and a return. */
__attribute__((noinline)) int record_len(const struct record *r)
{
    int n = r->len;            /* line A */
    if (r == NULL)             /* line B: deleted at -O2 */
        return -1;
    return n;
}

/* Fixed: check before use. */
__attribute__((noinline)) int record_len_fixed(const struct record *r)
{
    if (r == NULL)
        return -1;
    return r->len;
}

/* A version whose deletion you can SEE at run time without crashing. */
__attribute__((noinline)) int copy_then_check(char *dst, const char *src, size_t n)
{
    memcpy(dst, src, n);       /* UB if src == NULL, even with n == 0 */
    if (src == NULL)           /* GCC -O2: "src was passed to a nonnull parameter, so it is not NULL" */
        return -1;
    return 0;
}

/* Fixed: validate first, and skip the call entirely for n == 0. */
__attribute__((noinline)) int copy_then_check_fixed(char *dst, const char *src, size_t n)
{
    if (src == NULL)
        return -1;
    if (n > 0)
        memcpy(dst, src, n);
    return 0;
}

int main(void)
{
    struct record rec = { .len = 42, .data = "ok" };
    char dst[8] = { 0 };
    /* volatile: hide the NULL from the optimiser so it cannot fold the calls in main(). */
    const char *volatile nothing = NULL;

    assert(record_len(&rec) == 42);
    assert(record_len_fixed(&rec) == 42);
    assert(record_len_fixed(NULL) == -1);

    int r = copy_then_check(dst, nothing, 0);
    printf("copy_then_check(dst, NULL, 0)       = %2d  (-1 means the null check survived)\n", r);
    printf("copy_then_check_fixed(dst, NULL, 0) = %2d\n", copy_then_check_fixed(dst, nothing, 0));

    assert(copy_then_check_fixed(dst, nothing, 0) == -1);
    assert(copy_then_check_fixed(dst, "abc", 4) == 0 && strcmp(dst, "abc") == 0);
    puts("all checks passed");
    return 0;
}
