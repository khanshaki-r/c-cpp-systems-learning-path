/* m12_arena.c - an arena (bump) allocator with an explicit ownership contract, used the way a
 * batch-oriented data-migration job uses one: allocate everything for one batch from the
 * arena, write the batch out, reset the arena, repeat.
 *
 * OWNERSHIP CONTRACT (this is the part that prevents bugs; put it in the header):
 *   1. Memory from arena_alloc() is owned by the arena. Never free() it.
 *   2. It stays valid until arena_reset(), arena_rewind() past it, or arena_destroy().
 *   3. Nothing allocated from an arena may be stored in a structure that outlives the
 *      arena's next reset (a global cache, another thread's queue). Copy it out instead.
 *   4. An arena is not thread-safe. Use one per thread or one per batch.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m12_arena m12_arena.c
 * Run:    ./m12_arena
 */
#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* Count real malloc calls so the demo can prove the steady state does none. */
static size_t g_mallocs;
static void *counted_malloc(size_t n) { g_mallocs++; return malloc(n); }

/* Round x up to a multiple of a, where a is a power of two.
 *   a - 1 is a mask of the low bits, e.g. a = 16 -> 0b1111.
 *   x + (a - 1) pushes x past the next boundary unless it is already on one.
 *   & ~(a - 1) clears the low bits, landing exactly on the boundary. */
static uintptr_t align_up(uintptr_t x, size_t a)
{
    assert(a != 0 && (a & (a - 1)) == 0);
    return (x + (a - 1)) & ~(uintptr_t)(a - 1);
}

struct chunk {
    struct chunk *prev;                    /* chunks form a stack: newest first */
    size_t        cap;
    size_t        used;
    alignas(max_align_t) unsigned char data[];
};

struct arena {
    struct chunk *head;
    size_t        chunk_size;              /* default capacity for new chunks */
};

struct arena_mark {
    struct chunk *chunk;
    size_t        used;
};

static void arena_init(struct arena *a, size_t chunk_size)
{
    a->head = NULL;
    a->chunk_size = chunk_size;
}

