/* m11_vec.c - a growable array in C, written two ways:
 *
 *   1. struct vec: generic through void* + element size, the qsort() style. One copy of the
 *      code serves every element type; the price is no type checking and memcpy per element.
 *   2. DEFINE_VEC(T, name): a macro that stamps out a typed vector per element type, the
 *      "poor man's template". Type-checked and fast; the price is code size and harder debugging.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m11_vec m11_vec.c
 * Run:    ./m11_vec
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

/* ======================= 1. generic vector ======================= */
struct vec {
    unsigned char *data;      /* unsigned char*: byte arithmetic is legal, void* arithmetic is not */
    size_t         len;
    size_t         cap;
    size_t         elem_size;
    size_t         reallocs;  /* statistics for the demo */
};

static void vec_init(struct vec *v, size_t elem_size)
{
    v->data = NULL;
    v->len = v->cap = 0;
    v->elem_size = elem_size;
    v->reallocs = 0;
}

static void vec_free(struct vec *v)
{
    free(v->data);
    vec_init(v, v->elem_size);
}

/* Grow capacity to at least want. Doubling gives amortised O(1) push. Every multiplication
 * is overflow-checked: cap * elem_size wrapping to a small number would be a heap overflow. */
static int vec_reserve(struct vec *v, size_t want)
{
    if (want <= v->cap)
        return 0;
    size_t ncap = v->cap ? v->cap : 8;
    while (ncap < want) {
        if (ncap > SIZE_MAX / 2)
            return ENOMEM;
        ncap *= 2;
    }
    if (ncap > SIZE_MAX / v->elem_size)
        return ENOMEM;
    void *p = realloc(v->data, ncap * v->elem_size);
    if (!p)
        return ENOMEM;        /* v->data is still valid: nothing lost, nothing dangling */
    v->data = p;
    v->cap = ncap;
    v->reallocs++;
    return 0;
}

