/* m07_qsort_bsearch_callbacks.c - comparators done right, bsearch, and callbacks with a void * context.
 *
 * Module: 12_C_Programming/07_functions_function_pointers_and_callbacks.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m07_qsort_bsearch_callbacks.c -o m07_qsort_bsearch_callbacks
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct txn {
    uint32_t id;
    int64_t  amount_paise;
};

/* WRONG: subtraction. The int64 difference does not fit in the int return value, so the sign
 * can flip (and with int fields, the subtraction itself can overflow: undefined behaviour). */
static int cmp_amount_bad(const void *a, const void *b)
{
    const struct txn *x = a, *y = b;
    return (int)(x->amount_paise - y->amount_paise);
}

/* RIGHT: two comparisons, result is always -1, 0 or 1. */
static int cmp_amount(const void *a, const void *b)
{
    const struct txn *x = a, *y = b;
    return (x->amount_paise > y->amount_paise) - (x->amount_paise < y->amount_paise);
}

static int cmp_id(const void *a, const void *b)
{
    const struct txn *x = a, *y = b;
    return (x->id > y->id) - (x->id < y->id);
}

/* bsearch passes the KEY first and an ELEMENT second; they need not have the same type. */
static int cmp_key_id(const void *key, const void *elem)
{
    uint32_t k = *(const uint32_t *)key;
    const struct txn *t = elem;
    return (k > t->id) - (k < t->id);
}

/* Sorting an array of char *: the comparator receives POINTERS TO the elements, i.e. char **.
 * Passing them straight to strcmp compares the bytes of the pointers: a classic bug. */
static int cmp_str(const void *a, const void *b)
{
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}

/* A visitor with a context pointer: the callback gets state without globals. */
typedef void (*txn_visitor)(const struct txn *t, void *ctx);

static void for_each_txn(const struct txn *arr, size_t n, txn_visitor fn, void *ctx)
{
    for (size_t i = 0; i < n; ++i) fn(&arr[i], ctx);
}

struct totals {
    int64_t debit_paise;
    int64_t credit_paise;
    int     count_over_limit;
    int64_t limit_paise;
};

static void accumulate(const struct txn *t, void *ctx)
{
    struct totals *tot = ctx;    /* the caller promised ctx is a struct totals * */
    if (t->amount_paise < 0) tot->debit_paise += t->amount_paise;
    else tot->credit_paise += t->amount_paise;
    if (t->amount_paise > tot->limit_paise || -t->amount_paise > tot->limit_paise) tot->count_over_limit++;
}

int main(void)
{
    struct txn big = {1, 3000000000}, zero = {2, 0};
    printf("cmp_amount_bad(30000000.00, 0) = %d  (negative: claims big < zero)\n", cmp_amount_bad(&big, &zero));
    printf("cmp_amount    (30000000.00, 0) = %d\n", cmp_amount(&big, &zero));
    assert(cmp_amount_bad(&big, &zero) < 0 && cmp_amount(&big, &zero) > 0);

    struct txn book[] = {
        {104, 500000}, {101, -120000}, {107, 3000000000}, {102, 0}, {105, -9900},
    };
    size_t n = sizeof book / sizeof book[0];

    qsort(book, n, sizeof book[0], cmp_amount);
    printf("by amount:");
    for (size_t i = 0; i < n; ++i) printf(" %" PRId64, book[i].amount_paise);
    putchar('\n');
    for (size_t i = 1; i < n; ++i) assert(book[i - 1].amount_paise <= book[i].amount_paise);

    qsort(book, n, sizeof book[0], cmp_id);       /* bsearch needs the array sorted by ITS key */
    uint32_t key = 105;
    const struct txn *hit = bsearch(&key, book, n, sizeof book[0], cmp_key_id);
    assert(hit != NULL && hit->amount_paise == -9900);
    printf("bsearch id 105 -> amount %" PRId64 "\n", hit->amount_paise);
    key = 106;
    assert(bsearch(&key, book, n, sizeof book[0], cmp_key_id) == NULL);

    const char *codes[] = {"RTGS", "IMPS", "NEFT", "UPI"};
    qsort(codes, 4, sizeof codes[0], cmp_str);
    printf("sorted codes: %s %s %s %s\n", codes[0], codes[1], codes[2], codes[3]);
    assert(strcmp(codes[0], "IMPS") == 0 && strcmp(codes[3], "UPI") == 0);

    struct totals tot = {.limit_paise = 100000};
    for_each_txn(book, n, accumulate, &tot);
    printf("debits %" PRId64 ", credits %" PRId64 ", %d over the 1000.00 limit\n", tot.debit_paise,
           tot.credit_paise, tot.count_over_limit);
    assert(tot.debit_paise == -129900 && tot.count_over_limit == 3);
    return 0;
}
