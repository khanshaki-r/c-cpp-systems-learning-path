/* m11_ring_heap.c - a ring buffer and a generic binary heap.
 *
 *   ring: power-of-two capacity, free-running unsigned head/tail counters. "head - tail" is
 *         the element count even after the counters wrap past UINT_MAX, because unsigned
 *         arithmetic is defined to wrap modulo 2^N. No "full" flag, no wasted slot.
 *   heap: one implementation for any element type: void* storage + element size + comparator,
 *         the same contract as qsort(). Used here for heap sort and for a top-k query.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m11_ring_heap m11_ring_heap.c
 * Run:    ./m11_ring_heap
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ======================= ring buffer ======================= */
struct ring {
    int     *buf;
    unsigned cap;    /* power of two */
    unsigned head;   /* total pushes so far (mod 2^32) */
    unsigned tail;   /* total pops so far   (mod 2^32) */
};

static int ring_init(struct ring *r, unsigned cap)
{
    if (cap == 0 || (cap & (cap - 1)) != 0)
        return EINVAL;
    r->buf = malloc(cap * sizeof *r->buf);
    if (!r->buf)
        return ENOMEM;
    r->cap = cap;
    r->head = r->tail = 0;
    return 0;
}

static unsigned ring_count(const struct ring *r) { return r->head - r->tail; }

static int ring_push(struct ring *r, int v)
{
    if (ring_count(r) == r->cap)
        return -1;                            /* full: caller decides to drop or wait */
    r->buf[r->head & (r->cap - 1)] = v;
    r->head++;
    return 0;
}

static int ring_pop(struct ring *r, int *v)
{
    if (r->head == r->tail)
        return -1;                            /* empty */
    *v = r->buf[r->tail & (r->cap - 1)];
    r->tail++;
    return 0;
}

/* ======================= generic binary heap ======================= */
typedef int (*cmp_fn)(const void *, const void *);

struct heap {
    unsigned char *data;
    size_t         len;
    size_t         cap;
    size_t         esz;
    cmp_fn         cmp;   /* heap order: cmp(parent, child) <= 0, so the root is the minimum */
};

static void heap_init(struct heap *h, size_t esz, cmp_fn cmp)
{
    h->data = NULL;
    h->len = h->cap = 0;
    h->esz = esz;
    h->cmp = cmp;
}

static void heap_free(struct heap *h) { free(h->data); heap_init(h, h->esz, h->cmp); }

static void *heap_at(const struct heap *h, size_t i) { return h->data + i * h->esz; }

static void swap_bytes(void *a, void *b, size_t n)
{
    unsigned char *x = a, *y = b;
    while (n--) {
        unsigned char t = *x;
        *x++ = *y;
        *y++ = t;
    }
}

static void sift_up(struct heap *h, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (h->cmp(heap_at(h, parent), heap_at(h, i)) <= 0)
            break;
        swap_bytes(heap_at(h, parent), heap_at(h, i), h->esz);
        i = parent;
    }
}

static void sift_down(struct heap *h, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < h->len && h->cmp(heap_at(h, l), heap_at(h, best)) < 0) best = l;
        if (r < h->len && h->cmp(heap_at(h, r), heap_at(h, best)) < 0) best = r;
        if (best == i)
            return;
        swap_bytes(heap_at(h, i), heap_at(h, best), h->esz);
        i = best;
    }
}

static int heap_push(struct heap *h, const void *elem)
{
    if (h->len == h->cap) {
        size_t ncap = h->cap ? h->cap * 2 : 16;
        if (ncap > SIZE_MAX / h->esz)
            return ENOMEM;
        void *p = realloc(h->data, ncap * h->esz);
        if (!p)
            return ENOMEM;
        h->data = p;
        h->cap = ncap;
    }
    memcpy(heap_at(h, h->len), elem, h->esz);
    h->len++;
    sift_up(h, h->len - 1);
    return 0;
}

