/* m02_money_and_float.c - why money is never a double, and how to do money with integers.
 *
 * Module: 12_C_Programming/02_types_integers_and_conversions.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m02_money_and_float.c -o m02_money_and_float
 */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Money as a count of the smallest unit: 1 rupee = 100 paise. int64_t holds +-92 quadrillion
 * rupees, so range is never the problem; exactness is the point. */
typedef int64_t paise_t;

/* Format as "-1234.05". Works even for INT64_MIN because the magnitude is computed unsigned. */
static void format_paise(paise_t v, char *buf, size_t n)
{
    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    snprintf(buf, n, "%s%" PRIu64 ".%02" PRIu64, v < 0 ? "-" : "", mag / 100, mag % 100);
}

/* Simple interest for `days` at `rate_bp` basis points a year (1 bp = 0.01 %), ACT/365,
 * rounded half-up to the paisa. Every step is integer arithmetic with an overflow check. */
static bool simple_interest(paise_t principal, int64_t rate_bp, int64_t days, paise_t *out)
{
    if (principal < 0 || rate_bp < 0 || days < 0) return false;
    if (rate_bp != 0 && principal > INT64_MAX / rate_bp) return false;
    int64_t t = principal * rate_bp;
    if (days != 0 && t > INT64_MAX / days) return false;
    t *= days;
    const int64_t denom = 10000 * 365;
    if (t > INT64_MAX - denom / 2) return false;
    *out = (t + denom / 2) / denom;           /* add half the divisor, then truncate: half-up */
    return true;
}

int main(void)
{
    char s[32];

    /* 1. 0.1 and 0.2 have no exact binary representation. */
    double x = 0.1 + 0.2;
    printf("0.1 + 0.2 = %.17g, equal to 0.3? %s\n", x, x == 0.3 ? "yes" : "no");
    assert(x != 0.3);

    /* 2. Add 10 paise a million times: double drifts, integers do not. */
    double d = 0.0;
    paise_t p = 0;
    for (int i = 0; i < 1000000; ++i) {
        d += 0.10;
        p += 10;
    }
    format_paise(p, s, sizeof s);
    printf("one million x 0.10: double = %.10f, int64 paise = %s\n", d, s);
    assert(p == 10000000 && d != 100000.0);

    /* 3. printf rounds the BINARY value: 2.675 is really 2.67499999999999982236431605997495353221893310546875. */
    snprintf(s, sizeof s, "%.2f", 2.675);
    printf("printf(\"%%.2f\", 2.675) = %s (you expected 2.68)\n", s);
    assert(strcmp(s, "2.67") == 0);

    /* 4. Above 2^53 a double cannot hold every integer: big balances silently lose paise. */
    int64_t exact = (INT64_C(1) << 53) + 1;           /* 9007199254740993 paise */
    double as_double = (double)exact;
    printf("int64 %" PRId64 " -> double -> int64 %" PRId64 "\n", exact, (int64_t)as_double);
    assert((int64_t)as_double != exact);

    /* 5. Interest with integers: 10,00,000.00 rupees at 7.25 % for 30 days. */
    paise_t interest = 0;
    bool ok = simple_interest(100000000, 725, 30, &interest);
    assert(ok);
    format_paise(interest, s, sizeof s);
    printf("interest on 1000000.00 at 7.25%% for 30 days = %s\n", s);
    assert(interest == 595890);                        /* 5958.904109... rounds to 5958.90 */

    format_paise(INT64_MIN, s, sizeof s);
    printf("INT64_MIN paise formats as %s\n", s);
    return 0;
}
