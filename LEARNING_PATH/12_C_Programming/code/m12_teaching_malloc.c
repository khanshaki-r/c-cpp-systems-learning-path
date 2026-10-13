/* m12_teaching_malloc.c - a small but real malloc/free: block headers and footers (boundary
 * tags), an explicit doubly linked free list, first-fit search, splitting and coalescing.
 * Read this file first, then 05_Memory_Debugging_RCA/03_the_heap_malloc_internals.md.
 * glibc's ptmalloc2 is this design plus size-segregated bins, per-thread caches and arenas.
 *
 * Heap layout (offsets from the start of the 64 KiB region):
 *
 *   0       8          16         24        32
 *   | pad   | prologue | prologue | block   | payload ...          | ... | epilogue |
 *   |       | header   | footer   | header  | (16-byte aligned)    |     | header   |
 *
 * Every block: [header 8][payload ...][footer 8]. Header == footer == size | used-bit.
 * Sizes are multiples of 16, so the low 4 bits are free for flags; bit 0 = in use.
 * A FREE block stores two pointers at the start of its payload: next free, prev free.
 * Minimum block = 8 + 8 + 8 + 8 = 32 bytes.
 * The prologue and epilogue are permanently "in use", so coalescing never runs off the ends.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m12_teaching_malloc m12_teaching_malloc.c
 * Run:    ./m12_teaching_malloc
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

#define HEAP_SIZE (64u * 1024u)
#define WSIZE     8u            /* header or footer */
#define ALIGNMENT 16u
#define MIN_BLOCK 32u

typedef unsigned char byte;

/* The region comes from aligned_alloc, not from a static array: a static unsigned char array
 * has a declared type, and storing other types into it is technically an aliasing violation.
 * Memory from malloc (or from mmap/sbrk, for a real allocator) has no declared type. */
static byte  *g_heap;
static byte  *g_first;       /* payload pointer of the first real block */
static byte  *g_free_head;   /* payload pointer of the first free block, or NULL */
static size_t g_bad_frees;   /* double frees and invalid frees detected */

/* memcpy-based word access: no alignment or aliasing assumptions at all. */
static size_t get_word(const byte *p)        { size_t v; memcpy(&v, p, sizeof v); return v; }
static void   put_word(byte *p, size_t v)    { memcpy(p, &v, sizeof v); }
static byte  *get_ptr(const byte *p)         { byte *v; memcpy(&v, p, sizeof v); return v; }
static void   put_ptr(byte *p, byte *v)      { memcpy(p, &v, sizeof v); }

static size_t tag_size(const byte *tag)      { return get_word(tag) & ~(size_t)0xF; }
static int    tag_used(const byte *tag)      { return (int)(get_word(tag) & 1u); }

static byte *hdr(byte *bp)                   { return bp - WSIZE; }
static byte *ftr(byte *bp)                   { return bp + tag_size(hdr(bp)) - 2 * WSIZE; }
static byte *next_blk(byte *bp)              { return bp + tag_size(hdr(bp)); }
static byte *prev_ftr(byte *bp)              { return bp - 2 * WSIZE; }
static byte *prev_blk(byte *bp)              { return bp - tag_size(prev_ftr(bp)); }

static void set_block(byte *bp, size_t size, int used)
{
    put_word(bp - WSIZE, size | (size_t)used);               /* header */
    put_word(bp + size - 2 * WSIZE, size | (size_t)used);    /* footer */
}

/* ---- explicit free list: LIFO insertion, O(1) removal thanks to the prev pointer ---- */
static void list_insert(byte *bp)
{
    put_ptr(bp, g_free_head);          /* next */
    put_ptr(bp + WSIZE, NULL);         /* prev */
    if (g_free_head)
        put_ptr(g_free_head + WSIZE, bp);
    g_free_head = bp;
}

static void list_remove(byte *bp)
{
    byte *next = get_ptr(bp), *prev = get_ptr(bp + WSIZE);
    if (prev)
        put_ptr(prev, next);
    else
        g_free_head = next;
    if (next)
        put_ptr(next + WSIZE, prev);
}

