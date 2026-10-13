/* m11_hash_table.c - an open-addressing hash table (linear probing, tombstones) with string
 * keys, written the way you would for a production C server.
 *
 *   - capacity is a power of two, so "hash % cap" becomes "hash & (cap - 1)"
 *   - the table stores the full 64-bit hash per slot: most mismatches are rejected without
 *     touching the key string (one less cache miss)
 *   - deletion leaves a TOMBSTONE so later probes keep going past the hole
 *   - load (live + tombstones) is kept below 70%; a rebuild drops all tombstones
 *   - the table owns copies of its keys
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m11_hash_table m11_hash_table.c
 * Run:    ./m11_hash_table
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

enum slot_state { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_TOMB = 2 };

struct slot {
    char         *key;
    long long     value;
    uint64_t      hash;
    unsigned char state;
};

struct htab {
    struct slot *slots;
    size_t       cap;      /* power of two */
    size_t       live;     /* FULL slots */
    size_t       tombs;    /* TOMB slots */
    size_t       rebuilds; /* statistics */
};

/* FNV-1a, 64-bit: simple, decent for short keys. Not DoS-resistant: a server facing
 * untrusted keys would use a keyed hash such as SipHash. */
static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static char *dup_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int ht_init(struct htab *t, size_t cap)
{
    assert(cap >= 8 && (cap & (cap - 1)) == 0);
    t->slots = calloc(cap, sizeof *t->slots);   /* calloc: all slots start EMPTY (0) */
    if (!t->slots)
        return ENOMEM;
    t->cap = cap;
    t->live = t->tombs = t->rebuilds = 0;
    return 0;
}

static void ht_free(struct htab *t)
{
    for (size_t i = 0; i < t->cap; i++)
        if (t->slots[i].state == SLOT_FULL)
            free(t->slots[i].key);
    free(t->slots);
    t->slots = NULL;
    t->cap = t->live = t->tombs = 0;
}

/* Find the slot holding key, or (for_insert) the slot where it should go: the first
 * tombstone seen on the probe path, else the empty slot that ended the search.
 * Terminates because the table always has at least one EMPTY slot (load < 70%). */
static struct slot *ht_probe(const struct htab *t, const char *key, uint64_t h, int for_insert)
{
    size_t mask = t->cap - 1;
    struct slot *first_tomb = NULL;
    for (size_t i = (size_t)h & mask;; i = (i + 1) & mask) {
        struct slot *s = &t->slots[i];
        if (s->state == SLOT_EMPTY)
            return for_insert ? (first_tomb ? first_tomb : s) : NULL;
        if (s->state == SLOT_TOMB) {
            if (!first_tomb)
                first_tomb = s;
        } else if (s->hash == h && strcmp(s->key, key) == 0) {
            return s;
        }
    }
}

/* Rebuild into new_cap slots. Keys are moved, not copied. Tombstones vanish. */
static int ht_rebuild(struct htab *t, size_t new_cap)
{
    struct htab n;
    int err = ht_init(&n, new_cap);
    if (err)
        return err;
    for (size_t i = 0; i < t->cap; i++) {
        struct slot *s = &t->slots[i];
        if (s->state != SLOT_FULL)
            continue;
        struct slot *d = ht_probe(&n, s->key, s->hash, 1);
        *d = *s;
        n.live++;
    }
    n.rebuilds = t->rebuilds + 1;
    free(t->slots);
    *t = n;
    return 0;
}

static int ht_put(struct htab *t, const char *key, long long value)
{
    /* Keep (live + tombs + 1) / cap <= 0.7. Grow only if live entries need it; otherwise
     * rebuild at the same size, which just clears tombstones. */
    if ((t->live + t->tombs + 1) * 10 > t->cap * 7) {
        size_t new_cap = (t->live + 1) * 10 > t->cap * 5 ? t->cap * 2 : t->cap;
        int err = ht_rebuild(t, new_cap);
        if (err)
            return err;
    }
    uint64_t h = fnv1a(key);
    struct slot *s = ht_probe(t, key, h, 1);
    if (s->state == SLOT_FULL) {
        s->value = value;            /* update in place */
        return 0;
    }
    char *copy = dup_string(key);
    if (!copy)
        return ENOMEM;
    if (s->state == SLOT_TOMB)
        t->tombs--;
    s->key = copy;
    s->value = value;
    s->hash = h;
    s->state = SLOT_FULL;
    t->live++;
    return 0;
}

static int ht_get(const struct htab *t, const char *key, long long *value)
{
    const struct slot *s = ht_probe(t, key, fnv1a(key), 0);
    if (!s)
        return ENOENT;
    *value = s->value;
    return 0;
}

static int ht_del(struct htab *t, const char *key)
{
    struct slot *s = ht_probe(t, key, fnv1a(key), 0);
    if (!s)
        return ENOENT;
    free(s->key);
    s->key = NULL;
    s->state = SLOT_TOMB;            /* NOT EMPTY: that would cut other keys' probe chains */
    t->live--;
    t->tombs++;
    return 0;
}

int main(void)
{
    struct htab t;
    int err = ht_init(&t, 8);
    assert(err == 0);

    char key[32];
    const int N = 10000;
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "ACC-%05d", i);
        err = ht_put(&t, key, (long long)i * 100);
        assert(err == 0);
    }
    assert(t.live == (size_t)N);
    printf("after %d inserts: cap=%zu live=%zu load=%.2f rebuilds=%zu\n",
           N, t.cap, t.live, (double)t.live / (double)t.cap, t.rebuilds);

    long long v;
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "ACC-%05d", i);
        err = ht_get(&t, key, &v);
        assert(err == 0 && v == (long long)i * 100);
    }
    err = ht_get(&t, "ACC-99999", &v);
    assert(err == ENOENT);

    /* delete the even keys, check the odd ones are still reachable past the tombstones */
    for (int i = 0; i < N; i += 2) {
        snprintf(key, sizeof key, "ACC-%05d", i);
        err = ht_del(&t, key);
        assert(err == 0);
    }
    assert(t.live == (size_t)N / 2 && t.tombs == (size_t)N / 2);
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof key, "ACC-%05d", i);
        err = ht_get(&t, key, &v);
        assert((i % 2 == 0) ? err == ENOENT : (err == 0 && v == (long long)i * 100));
    }

    /* update in place */
    err = ht_put(&t, "ACC-00001", -5);
    assert(err == 0);
    err = ht_get(&t, "ACC-00001", &v);
    assert(err == 0 && v == -5 && t.live == (size_t)N / 2);

    /* churn: insert and delete the same key many times. Without tombstone cleanup the
     * table would fill with tombstones and every miss would scan the whole table. */
    size_t cap_before = t.cap;
    for (int i = 0; i < 200000; i++) {
        snprintf(key, sizeof key, "TMP-%d", i);
        err = ht_put(&t, key, i);
        assert(err == 0);
        err = ht_del(&t, key);
        assert(err == 0);
    }
    assert(t.cap == cap_before);                /* churn did not grow the table */
    printf("after 200000 put/del pairs: cap=%zu live=%zu tombs=%zu rebuilds=%zu\n",
           t.cap, t.live, t.tombs, t.rebuilds);

    ht_free(&t);
    puts("all checks passed");
    return 0;
}
