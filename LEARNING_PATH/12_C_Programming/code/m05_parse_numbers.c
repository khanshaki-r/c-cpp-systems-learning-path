/* m05_parse_numbers.c - why atoi is dangerous, how strtol must be checked, and sscanf pitfalls.
 *
 * Module: 12_C_Programming/05_strings_and_buffers.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m05_parse_numbers.c -o m05_parse_numbers
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PARSE_OK = 0,
    PARSE_EMPTY,
    PARSE_NOT_A_NUMBER,
    PARSE_TRAILING_GARBAGE,
    PARSE_OUT_OF_RANGE,
} parse_status;

static const char *parse_status_name(parse_status s)
{
    switch (s) {
    case PARSE_OK:               return "ok";
    case PARSE_EMPTY:            return "empty";
    case PARSE_NOT_A_NUMBER:     return "not a number";
    case PARSE_TRAILING_GARBAGE: return "trailing garbage";
    case PARSE_OUT_OF_RANGE:     return "out of range";
    }
    return "?";
}

/* Parse a whole string as a base-10 int64. *out is written only on PARSE_OK. */
static parse_status parse_i64(const char *s, int64_t *out)
{
    if (s == NULL || *s == '\0') return PARSE_EMPTY;
    char *end;
    errno = 0;                                   /* strtoll SETS errno on error, never clears it */
    long long v = strtoll(s, &end, 10);
    if (end == s) return PARSE_NOT_A_NUMBER;     /* not a single digit was consumed */
    if (errno == ERANGE) return PARSE_OUT_OF_RANGE;   /* v is LLONG_MAX or LLONG_MIN */
    while (isspace((unsigned char)*end)) ++end;  /* allow "42\n" from fgets */
    if (*end != '\0') return PARSE_TRAILING_GARBAGE;
    *out = (int64_t)v;
    return PARSE_OK;
}

/* Unsigned 32-bit with a range check. strtoul ACCEPTS a minus sign and negates the result,
 * so "-1" quietly becomes ULONG_MAX. Reject the sign yourself. */
static parse_status parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return PARSE_EMPTY;
    const char *q = s;
    while (isspace((unsigned char)*q)) ++q;
    if (*q == '-') return PARSE_OUT_OF_RANGE;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return PARSE_NOT_A_NUMBER;
    if (errno == ERANGE || v > UINT32_MAX) return PARSE_OUT_OF_RANGE;
    if (*end != '\0') return PARSE_TRAILING_GARBAGE;
    *out = (uint32_t)v;
    return PARSE_OK;
}

int main(void)
{
    /* atoi cannot report errors: "12abc" -> 12, "abc" -> 0 (is that a zero amount?), and an
     * out-of-range input is undefined behaviour. Never use it on input from outside. */
    printf("atoi(\"12abc\") = %d, atoi(\"abc\") = %d, atoi(\"1O0\") = %d   (letter O)\n",
           atoi("12abc"), atoi("abc"), atoi("1O0"));

    const char *inputs[] = {"42", "  -17\n", "", "abc", "12abc", "99999999999999999999", "1O0"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
        int64_t v = 0;
        parse_status st = parse_i64(inputs[i], &v);
        char shown[32];
        snprintf(shown, sizeof shown, "\"%s\"", inputs[i]);
        if (shown[strlen(shown) - 2] == '\n') memcpy(shown + strlen(shown) - 2, "\\n\"", 4);
        if (st == PARSE_OK) printf("parse_i64(%-24s -> %" PRId64 "\n", shown, v);
        else printf("parse_i64(%-24s -> error: %s\n", shown, parse_status_name(st));
    }

    unsigned long raw = strtoul("-1", NULL, 10);
    uint32_t u = 0;
    printf("strtoul(\"-1\") = %lu with no error; parse_u32(\"-1\") -> %s\n", raw,
           parse_status_name(parse_u32("-1", &u)));
    assert(raw == ULONG_MAX);
    assert(parse_u32("4294967295", &u) == PARSE_OK && u == UINT32_MAX);
    assert(parse_u32("4294967296", &u) == PARSE_OUT_OF_RANGE);

    /* sscanf pitfall 1: it returns how many conversions SUCCEEDED. Always check it. */
    int a = -1, b = -1;
    int got = sscanf("12 abc", "%d %d", &a, &b);
    printf("sscanf(\"12 abc\", \"%%d %%d\") returned %d; b is still %d\n", got, b);
    assert(got == 1 && a == 12 && b == -1);

    /* Pitfall 2: %s with no width is gets() again. Always give a width of size - 1. */
    char name[8];
    got = sscanf("SBIN0001234", "%7s", name);
    printf("sscanf(\"SBIN0001234\", \"%%7s\") -> \"%s\" (bounded)\n", name);
    assert(got == 1 && strcmp(name, "SBIN000") == 0);

    /* Pitfall 3: trailing garbage is silently ignored. %n reports how far it got. */
    int value = 0, used = 0;
    got = sscanf("250xyz", "%d%n", &value, &used);
    printf("sscanf(\"250xyz\", \"%%d%%n\") -> value %d, stopped at offset %d (\"%s\" left over)\n",
           value, used, "250xyz" + used);
    assert(got == 1 && used == 3);
    /* Pitfall 4: %d on a number that does not fit is undefined behaviour. Use strtol for that. */
    return 0;
}
