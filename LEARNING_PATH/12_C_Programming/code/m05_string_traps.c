/* m05_string_traps.c - the string.h traps that cause real production bugs, each shown safely.
 *
 * Module: 12_C_Programming/05_strings_and_buffers.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m05_string_traps.c -o m05_string_traps
 *
 * The strncpy and snprintf demos receive their input through function parameters. If GCC could
 * see the literals it would refuse to compile (-Wstringop-truncation and -Wformat-truncation, both
 * part of -Wall): GCC knows these traps and catches the cases it can prove at compile time.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void show_bytes(const char *label, const char *buf, size_t n)
{
    printf("%-30s", label);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\0') printf(" \\0");
        else printf("  %c", c);
    }
    putchar('\n');
}

/* strncpy was designed for fixed-width, NUL-PADDED fields (old Unix directory entries),
 * not as a "safe strcpy". Two surprises: no terminator when src is too long, and it always
 * writes all n bytes, padding with zeros when src is short. */
static void strncpy_demo(const char *src)
{
    char dst[8];
    memset(dst, '#', sizeof dst);
    strncpy(dst, src, sizeof dst);
    show_bytes("strncpy(dst[8], src):", dst, sizeof dst);
    int terminated = memchr(dst, '\0', sizeof dst) != NULL;
    printf("%-30s %s\n", "  NUL-terminated?", terminated ? "yes" : "NO - strlen(dst) would read past the end");
}

/* Same idea for snprintf: with a literal 123456789 in main, GCC would prove the truncation at
 * compile time (-Wformat-truncation). Behind a parameter it is a run-time question, as in real code. */
static int make_label(char *buf, size_t n, int id)
{
    return snprintf(buf, n, "TXN-%d", id);
}

int main(void)
{
    /* 1. sizeof is the size of the OBJECT; strlen counts characters before the first NUL. */
    const char *p = "hello";
    char a[32] = "hello";
    printf("1. sizeof p = %zu, sizeof a = %zu, strlen = %zu\n", sizeof p, sizeof a, strlen(a));
    assert(strlen(p) == 5 && sizeof a == 32);

    /* 2. strncpy: short source is padded, long source is NOT terminated. */
    strncpy_demo("abc");
    strncpy_demo("ABCDEFGHIJ");

    /* 3. strncat's n is the most characters to APPEND, not the destination size. */
    char path[16] = "/var/log/";
    strncat(path, "server-2026-10-13.log", sizeof path - strlen(path) - 1);   /* the correct n */
    printf("3. strncat with the right n: \"%s\" (%zu chars, fits in 16)\n", path, strlen(path));
    assert(strlen(path) == 15);

    /* 4. strcmp returns <0, 0 or >0 - not -1/0/1 - and 0 means EQUAL. */
    int c1 = strcmp("abc", "abd"), c2 = strcmp("abc", "abc");
    printf("4. strcmp(\"abc\",\"abd\") %s 0, strcmp(\"abc\",\"abc\") == 0 is %s\n",
           c1 < 0 ? "<" : ">=", c2 == 0 ? "true" : "false");
    assert(c1 < 0 && c2 == 0);

    /* 5. == compares ADDRESSES, not contents. */
    char x[] = "PAID", y[] = "PAID";
    const char *px = x, *py = y;
    printf("5. two \"PAID\" arrays: == says %s, strcmp says %s\n", px == py ? "equal" : "different",
           strcmp(px, py) == 0 ? "equal" : "different");
    assert(px != py && strcmp(px, py) == 0);

    /* 6. snprintf returns the length it WANTED to write. Compare it with the size to detect
     *    truncation. The output is always terminated (when size > 0). */
    char small[8];
    int want = make_label(small, sizeof small, 123456789);
    printf("6. snprintf wanted %d chars, buffer holds %zu: got \"%s\" -> %s\n", want, sizeof small - 1,
           small, (want < 0 || (size_t)want >= sizeof small) ? "TRUNCATED" : "complete");
    assert(want == 13 && strcmp(small, "TXN-123") == 0);

    /* 7. The off-by-one: a string of length n needs n + 1 bytes. */
    const char *code = "IFSC0001";
    char exact[9];                                /* 8 chars + NUL */
    assert(strlen(code) + 1 <= sizeof exact);
    memcpy(exact, code, strlen(code) + 1);        /* copy the terminator too */
    printf("7. \"%s\" needs %zu bytes, not %zu\n", exact, strlen(code) + 1, strlen(code));
    return 0;
}
