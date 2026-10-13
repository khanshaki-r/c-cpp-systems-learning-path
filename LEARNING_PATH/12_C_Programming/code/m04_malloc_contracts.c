/* m04_malloc_contracts.c - what malloc, calloc, realloc and free promise, and the realloc-leak idiom.
 *
 * Module: 12_C_Programming/04_memory_storage_scope_and_lifetime.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m04_malloc_contracts.c -o m04_malloc_contracts
 * Check:  valgrind --leak-check=full ./m04_malloc_contracts   (should report no leaks)
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A growable byte buffer.
 * OWNERSHIP: a struct buf owns `data`. Release with buf_free(). Copying the struct by value
 * creates two owners of one block: do not do it. */
struct buf {
    char  *data;
    size_t len;
    size_t cap;
};

/* Ensures cap >= need. On failure returns -1 and leaves *b UNCHANGED and still valid. */
static int buf_reserve(struct buf *b, size_t need)
{
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return -1;          /* doubling would wrap */
        cap *= 2;
    }
    /* WRONG: b->data = realloc(b->data, cap);
     * If realloc fails it returns NULL, the old block is NOT freed, and you just overwrote the
     * only pointer to it: a leak, and b->data is now NULL while b->len says there is data. */
    char *p = realloc(b->data, cap);
    if (p == NULL) return -1;                       /* b->data still points to the old block */
    b->data = p;                                    /* the block may have MOVED: old pointers into it are dead */
    b->cap = cap;
    return 0;
}

static int buf_append(struct buf *b, const void *src, size_t n)
{
    if (n > SIZE_MAX - b->len) return -1;
    if (buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void buf_free(struct buf *b)
{
    free(b->data);                                  /* free(NULL) is fine: no check needed */
    b->data = NULL;                                 /* a second buf_free is now harmless */
    b->len = b->cap = 0;
}

/* malloc(count * size) with the overflow check calloc does for you. */
static void *alloc_array(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    return malloc(count * size);
}

int main(void)
{
    /* calloc zero-fills; malloc leaves whatever bytes were there. */
    int *z = calloc(8, sizeof *z);
    if (z == NULL) return 1;
    for (int i = 0; i < 8; ++i) assert(z[i] == 0);
    free(z);

    /* calloc checks count * size for overflow and fails cleanly. volatile only hides the
     * constant from GCC, which would otherwise warn at compile time that the size is absurd. */
    volatile size_t huge_count = SIZE_MAX / 2;
    void *huge = calloc(huge_count, 4);
    printf("calloc(SIZE_MAX / 2, 4) -> %s\n", huge == NULL ? "NULL (overflow detected)" : "non-NULL?!");
    assert(huge == NULL);
    assert(alloc_array(huge_count, 4) == NULL);

    /* Edge cases with defined behaviour. */
    free(NULL);                                     /* does nothing */
    char *r = realloc(NULL, 32);                    /* same as malloc(32) */
    if (r == NULL) return 1;
    free(r);
    /* realloc(p, 0) is NOT used: implementation-defined in C17, undefined in C23. */

    struct buf b = {0};
    size_t cap_changes = 0, last_cap = 0;
    for (int i = 0; i < 1000; ++i) {
        if (buf_append(&b, "0123456789", 10) != 0) {
            buf_free(&b);
            return 1;
        }
        if (b.cap != last_cap) {
            ++cap_changes;
            last_cap = b.cap;
        }
    }
    printf("appended %zu bytes; capacity is %zu after %zu reallocations (doubling)\n", b.len, b.cap,
           cap_changes);
    assert(b.len == 10000 && b.cap == 16384 && memcmp(b.data + 9990, "0123456789", 10) == 0);
    buf_free(&b);
    buf_free(&b);                                   /* safe because buf_free nulls the pointer */
    puts("all allocations released");
    return 0;
}