static int heap_pop(struct heap *h, void *out)
{
    if (h->len == 0)
        return -1;
    memcpy(out, heap_at(h, 0), h->esz);
    h->len--;
    if (h->len > 0) {
        memcpy(heap_at(h, 0), heap_at(h, h->len), h->esz);
        sift_down(h, 0);
    }
    return 0;
}

static const void *heap_peek(const struct heap *h) { return h->len ? heap_at(h, 0) : NULL; }

/* ---- element types and comparators ---- */
static int cmp_int_asc(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

struct txn {
    int       id;
    long long amount;
};

static int cmp_txn_amount_asc(const void *a, const void *b)
{
    long long x = ((const struct txn *)a)->amount, y = ((const struct txn *)b)->amount;
    return (x > y) - (x < y);
}

/* deterministic pseudo-random numbers for the test (a 32-bit LCG) */
static uint32_t g_seed = 12345u;
static uint32_t next_rand(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

int main(void)
{
    /* ---- ring ---- */
    struct ring r;
    int err = ring_init(&r, 8);
    assert(err == 0);
    r.head = r.tail = UINT_MAX - 3;         /* start near the wrap point on purpose */
    for (int i = 0; i < 8; i++) {
        err = ring_push(&r, i);
        assert(err == 0);
    }
    assert(ring_push(&r, 99) == -1);        /* full */
    assert(ring_count(&r) == 8);
    assert(r.head < r.tail);                /* head has wrapped, tail has not ... */
    int v;
    for (int i = 0; i < 8; i++) {           /* ... and FIFO order still holds */
        err = ring_pop(&r, &v);
        assert(err == 0 && v == i);
    }
    assert(ring_pop(&r, &v) == -1);         /* empty */
    printf("ring: counters wrapped (head=%u tail=%u), FIFO order kept\n", r.head, r.tail);
    free(r.buf);

    /* ---- heap sort of 1000 ints, compared against qsort ---- */
    enum { N = 1000 };
    int input[N], expected[N];
    for (int i = 0; i < N; i++)
        input[i] = expected[i] = (int)(next_rand() % 100000) - 50000;
    qsort(expected, N, sizeof expected[0], cmp_int_asc);

    struct heap h;
    heap_init(&h, sizeof(int), cmp_int_asc);
    for (int i = 0; i < N; i++) {
        err = heap_push(&h, &input[i]);
        assert(err == 0);
    }
    for (int i = 0; i < N; i++) {
        err = heap_pop(&h, &v);
        assert(err == 0 && v == expected[i]);
    }
    assert(heap_pop(&h, &v) == -1);
    heap_free(&h);

    /* ---- top-k: the 3 largest transactions out of 10000, using a MIN-heap of size k ----
     * Keep the k best seen so far; the root is the weakest of them and is replaced when a
     * bigger one arrives. O(n log k) time, O(k) memory: works on a stream. */
    struct heap top;
    heap_init(&top, sizeof(struct txn), cmp_txn_amount_asc);
    const size_t K = 3;
    long long best[3] = { -1, -1, -1 };     /* brute-force check, kept sorted descending */
    for (int i = 0; i < 10000; i++) {
        struct txn t = { .id = i, .amount = (long long)(next_rand() % 1000000) };
        if (top.len < K) {
            err = heap_push(&top, &t);
            assert(err == 0);
        } else if (t.amount > ((const struct txn *)heap_peek(&top))->amount) {
            struct txn dropped;
            heap_pop(&top, &dropped);
            err = heap_push(&top, &t);
            assert(err == 0);
        }
        for (int j = 0; j < 3; j++) {
            if (t.amount > best[j]) {
                for (int m = 2; m > j; m--)
                    best[m] = best[m - 1];
                best[j] = t.amount;
                break;
            }
        }
    }
    struct txn out;
    for (int j = 2; j >= 0; j--) {          /* min-heap pops smallest of the top 3 first */
        err = heap_pop(&top, &out);
        assert(err == 0 && out.amount == best[j]);
    }
    printf("top-3 amounts: %lld %lld %lld\n", best[0], best[1], best[2]);
    heap_free(&top);

    puts("all checks passed");
    return 0;
}
