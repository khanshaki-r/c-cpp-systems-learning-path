/* m04_arena_allocator.c - a bump (arena) allocator with alignment and an explicit ownership contract.
 *
 * Module: 12_C_Programming/04_memory_storage_scope_and_lifetime.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m04_arena_allocator.c -o m04_arena_allocator
 *
 * OWNERSHIP CONTRACT (the comment every arena-using module should carry):
 *   1. The arena OWNS one block from malloc. Every pointer handed out by arena_alloc() is
 *      BORROWED from the arena; callers never free() it.
 *   2. A borrowed pointer is valid until the next arena_reset() or arena_destroy() of THAT arena.
 *      Storing it anywhere that outlives the reset is a use-after-free waiting to happen.
 *   3. Allocation failure returns NULL; the arena stays valid.
 *   4. Not thread-safe. One arena per thread, or per request.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *base;
    size_t         cap;
    size_t         used;
    size_t         high_water;    /* most bytes ever in use: tells you how to size cap */
} arena;

static int arena_init(arena *a, size_t cap)
{
    a->base = malloc(cap);        /* malloc's result is aligned for any type (max_align_t) */
    if (a->base == NULL) return -1;
    a->cap = cap;
    a->used = 0;
    a->high_water = 0;
    return 0;
}

/* align must be a power of two. Rounds the current position up to a multiple of align. */
static void *arena_alloc(arena *a, size_t size, size_t align)
{
    assert(align != 0 && (align & (align - 1)) == 0);
    uintptr_t cur = (uintptr_t)(a->base + a->used);
    uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t pad = (size_t)(aligned - cur);
    /* Written as subtractions so that a huge `size` cannot wrap the check. */
    if (pad > a->cap - a->used || size > a->cap - a->used - pad) return NULL;
    a->used += pad + size;
    if (a->used > a->high_water) a->high_water = a->used;
    return (void *)aligned;
}

#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T), alignof(T)))

static char *arena_strdup(arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(a, n, 1);
    if (p != NULL) memcpy(p, s, n);
    return p;
}

static void arena_reset(arena *a) { a->used = 0; }     /* frees EVERYTHING at once, O(1) */

static void arena_destroy(arena *a)
{
    free(a->base);
    a->base = NULL;
    a->cap = a->used = 0;
}

typedef struct {
    int64_t  id;
    char    *name;          /* borrowed from the same arena: same lifetime as the record */
    int32_t  rate_bp;
} customer;

int main(void)
{
    arena a;
    if (arena_init(&a, 4096) != 0) return 1;

    char *flag = arena_alloc(&a, 1, 1);            /* 1 byte: the next allocation needs padding */
    customer *c = ARENA_NEW(&a, customer);
    assert(flag != NULL && c != NULL);
    assert((uintptr_t)c % alignof(customer) == 0);
    printf("1-byte alloc, then customer (size %zu, align %zu) placed at offset %td; used = %zu\n",
           sizeof(customer), alignof(customer), (unsigned char *)c - a.base, a.used);

    c->id = 42;
    c->rate_bp = 725;
    c->name = arena_strdup(&a, "R. Kumar");
    assert(c->name != NULL && strcmp(c->name, "R. Kumar") == 0);
    printf("customer %" PRId64 " \"%s\" at %d bp; used = %zu\n", c->id, c->name, (int)c->rate_bp, a.used);

    /* Per-request pattern: allocate freely while handling a request, reset when it is done. */
    for (int req = 1; req <= 3; ++req) {
        arena_reset(&a);
        for (int i = 0; i < req * 10; ++i)
            if (ARENA_NEW(&a, customer) == NULL) return 1;
        printf("request %d used %zu bytes\n", req, a.used);
    }
    printf("high-water mark: %zu of %zu bytes\n", a.high_water, a.cap);

    /* Exhaustion is a clean NULL, not a crash. */
    arena_reset(&a);
    assert(arena_alloc(&a, 5000, 8) == NULL);
    assert(arena_alloc(&a, SIZE_MAX, 8) == NULL);  /* no wrap-around in the bounds check */
    arena_destroy(&a);
    puts("arena destroyed: every borrowed pointer is now invalid");
    return 0;
}