static int vec_push(struct vec *v, const void *elem)
{
    if (v->len == v->cap) {
        int err = vec_reserve(v, v->len + 1);
        if (err)
            return err;
    }
    memcpy(v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
    return 0;
}

/* Returns a pointer INTO the vector. It dangles after the next push that reallocates -
 * exactly like a std::vector iterator. Copy the element out if you need to keep it. */
static void *vec_at(const struct vec *v, size_t i)
{
    assert(i < v->len);
    return v->data + i * v->elem_size;
}

static int vec_pop(struct vec *v, void *out)
{
    if (v->len == 0)
        return -1;
    v->len--;
    if (out)
        memcpy(out, v->data + v->len * v->elem_size, v->elem_size);
    return 0;
}

/* Order-preserving removal: memmove because source and destination overlap. */
static void vec_remove_at(struct vec *v, size_t i)
{
    assert(i < v->len);
    unsigned char *dst = v->data + i * v->elem_size;
    memmove(dst, dst + v->elem_size, (v->len - i - 1) * v->elem_size);
    v->len--;
}

static void vec_sort(struct vec *v, int (*cmp)(const void *, const void *))
{
    if (v->len > 1)
        qsort(v->data, v->len, v->elem_size, cmp);
}

/* ---- comparators: the classic bug is "return a - b", which overflows ---- */
static int cmp_int(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    return (a > b) - (a < b);
}

struct txn {
    long long id;
    long long amount_paise;
};

static int cmp_txn_by_amount_desc(const void *pa, const void *pb)
{
    const struct txn *a = pa, *b = pb;
    if (a->amount_paise != b->amount_paise)
        return (a->amount_paise < b->amount_paise) - (a->amount_paise > b->amount_paise);
    return (a->id > b->id) - (a->id < b->id);      /* tie-break: deterministic order */
}

/* ======================= 2. type-safe vector by macro ======================= */
#define DEFINE_VEC(T, name)                                                     \
    struct name {                                                               \
        T     *data;                                                            \
        size_t len;                                                             \
        size_t cap;                                                             \
    };                                                                          \
    static inline int name##_push(struct name *v, T x)                          \
    {                                                                           \
        if (v->len == v->cap) {                                                 \
            size_t ncap = v->cap ? v->cap * 2 : 8;                              \
            if (ncap > SIZE_MAX / sizeof(T))                                    \
                return ENOMEM;                                                  \
            T *p = realloc(v->data, ncap * sizeof(T));                          \
            if (!p)                                                             \
                return ENOMEM;                                                  \
            v->data = p;                                                        \
            v->cap = ncap;                                                      \
        }                                                                       \
        v->data[v->len++] = x;                                                  \
        return 0;                                                               \
    }                                                                           \
    static inline void name##_free(struct name *v)                              \
    {                                                                           \
        free(v->data);                                                          \
        v->data = NULL;                                                         \
        v->len = v->cap = 0;                                                    \
    }

/* No semicolon after these: an extra ';' at file scope is a -Wpedantic error. */
DEFINE_VEC(double, dvec)
DEFINE_VEC(struct txn, txnvec)

int main(void)
{
    /* generic vector of int */
    struct vec v;
    vec_init(&v, sizeof(int));
    for (int i = 0; i < 1000; i++) {
        int x = (i * 7919) % 1000;     /* a permutation of 0..999 */
        int err = vec_push(&v, &x);
        assert(err == 0);
    }
    assert(v.len == 1000 && v.cap == 1024);
    printf("1000 pushes: capacity %zu, %zu reallocations\n", v.cap, v.reallocs);

    vec_sort(&v, cmp_int);
    for (size_t i = 0; i < v.len; i++)
        assert(*(int *)vec_at(&v, i) == (int)i);

    int key = 777;
    int *found = bsearch(&key, v.data, v.len, v.elem_size, cmp_int);
    assert(found && *found == 777);

    vec_remove_at(&v, 0);
    assert(v.len == 999 && *(int *)vec_at(&v, 0) == 1);
    int last;
    int err = vec_pop(&v, &last);
    assert(err == 0 && last == 999 && v.len == 998);
    vec_free(&v);

    /* generic vector of structs, sorted with a multi-key comparator */
    struct vec tv;
    vec_init(&tv, sizeof(struct txn));
    struct txn sample[] = { { 1, 500 }, { 2, 2500 }, { 3, 500 }, { 4, -100 }, { 5, 2500 } };
    for (size_t i = 0; i < sizeof sample / sizeof sample[0]; i++) {
        err = vec_push(&tv, &sample[i]);
        assert(err == 0);
    }
    vec_sort(&tv, cmp_txn_by_amount_desc);
    long long expect_ids[] = { 2, 5, 1, 3, 4 };
    for (size_t i = 0; i < tv.len; i++)
        assert(((struct txn *)vec_at(&tv, i))->id == expect_ids[i]);
    vec_free(&tv);

    /* comparator overflow: "a - b" gives the wrong sign for these two values */
    int big = 2000000000, neg = -2000000000;
    assert(cmp_int(&big, &neg) > 0);
    long long wrong = (long long)big - neg;           /* 4e9 does not fit in an int ... */
    assert(wrong > 2147483647LL);                     /* ... so int "a - b" would have wrapped */

    /* typed vectors from the macro */
    struct dvec dv = { 0 };
    for (int i = 0; i < 100; i++) {
        err = dvec_push(&dv, i * 0.5);
        assert(err == 0);
    }
    assert(dv.len == 100 && dv.data[99] == 49.5);
    dvec_free(&dv);

    struct txnvec xv = { 0 };
    err = txnvec_push(&xv, (struct txn){ .id = 9, .amount_paise = 123 });   /* compound literal */
    assert(err == 0 && xv.data[0].id == 9);
    txnvec_free(&xv);

    puts("all checks passed");
    return 0;
}
