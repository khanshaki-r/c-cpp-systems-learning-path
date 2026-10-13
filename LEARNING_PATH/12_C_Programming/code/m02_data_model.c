/* m02_data_model.c - how big is each type HERE, which data model is this, and how to print portably.
 *
 * Module: 12_C_Programming/02_types_integers_and_conversions.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m02_data_model.c -o m02_data_model
 *
 * On 64-bit Windows (MSVC or MinGW) the same program reports long = 4 and "LLP64".
 */
#include <assert.h>
#include <inttypes.h>   /* PRId64 and friends; also includes <stdint.h> */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SHOW(T) printf("  %-12s %2zu bytes\n", #T, sizeof(T))

int main(void)
{
    puts("sizes on this platform:");
    SHOW(char);
    SHOW(short);
    SHOW(int);
    SHOW(long);
    SHOW(long long);
    SHOW(void *);
    SHOW(size_t);
    SHOW(ptrdiff_t);
    SHOW(float);
    SHOW(double);
    SHOW(long double);

    const char *model =
        (sizeof(int) == 4 && sizeof(long) == 8 && sizeof(void *) == 8) ? "LP64 (64-bit Linux, macOS)" :
        (sizeof(int) == 4 && sizeof(long) == 4 && sizeof(void *) == 8) ? "LLP64 (64-bit Windows)" :
        (sizeof(int) == 4 && sizeof(long) == 4 && sizeof(void *) == 4) ? "ILP32 (32-bit)" : "other";
    printf("data model: %s\n", model);

    printf("CHAR_BIT = %d, plain char is %s\n", CHAR_BIT, CHAR_MIN < 0 ? "signed" : "unsigned");
    printf("INT_MAX  = %d\n", INT_MAX);
    printf("LONG_MAX = %ld\n", LONG_MAX);
    printf("SIZE_MAX = %zu\n", SIZE_MAX);

    /* Portable printing: the fixed-width types have their own macros, because int64_t is
     * "long" on Linux and "long long" on Windows, so neither %ld nor %lld is right everywhere. */
    int64_t   balance_paise = INT64_C(-123456789012);
    uint32_t  account_no    = UINT32_C(4000000000);
    int       arr[10];
    size_t    n             = sizeof arr / sizeof arr[0];
    ptrdiff_t gap           = &arr[7] - &arr[2];
    intmax_t  widest        = INTMAX_MAX;

    printf("int64_t   via PRId64: %" PRId64 "\n", balance_paise);
    printf("uint32_t  via PRIu32: %" PRIu32 "\n", account_no);
    printf("size_t    via %%zu  : %zu\n", n);
    printf("ptrdiff_t via %%td  : %td\n", gap);
    printf("intmax_t  via %%jd  : %jd\n", widest);

    /* What the standard guarantees, and nothing more. */
    assert(sizeof(char) == 1);                                   /* by definition */
    assert(sizeof(short) <= sizeof(int) && sizeof(int) <= sizeof(long));
    assert(sizeof(long) <= sizeof(long long));
    assert(sizeof(int64_t) == 8 && sizeof(int32_t) == 4);        /* exact-width types are exact */
    assert(n == 10 && gap == 5);
    return 0;
}
