/* m16_string_questions.c - the classic C string and memory interview questions, each with
 * a correct solution and tests against the C library.
 *
 *   my_strlen, my_strcmp, my_strcpy, my_strlcpy (safe copy), my_memcpy, my_memmove,
 *   my_atoi (with overflow detection), reverse_in_place, my_strtok_r, next_field
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m16_string_questions m16_string_questions.c
 * ASan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -o m16_strings_asan m16_string_questions.c
 * Run:    ./m16_string_questions
 */
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---- strlen: walk to the NUL; the difference of two pointers is the length ---- */
size_t my_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

/* ---- strcmp: compare as unsigned char, as the standard requires ---- */
int my_strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (*x > *y) - (*x < *y);
}

/* ---- strcpy: copies the NUL too. The caller guarantees dst is big enough ---- */
char *my_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

/* ---- strlcpy (BSD; added to glibc 2.38): always terminates, never overflows, and returns
 * strlen(src) so the caller can detect truncation with "if (ret >= size)" ---- */
size_t my_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = my_strlen(src);
    if (size > 0) {
        size_t n = len < size - 1 ? len : size - 1;
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i];
        dst[n] = '\0';
    }
    return len;
}

/* ---- memcpy: restrict promises no overlap, which lets the compiler vectorise ---- */
void *my_memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* ---- memmove: overlap allowed. If dst is after src, copy backwards so we never read a
 * byte we already overwrote. Comparing pointers into DIFFERENT objects with < is
 * undefined in C, so compare their integer values (implementation-defined, and correct on
 * every flat-memory platform). ---- */
