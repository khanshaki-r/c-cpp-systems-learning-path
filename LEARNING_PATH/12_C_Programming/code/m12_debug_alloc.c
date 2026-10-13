/* m12_debug_alloc.c - a debug allocator layered on malloc: guard bytes, fill patterns,
 * a header canary, a quarantine for freed blocks, and a leak report with file:line.
 *
 * It catches, at free time or on demand:
 *   - writes past the end (overflow) or before the start (underflow) of a block
 *   - double free (the block is still in quarantine, so its header is readable)
 *   - writes after free (the freed bytes no longer hold the 0xDD pattern at eviction)
 *   - leaks (blocks still live at exit, with the line that allocated them)
 * and makes uninitialised reads obvious: new memory is 0xCD, not "whatever was there".
 *
 * The byte values copy the MSVC debug CRT, so they will look familiar in a Windows dump:
 *   0xCD "clean" (allocated, never written)  0xDD "dead" (freed)  0xFD "no man's land" (guards)
 *
 * Block layout (every part a multiple of 16 so the user pointer stays 16-byte aligned):
 *   [ struct dbg_hdr, padded to 48 ][ front guard 16 x 0xFD ][ user bytes ][ back guard 16 x 0xFD ]
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m12_debug_alloc m12_debug_alloc.c
 * Run:    ./m12_debug_alloc
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

#define GUARD      16u
#define FILL_NEW   0xCD
#define FILL_DEAD  0xDD
#define FILL_GUARD 0xFD
#define MAGIC_LIVE 0xA110CA7Eu
#define MAGIC_DEAD 0xDEADB10Cu
#define QUARANTINE 8u

struct dbg_hdr {
    uint32_t        magic;
    uint32_t        line;
    size_t          size;
    const char     *file;
    struct dbg_hdr *prev, *next;      /* doubly linked list of live blocks */
};

#define HDR_SPACE ((sizeof(struct dbg_hdr) + 15u) & ~(size_t)15u)

enum { DBG_OK = 0, DBG_OVERFLOW = 1, DBG_UNDERFLOW = 2, DBG_DOUBLE_FREE = 4, DBG_BAD_PTR = 8 };

static struct dbg_hdr *g_live;
static struct dbg_hdr *g_quarantine[QUARANTINE];
static size_t          g_q_next;
static size_t          g_write_after_free;

static unsigned char *user_of(struct dbg_hdr *h) { return (unsigned char *)h + HDR_SPACE + GUARD; }
static struct dbg_hdr *hdr_of(void *p) { return (struct dbg_hdr *)(void *)((unsigned char *)p - GUARD - HDR_SPACE); }