static int tm_init(void)
{
    g_heap = aligned_alloc(ALIGNMENT, HEAP_SIZE);
    if (!g_heap)
        return -1;
    put_word(g_heap + 8, 16u | 1u);                  /* prologue header */
    put_word(g_heap + 16, 16u | 1u);                 /* prologue footer */
    put_word(g_heap + HEAP_SIZE - WSIZE, 0u | 1u);   /* epilogue header: size 0, in use */
    g_first = g_heap + 32;
    set_block(g_first, HEAP_SIZE - 32, 0);           /* one big free block in between */
    g_free_head = NULL;
    g_bad_frees = 0;
    list_insert(g_first);
    return 0;
}

static void tm_shutdown(void)
{
    free(g_heap);
    g_heap = g_first = g_free_head = NULL;
}

/* Use free block bp for a request of asize bytes; split off the tail if it is big enough
 * to be a block of its own, otherwise hand out the whole block (internal fragmentation). */
static void place(byte *bp, size_t asize)
{
    size_t bsize = tag_size(hdr(bp));
    list_remove(bp);
    if (bsize - asize >= MIN_BLOCK) {
        set_block(bp, asize, 1);
        byte *rest = bp + asize;
        set_block(rest, bsize - asize, 0);
        list_insert(rest);
    } else {
        set_block(bp, bsize, 1);
    }
}

static void *tm_malloc(size_t n)
{
    if (n == 0 || n > HEAP_SIZE)
        return NULL;
    size_t asize = (n + 2 * WSIZE + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);
    if (asize < MIN_BLOCK)
        asize = MIN_BLOCK;
    for (byte *bp = g_free_head; bp; bp = get_ptr(bp)) {     /* first fit */
        if (tag_size(hdr(bp)) >= asize) {
            place(bp, asize);
            return bp;
        }
    }
    return NULL;   /* a real malloc would now ask the kernel for more: brk or mmap */
}

static void tm_free(void *ptr)
{
    if (!ptr)
        return;
    byte *bp = ptr;
    if (bp < g_first || bp >= g_heap + HEAP_SIZE || !tag_used(hdr(bp))) {
        g_bad_frees++;          /* glibc would abort: "free(): double free detected" */
        return;
    }
    size_t size = tag_size(hdr(bp));
    byte *next = next_blk(bp);
    int prev_used = tag_used(prev_ftr(bp));    /* the boundary tag makes this O(1) */
    int next_used = tag_used(hdr(next));

    if (!next_used) {                          /* merge with the following block */
        list_remove(next);
        size += tag_size(hdr(next));
    }
    if (!prev_used) {                          /* merge with the preceding block */
        byte *prev = prev_blk(bp);
        list_remove(prev);
        size += tag_size(hdr(prev));
        bp = prev;
    }
    set_block(bp, size, 0);
    list_insert(bp);
}

/* ---- heap checker: the function you write FIRST when building an allocator ---- */
struct tm_stats {
    size_t free_blocks, free_bytes, largest_free, used_blocks;
};

static int tm_check(struct tm_stats *st)
{
    memset(st, 0, sizeof *st);
    int prev_free = 0;
    byte *bp = g_first;
    for (; tag_size(hdr(bp)) > 0; bp = next_blk(bp)) {
        size_t size = tag_size(hdr(bp));
        if ((uintptr_t)bp % ALIGNMENT != 0) return -1;            /* misaligned payload */
        if (size % ALIGNMENT != 0 || size < MIN_BLOCK) return -2; /* impossible size */
        if (get_word(hdr(bp)) != get_word(ftr(bp))) return -3;    /* header != footer */
        int is_free = !tag_used(hdr(bp));
        if (is_free && prev_free) return -4;                       /* missed a coalesce */
        if (is_free) {
            st->free_blocks++;
            st->free_bytes += size;
            if (size > st->largest_free)
                st->largest_free = size;
        } else {
            st->used_blocks++;
        }
        prev_free = is_free;
    }
    if (bp != g_heap + HEAP_SIZE) return -5;                       /* walk ran off the end */
    size_t listed = 0;
    for (byte *f = g_free_head; f; f = get_ptr(f)) {
        if (tag_used(hdr(f))) return -6;                           /* used block on free list */
        if (++listed > st->free_blocks) return -7;                 /* cycle or stale entry */
    }
    return listed == st->free_blocks ? 0 : -8;
}