static struct chunk *chunk_new(size_t cap)
{
    struct chunk *c = counted_malloc(sizeof *c + cap);
    if (!c)
        return NULL;
    c->prev = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

/* Try to carve size bytes at alignment align out of chunk c. */
static void *chunk_take(struct chunk *c, size_t size, size_t align)
{
    uintptr_t base = (uintptr_t)c->data;
    uintptr_t p = align_up(base + c->used, align);
    size_t offset = (size_t)(p - base);
    if (offset > c->cap || size > c->cap - offset)   /* written so nothing can overflow */
        return NULL;
    c->used = offset + size;
    return (void *)p;
}

static void *arena_alloc(struct arena *a, size_t size, size_t align)
{
    if (a->head) {
        void *p = chunk_take(a->head, size, align);
        if (p)
            return p;
    }
    /* New chunk, big enough for this request even if it exceeds chunk_size. */
    if (size > SIZE_MAX - align)
        return NULL;
    size_t need = size + align;
    struct chunk *c = chunk_new(need > a->chunk_size ? need : a->chunk_size);
    if (!c)
        return NULL;
    c->prev = a->head;
    a->head = c;
    return chunk_take(c, size, align);
}

#define ARENA_NEW(a, T)        ((T *)arena_alloc((a), sizeof(T), alignof(T)))
#define ARENA_ARRAY(a, T, n)   ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))

static void *arena_alloc_array(struct arena *a, size_t n, size_t size, size_t align)
{
    if (size != 0 && n > SIZE_MAX / size)
        return NULL;
    return arena_alloc(a, n * size, align);
}

static char *arena_strdup(struct arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(a, n, 1);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Scratch usage: remember a position, allocate temporaries, roll back to it. */
static struct arena_mark arena_save(const struct arena *a)
{
    struct arena_mark m = { a->head, a->head ? a->head->used : 0 };
    return m;
}

static void arena_rewind(struct arena *a, struct arena_mark m)
{
    while (a->head != m.chunk) {
        struct chunk *dead = a->head;
        a->head = dead->prev;
        free(dead);
    }
    if (a->head)
        a->head->used = m.used;
}

/* Free everything, but keep ONE chunk sized to the high-water mark, so a workload that
 * repeats (batch after batch) settles into zero malloc calls per batch. */
static int arena_reset(struct arena *a)
{
    size_t total = 0, chunks = 0;
    for (struct chunk *c = a->head; c; c = c->prev) {
        total += c->cap;
        chunks++;
    }
    if (chunks <= 1) {
        if (a->head)
            a->head->used = 0;
        return 0;
    }
    arena_rewind(a, (struct arena_mark){ NULL, 0 });
    a->head = chunk_new(total);
    return a->head ? 0 : -1;
}

static void arena_destroy(struct arena *a)
{
    arena_rewind(a, (struct arena_mark){ NULL, 0 });
}

/* ---------------- the batch workload ---------------- */
struct row {
    long long id;
    char     *account;     /* points into the same arena */
    long long balance_paise;
};

/* Convert one batch: every row and every string comes from the arena, nothing is freed
 * individually, and the whole batch disappears with one arena_reset(). */
static long long process_batch(struct arena *a, int batch_no, int rows)
{
    struct row **out = ARENA_ARRAY(a, struct row *, (size_t)rows);
    assert(out != NULL);
    char name[32];
    long long checksum = 0;
    for (int i = 0; i < rows; i++) {
        struct row *r = ARENA_NEW(a, struct row);
        assert(r != NULL);
        snprintf(name, sizeof name, "ACC-%d-%05d", batch_no, i);
        r->id = (long long)batch_no * rows + i;
        r->account = arena_strdup(a, name);
        r->balance_paise = r->id * 10;
        assert(r->account != NULL);
        assert((uintptr_t)r % alignof(struct row) == 0);
        out[i] = r;
        checksum += r->balance_paise;
    }
    return checksum;
}

int main(void)
{
    struct arena a;
    arena_init(&a, 4096);

    /* alignment: every request lands on its boundary, whatever came before it */
    char *c1 = arena_alloc(&a, 1, 1);
    double *d = ARENA_NEW(&a, double);
    char *c2 = arena_alloc(&a, 3, 1);
    void *v64 = arena_alloc(&a, 10, 64);
    assert(c1 && d && c2 && v64);
    assert((uintptr_t)d % alignof(double) == 0);
    assert((uintptr_t)v64 % 64 == 0);
    assert(align_up(13, 8) == 16 && align_up(16, 8) == 16 && align_up(0, 16) == 0);

    /* scratch: mark, allocate temporaries, rewind, and the space is reused */
    struct arena_mark m = arena_save(&a);
    char *tmp1 = arena_alloc(&a, 100, 1);
    arena_rewind(&a, m);
    char *tmp2 = arena_alloc(&a, 100, 1);
    assert(tmp1 == tmp2);

    /* an allocation bigger than a chunk gets its own chunk */
    void *big = arena_alloc(&a, 10000, 16);
    assert(big != NULL);
    arena_destroy(&a);

    /* the batch loop */
    arena_init(&a, 16 * 1024);
    size_t before = g_mallocs;
    long long first = process_batch(&a, 0, 1000);
    size_t first_batch_mallocs = g_mallocs - before;
    int rc = arena_reset(&a);
    assert(rc == 0);

    before = g_mallocs;
    for (int b = 1; b <= 50; b++) {
        long long sum = process_batch(&a, 0, 1000);   /* same shape every time */
        assert(sum == first);
        rc = arena_reset(&a);
        assert(rc == 0);
    }
    size_t steady_mallocs = g_mallocs - before;
    printf("batch 1: %zu malloc calls; batches 2-51: %zu malloc calls in total\n",
           first_batch_mallocs, steady_mallocs);
    assert(first_batch_mallocs > 1);
    assert(steady_mallocs == 0);

    arena_destroy(&a);
    puts("all checks passed");
    return 0;
}