static int all_bytes(const unsigned char *p, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

static int check_guards(struct dbg_hdr *h)
{
    unsigned char *u = user_of(h);
    int r = DBG_OK;
    if (!all_bytes(u - GUARD, GUARD, FILL_GUARD)) r |= DBG_UNDERFLOW;
    if (!all_bytes(u + h->size, GUARD, FILL_GUARD)) r |= DBG_OVERFLOW;
    return r;
}

static void *dbg_malloc(size_t size, const char *file, int line)
{
    if (size > SIZE_MAX - HDR_SPACE - 2 * GUARD)
        return NULL;
    struct dbg_hdr *h = malloc(HDR_SPACE + GUARD + size + GUARD);
    if (!h)
        return NULL;
    h->magic = MAGIC_LIVE;
    h->file = file;
    h->line = (uint32_t)line;
    h->size = size;
    unsigned char *u = user_of(h);
    memset(u - GUARD, FILL_GUARD, GUARD);
    memset(u, FILL_NEW, size);
    memset(u + size, FILL_GUARD, GUARD);
    h->prev = NULL;                     /* push on the live list */
    h->next = g_live;
    if (g_live)
        g_live->prev = h;
    g_live = h;
    return u;
}

/* Really release a quarantined block, first checking nobody wrote to it after free. */
static void evict(struct dbg_hdr *h)
{
    if (!all_bytes(user_of(h), h->size, FILL_DEAD)) {
        g_write_after_free++;
        printf("dbg: WRITE AFTER FREE into %zu-byte block from %s:%u\n", h->size, h->file, h->line);
    }
    free(h);
}

static int dbg_free(void *p, const char *file, int line)
{
    if (!p)
        return DBG_OK;
    struct dbg_hdr *h = hdr_of(p);
    if (h->magic == MAGIC_DEAD) {
        printf("dbg: DOUBLE FREE at %s:%d of block allocated at %s:%u\n", file, line, h->file, h->line);
        return DBG_DOUBLE_FREE;
    }
    if (h->magic != MAGIC_LIVE) {
        printf("dbg: free of a pointer we never returned, at %s:%d\n", file, line);
        return DBG_BAD_PTR;
    }
    int r = check_guards(h);
    if (r & DBG_OVERFLOW)
        printf("dbg: OVERFLOW past %zu-byte block from %s:%u, detected at free %s:%d\n",
               h->size, h->file, h->line, file, line);
    if (r & DBG_UNDERFLOW)
        printf("dbg: UNDERFLOW before %zu-byte block from %s:%u, detected at free %s:%d\n",
               h->size, h->file, h->line, file, line);

    if (h->prev) h->prev->next = h->next; else g_live = h->next;   /* unlink */
    if (h->next) h->next->prev = h->prev;

    h->magic = MAGIC_DEAD;
    memset(p, FILL_DEAD, h->size);
    if (g_quarantine[g_q_next])               /* quarantine: delay the real free */
        evict(g_quarantine[g_q_next]);
    g_quarantine[g_q_next] = h;
    g_q_next = (g_q_next + 1) % QUARANTINE;
    return r;
}

static size_t dbg_flush_quarantine(void)
{
    size_t before = g_write_after_free;
    for (size_t i = 0; i < QUARANTINE; i++) {
        if (g_quarantine[i]) {
            evict(g_quarantine[i]);
            g_quarantine[i] = NULL;
        }
    }
    return g_write_after_free - before;
}

/* Walk every live block and verify its guards: call it from a timer or a debug command
 * to find corruption close to when it happened, not at some distant free(). */
static size_t dbg_check_all(void)
{
    size_t bad = 0;
    for (struct dbg_hdr *h = g_live; h; h = h->next)
        if (check_guards(h) != DBG_OK)
            bad++;
    return bad;
}

static size_t dbg_leak_report(void)
{
    size_t n = 0;
    for (struct dbg_hdr *h = g_live; h; h = h->next, n++)
        printf("dbg: LEAK %zu bytes allocated at %s:%u\n", h->size, h->file, h->line);
    return n;
}

#define DMALLOC(n) dbg_malloc((n), __FILE__, __LINE__)
#define DFREE(p)   dbg_free((p), __FILE__, __LINE__)

int main(void)
{
    int r;

    /* 1. normal use, and the fill pattern that exposes uninitialised reads */
    unsigned char *p = DMALLOC(24);
    assert(p != NULL && (uintptr_t)p % 16 == 0);
    assert(p[0] == FILL_NEW && p[23] == FILL_NEW);
    int as_int;
    memcpy(&as_int, p, sizeof as_int);
    printf("uninitialised int read from a fresh block: 0x%08X\n", (unsigned)as_int);
    r = DFREE(p);
    assert(r == DBG_OK);

    /* 2. off-by-one overflow: p[n] lands in OUR guard, inside our malloc block, so the test
     *    itself stays defined behaviour */
    char *s = DMALLOC(8);
    memcpy(s, "ACC-1001", 9);            /* 8 chars + NUL = 9 bytes into an 8-byte block */
    assert(dbg_check_all() == 1);        /* found before free, by the periodic checker */
    r = DFREE(s);
    assert(r == DBG_OVERFLOW);

    /* 3. underflow */
    char *u = DMALLOC(16);
    u[-1] = 'X';
    r = DFREE(u);
    assert(r == DBG_UNDERFLOW);

    /* 4. double free: the block is in quarantine, so its header can still be read */
    char *d = DMALLOC(32);
    r = DFREE(d);
    assert(r == DBG_OK);
    r = DFREE(d);
    assert(r == DBG_DOUBLE_FREE);

    /* 5. write after free: caught when the block leaves quarantine */
    char *w = DMALLOC(40);
    r = DFREE(w);
    assert(r == DBG_OK);
    w[5] = 'Z';                          /* still our memory (quarantined), so no real UB */
    size_t waf = dbg_flush_quarantine();
    assert(waf == 1);

    /* 6. leaks */
    void *leak1 = DMALLOC(100);
    void *leak2 = DMALLOC(7);
    assert(leak1 && leak2);
    size_t leaks = dbg_leak_report();
    assert(leaks == 2);

    r = DFREE(leak1);
    assert(r == DBG_OK);
    r = DFREE(leak2);
    assert(r == DBG_OK);
    dbg_flush_quarantine();
    assert(dbg_leak_report() == 0);
    puts("all checks passed");
    return 0;
}