static uint32_t g_seed = 2024u;
static uint32_t next_rand(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

int main(void)
{
    struct tm_stats st;
    int rc = tm_init();
    assert(rc == 0);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 1 && st.free_bytes == HEAP_SIZE - 32);

    /* splitting: each request takes a block rounded up to 16 with 16 bytes of tags */
    byte *a = tm_malloc(100), *b = tm_malloc(200), *c = tm_malloc(300);
    assert(a && b && c);
    assert(tag_size(hdr(a)) == 128 && tag_size(hdr(b)) == 224 && tag_size(hdr(c)) == 320);
    assert((uintptr_t)a % 16 == 0 && (uintptr_t)b % 16 == 0 && (uintptr_t)c % 16 == 0);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    memset(c, 0xCC, 300);
    rc = tm_check(&st);
    assert(rc == 0 && st.used_blocks == 3 && st.free_blocks == 1);

    /* coalescing: free the middle, then its neighbours */
    tm_free(b);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 2);          /* b, and the tail */
    tm_free(a);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 2);          /* a merged forward into b */
    tm_free(c);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 1 && st.free_bytes == HEAP_SIZE - 32);
    printf("after freeing a, b, c: %zu free block of %zu bytes (fully coalesced)\n",
           st.free_blocks, st.free_bytes);

    tm_free(a);                                       /* double free: detected, ignored */
    assert(g_bad_frees == 1);

    /* stress: random sizes, random order, contents verified, heap checked after every op */
    enum { SLOTS = 64 };
    byte *live[SLOTS] = { 0 };
    size_t len[SLOTS] = { 0 };
    size_t failures = 0;
    for (int op = 0; op < 20000; op++) {
        size_t i = next_rand() % SLOTS;
        if (live[i]) {
            for (size_t k = 0; k < len[i]; k++)
                assert(live[i][k] == (byte)(i + 1));  /* nobody overwrote us */
            tm_free(live[i]);
            live[i] = NULL;
        } else {
            len[i] = 1 + next_rand() % 2000;
            live[i] = tm_malloc(len[i]);
            if (!live[i]) {
                failures++;
                continue;
            }
            memset(live[i], (int)(i + 1), len[i]);
        }
        rc = tm_check(&st);
        assert(rc == 0);
    }
    for (size_t i = 0; i < SLOTS; i++)
        tm_free(live[i]);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 1 && g_bad_frees == 1);
    printf("stress: 20000 random ops, heap valid after each, %zu out-of-memory results\n", failures);

    /* external fragmentation: plenty of free bytes, no single block big enough */
    byte *small[1024];
    size_t k = 0;
    while (k < 1024 && (small[k] = tm_malloc(48)) != NULL)
        k++;
    for (size_t i = 0; i < k; i += 2)
        tm_free(small[i]);
    rc = tm_check(&st);
    assert(rc == 0);
    void *wish = tm_malloc(100);
    printf("fragmented: %zu bytes free in %zu blocks, largest %zu -> malloc(100) %s\n",
           st.free_bytes, st.free_blocks, st.largest_free, wish ? "succeeded" : "FAILED");
    assert(wish == NULL && st.free_bytes > 100);
    for (size_t i = 1; i < k; i += 2)
        tm_free(small[i]);
    rc = tm_check(&st);
    assert(rc == 0 && st.free_blocks == 1);

    tm_shutdown();
    puts("all checks passed");
    return 0;
}
