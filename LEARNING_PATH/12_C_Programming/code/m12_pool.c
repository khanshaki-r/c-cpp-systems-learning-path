/* m12_pool.c - a fixed-size object pool with an embedded free list.
 *
 * One malloc up front for N equal slots. A free slot stores the pointer to the next free slot
 * in its own first bytes, so the free list costs no extra memory. alloc and free are O(1)
 * pointer pops and pushes, there is no per-object header, and there is no fragmentation
 * because every slot is the same size.
 *
 * The debug checks (range, slot boundary, double free) use a one-byte-per-slot state map.
 * A release build could drop the map; this one keeps it because the checks are cheap.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m12_pool m12_pool.c
 * Run:    ./m12_pool
 */
#include <assert.h>
#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

struct pool {
    unsigned char *mem;        /* nslots * slot_size bytes, one allocation */
    unsigned char *state;      /* 0 = free, 1 = in use: for the debug checks */
    void          *free_list;  /* first free slot, or NULL */
    size_t         slot_size;
    size_t         nslots;
    size_t         in_use;
};

enum { POOL_OK = 0, POOL_FOREIGN = 1, POOL_MISALIGNED = 2, POOL_DOUBLE_FREE = 3 };

static size_t round_up(size_t x, size_t a) { return (x + a - 1) / a * a; }

static int pool_init(struct pool *p, size_t obj_size, size_t nslots)
{
    /* A slot must hold the free-list pointer and keep every object suitably aligned. */
    size_t sz = obj_size < sizeof(void *) ? sizeof(void *) : obj_size;
    sz = round_up(sz, alignof(max_align_t));
    if (nslots == 0 || nslots > SIZE_MAX / sz)
        return EINVAL;
    p->mem = malloc(nslots * sz);
    p->state = calloc(nslots, 1);
    if (!p->mem || !p->state) {
        free(p->mem);
        free(p->state);
        return ENOMEM;
    }
    p->slot_size = sz;
    p->nslots = nslots;
    p->in_use = 0;

    /* Thread the free list through the slots, lowest address first. */
    p->free_list = NULL;
    for (size_t i = nslots; i-- > 0;) {
        void *slot = p->mem + i * sz;
        memcpy(slot, &p->free_list, sizeof(void *));   /* slot->next = free_list */
        p->free_list = slot;
    }
    return 0;
}

static void pool_destroy(struct pool *p)
{
    free(p->mem);
    free(p->state);
    memset(p, 0, sizeof *p);
}

static void *pool_alloc(struct pool *p)
{
    void *slot = p->free_list;
    if (!slot)
        return NULL;                                   /* exhausted: caller decides */
    memcpy(&p->free_list, slot, sizeof(void *));       /* free_list = slot->next */
    p->state[((unsigned char *)slot - p->mem) / p->slot_size] = 1;
    p->in_use++;
    return slot;
}

static int pool_free(struct pool *p, void *obj)
{
    uintptr_t base = (uintptr_t)p->mem;
    uintptr_t addr = (uintptr_t)obj;
    if (addr < base || addr >= base + p->nslots * p->slot_size)
        return POOL_FOREIGN;                           /* not ours: maybe malloc'd */
    size_t off = (size_t)(addr - base);
    if (off % p->slot_size != 0)
        return POOL_MISALIGNED;                        /* interior pointer */
    size_t idx = off / p->slot_size;
    if (p->state[idx] == 0)
        return POOL_DOUBLE_FREE;
    p->state[idx] = 0;
    memcpy(obj, &p->free_list, sizeof(void *));        /* push front: LIFO */
    p->free_list = obj;
    p->in_use--;
    return POOL_OK;
}

/* A typical pooled object: a connection or request record in a server. */
struct request {
    int       conn_id;
    int       opcode;
    long long txn_id;
    char      account[24];
};

int main(void)
{
    struct pool p;
    enum { N = 64 };
    int err = pool_init(&p, sizeof(struct request), N);
    assert(err == 0);
    printf("sizeof(struct request)=%zu, slot_size=%zu, slots=%d\n",
           sizeof(struct request), p.slot_size, N);

    /* exhaust the pool */
    struct request *r[N];
    for (int i = 0; i < N; i++) {
        r[i] = pool_alloc(&p);
        assert(r[i] != NULL);
        assert((uintptr_t)r[i] % alignof(max_align_t) == 0);
        r[i]->conn_id = i;
        snprintf(r[i]->account, sizeof r[i]->account, "ACC-%04d", i);
    }
    assert(pool_alloc(&p) == NULL);
    assert(p.in_use == N);

    /* objects do not overlap: every one still holds what we wrote */
    for (int i = 0; i < N; i++)
        assert(r[i]->conn_id == i);

    /* LIFO reuse: the slot freed last comes back first (still warm in cache) */
    err = pool_free(&p, r[10]);
    assert(err == POOL_OK);
    err = pool_free(&p, r[20]);
    assert(err == POOL_OK);
    void *again = pool_alloc(&p);
    assert(again == r[20]);
    again = pool_alloc(&p);
    assert(again == r[10]);

    /* the debug checks */
    err = pool_free(&p, r[5]);
    assert(err == POOL_OK);
    err = pool_free(&p, r[5]);
    assert(err == POOL_DOUBLE_FREE);
    int on_stack;
    err = pool_free(&p, &on_stack);
    assert(err == POOL_FOREIGN);
    err = pool_free(&p, (unsigned char *)r[6] + 8);
    assert(err == POOL_MISALIGNED);
    printf("double free -> %d, foreign pointer -> %d, interior pointer -> %d (all rejected)\n",
           POOL_DOUBLE_FREE, POOL_FOREIGN, POOL_MISALIGNED);

    /* churn: a million alloc/free pairs, no malloc involved */
    for (int i = 0; i < 1000000; i++) {
        struct request *x = pool_alloc(&p);
        assert(x != NULL);
        x->txn_id = i;
        err = pool_free(&p, x);
        assert(err == POOL_OK);
    }
    assert(p.in_use == N - 1);

    pool_destroy(&p);
    puts("all checks passed");
    return 0;
}