void *my_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d < (uintptr_t)s) {
        while (n--)
            *d++ = *s++;
    } else if ((uintptr_t)d > (uintptr_t)s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

/* ---- atoi with overflow detection ----
 * Accepts: optional leading spaces, optional sign, at least one digit. Stops at the first
 * non-digit and reports where it stopped. Accumulates in unsigned so that INT_MIN, whose
 * magnitude is one more than INT_MAX, can be represented during parsing. */
enum { ATOI_OK = 0, ATOI_NO_DIGITS = 1, ATOI_OVERFLOW = 2 };

int my_atoi(const char *s, int *out, const char **end)
{
    while (isspace((unsigned char)*s))       /* cast: isspace(negative char) is UB */
        s++;
    int neg = 0;
    if (*s == '+' || *s == '-')
        neg = (*s++ == '-');
    if (!isdigit((unsigned char)*s)) {
        if (end) *end = s;
        return ATOI_NO_DIGITS;
    }
    unsigned limit = neg ? (unsigned)INT_MAX + 1u : (unsigned)INT_MAX;
    unsigned acc = 0;
    for (; isdigit((unsigned char)*s); s++) {
        unsigned d = (unsigned)(*s - '0');
        if (acc > (limit - d) / 10) {         /* acc * 10 + d would exceed limit */
            *out = neg ? INT_MIN : INT_MAX;   /* clamp, like strtol */
            while (isdigit((unsigned char)*s))
                s++;
            if (end) *end = s;
            return ATOI_OVERFLOW;
        }
        acc = acc * 10 + d;
    }
    if (neg)
        *out = acc == (unsigned)INT_MAX + 1u ? INT_MIN : -(int)acc;
    else
        *out = (int)acc;
    if (end) *end = s;
    return ATOI_OK;
}

/* ---- reverse a string in place: two indices walking towards each other ---- */
char *reverse_in_place(char *s)
{
    size_t n = my_strlen(s);
    if (n < 2)
        return s;
    for (size_t i = 0, j = n - 1; i < j; i++, j--) {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    return s;
}

/* ---- strtok_r: destructive, skips empty fields, state kept by the caller ---- */
char *my_strtok_r(char *s, const char *delim, char **save)
{
    if (s == NULL)
        s = *save;
    s += strspn(s, delim);                   /* skip leading delimiters */
    if (*s == '\0') {
        *save = s;
        return NULL;
    }
    char *end = s + strcspn(s, delim);       /* find the end of the token */
    if (*end != '\0')
        *end++ = '\0';
    *save = end;
    return s;
}

/* ---- non-destructive field splitter: keeps empty fields, like a CSV reader must.
 * Returns 1 and sets (*tok, *len) for each field; 0 when done. ---- */
int next_field(const char **cursor, char delim, const char **tok, size_t *len)
{
    const char *s = *cursor;
    if (s == NULL)
        return 0;
    const char *d = strchr(s, delim);
    *tok = s;
    if (d) {
        *len = (size_t)(d - s);
        *cursor = d + 1;
    } else {
        *len = strlen(s);
        *cursor = NULL;                      /* last field consumed */
    }
    return 1;
}

int main(void)
{
    /* strlen, strcmp */
    const char *samples[] = { "", "a", "hello", "ACC-1001", "\xff\x01", "longer string here" };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        assert(my_strlen(samples[i]) == strlen(samples[i]));
        for (size_t j = 0; j < sizeof samples / sizeof samples[0]; j++) {
            int mine = my_strcmp(samples[i], samples[j]), libc = strcmp(samples[i], samples[j]);
            assert((mine > 0) == (libc > 0) && (mine < 0) == (libc < 0));
        }
    }
    assert(my_strcmp("\xff", "a") > 0);       /* 0xff > 'a' as unsigned char */

    /* strcpy, strlcpy */
    char buf[16];
    assert(strcmp(my_strcpy(buf, "ACC-1001"), "ACC-1001") == 0);
    size_t r = my_strlcpy(buf, "a very long account name", sizeof buf);
    assert(r == 24 && r >= sizeof buf && strlen(buf) == 15);   /* truncated, terminated */
    r = my_strlcpy(buf, "short", sizeof buf);
    assert(r == 5 && strcmp(buf, "short") == 0);

    /* memcpy, memmove (both overlap directions) */
    char m[32] = "0123456789";
    my_memcpy(buf, m, 11);
    assert(strcmp(buf, "0123456789") == 0);
    my_memmove(m + 2, m, 10);                 /* overlap, dst after src: copy backwards */
    assert(memcmp(m, "010123456789", 12) == 0);
    char m2[32] = "0123456789";
    my_memmove(m2, m2 + 3, 8);                /* overlap, dst before src: copy forwards */
    assert(memcmp(m2, "3456789\0", 8) == 0);

    /* atoi */
    struct { const char *in; int ret; int val; char stop; } cases[] = {
        { "42", ATOI_OK, 42, '\0' },
        { "   -17xyz", ATOI_OK, -17, 'x' },
        { "+0", ATOI_OK, 0, '\0' },
        { "2147483647", ATOI_OK, INT_MAX, '\0' },
        { "-2147483648", ATOI_OK, INT_MIN, '\0' },
        { "2147483648", ATOI_OVERFLOW, INT_MAX, '\0' },
        { "-2147483649", ATOI_OVERFLOW, INT_MIN, '\0' },
        { "99999999999999999999,", ATOI_OVERFLOW, INT_MAX, ',' },
        { "", ATOI_NO_DIGITS, 0, '\0' },
        { "-", ATOI_NO_DIGITS, 0, '\0' },
        { "abc", ATOI_NO_DIGITS, 0, 'a' },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int v = 0;
        const char *end = NULL;
        int ret = my_atoi(cases[i].in, &v, &end);
        assert(ret == cases[i].ret);
        if (ret != ATOI_NO_DIGITS)
            assert(v == cases[i].val);
        assert(*end == cases[i].stop);
    }

    /* reverse */
    char s1[] = "", s2[] = "a", s3[] = "ab", s4[] = "banking";
    assert(strcmp(reverse_in_place(s1), "") == 0);
    assert(strcmp(reverse_in_place(s2), "a") == 0);
    assert(strcmp(reverse_in_place(s3), "ba") == 0);
    assert(strcmp(reverse_in_place(s4), "gniknab") == 0);

    /* strtok_r: empty fields vanish */
    char line[] = ",,TXN,,ACC-1001,DEBIT,,";
    char *save, *tok;
    const char *expect[] = { "TXN", "ACC-1001", "DEBIT" };
    size_t k = 0;
    for (tok = my_strtok_r(line, ",", &save); tok; tok = my_strtok_r(NULL, ",", &save))
        assert(k < 3 && strcmp(tok, expect[k++]) == 0);
    assert(k == 3);

    /* next_field: empty fields kept, input untouched */
    const char *csv = "TXN,,ACC-1001,";
    const char *cur = csv, *f;
    size_t flen, nf = 0;
    size_t lens[] = { 3, 0, 8, 0 };
    while (next_field(&cur, ',', &f, &flen))
        assert(nf < 4 && flen == lens[nf++]);
    assert(nf == 4);

    puts("all checks passed");
    return 0;
}
