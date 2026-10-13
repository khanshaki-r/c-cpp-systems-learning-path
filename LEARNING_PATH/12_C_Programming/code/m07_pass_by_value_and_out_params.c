/* m07_pass_by_value_and_out_params.c - C passes everything by value; out-parameters and status codes.
 *
 * Module: 12_C_Programming/07_functions_function_pointers_and_callbacks.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m07_pass_by_value_and_out_params.c -o m07_pass_by_value_and_out_params
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void try_to_change(int x)
{
    x = 99;                       /* changes the function's own copy only */
    (void)x;
}

static void change(int *x) { *x = 99; }        /* the caller passed an address: this sticks */

struct big {
    int v[1000];
};

static long sum_by_value(struct big b)          /* copies 4000 bytes onto the stack per call */
{
    long s = 0;
    for (int i = 0; i < 1000; ++i) s += b.v[i];
    return s;
}

static long sum_by_pointer(const struct big *b) /* copies 8 bytes; const says "read only" */
{
    long s = 0;
    for (int i = 0; i < 1000; ++i) s += b->v[i];
    return s;
}

/* Small structs returned by value are fine and clear: multiple results, no out-params. */
struct split {
    int64_t rupees;
    int64_t paise;
};

static struct split split_amount(int64_t total_paise)
{
    return (struct split){.rupees = total_paise / 100, .paise = total_paise % 100};
}

/* The dominant C API shape: return a STATUS, deliver results through OUT-PARAMETERS.
 * Contract: *new_balance is written only when the result is ST_OK. */
typedef enum {
    ST_OK = 0,
    ST_INVALID_ARG,
    ST_INSUFFICIENT_FUNDS,
} status;

struct account {
    int64_t balance_paise;
};

static status withdraw(struct account *acct, int64_t paise, int64_t *new_balance)
{
    if (acct == NULL || new_balance == NULL || paise <= 0) return ST_INVALID_ARG;
    if (paise > acct->balance_paise) return ST_INSUFFICIENT_FUNDS;
    acct->balance_paise -= paise;
    *new_balance = acct->balance_paise;
    return ST_OK;
}

int main(void)
{
    int n = 1;
    try_to_change(n);
    assert(n == 1);
    change(&n);
    assert(n == 99);
    printf("after try_to_change: 1, after change(&n): %d\n", n);

    /* static: 4000 bytes is too big for some threads' stacks if it were a local in a deep call chain */
    static struct big b;
    for (int i = 0; i < 1000; ++i) b.v[i] = i;
    assert(sum_by_value(b) == sum_by_pointer(&b));
    printf("sum_by_value == sum_by_pointer == %ld (sizeof(struct big) = %zu bytes copied by value)\n",
           sum_by_pointer(&b), sizeof b);

    struct split s = split_amount(123456);
    printf("split_amount(123456) -> %" PRId64 " rupees %" PRId64 " paise\n", s.rupees, s.paise);
    assert(s.rupees == 1234 && s.paise == 56);

    /* div() from <stdlib.h> is the standard library doing the same thing. */
    div_t d = div(17, 5);
    assert(d.quot == 3 && d.rem == 2);

    struct account acct = {.balance_paise = 50000};
    int64_t bal = -1;
    status st = withdraw(&acct, 20000, &bal);
    printf("withdraw 200.00 -> status %d, new balance %" PRId64 "\n", (int)st, bal);
    assert(st == ST_OK && bal == 30000);
    bal = -1;
    st = withdraw(&acct, 99999, &bal);
    printf("withdraw 999.99 -> status %d (insufficient funds), out-param untouched: %" PRId64 "\n", (int)st, bal);
    assert(st == ST_INSUFFICIENT_FUNDS && bal == -1);
    return 0;
}
