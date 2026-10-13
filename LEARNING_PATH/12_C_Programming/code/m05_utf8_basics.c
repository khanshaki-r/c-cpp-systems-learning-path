/* m05_utf8_basics.c - bytes are not characters: counting, validating and truncating UTF-8.
 *
 * Module: 12_C_Programming/05_strings_and_buffers.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m05_utf8_basics.c -o m05_utf8_basics
 *
 * Non-ASCII text is written with \x escapes so the source file is pure ASCII and compiles the
 * same with GCC and with MSVC (which assumes the local code page unless you pass /utf-8).
 */
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* "INR sign + 100": U+20B9 is 3 bytes in UTF-8: E2 82 B9. */
#define RUPEE "\xE2\x82\xB9"

/* Code points = bytes that are NOT continuation bytes (continuation bytes look like 10xxxxxx). */
static size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

/* Strict validation: rejects bad lead bytes, truncated sequences, overlong forms,
 * UTF-16 surrogates (U+D800..U+DFFF) and anything above U+10FFFF. */
static bool utf8_valid(const char *str)
{
    const unsigned char *s = (const unsigned char *)str;
    while (*s) {
        uint32_t cp;
        int extra;
        if (*s < 0x80)              { ++s; continue; }
        else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; extra = 1; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; extra = 2; }
        else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; extra = 3; }
        else return false;                              /* continuation byte or 0xF8..0xFF as lead */
        ++s;
        for (int i = 0; i < extra; ++i, ++s) {
            if ((*s & 0xC0) != 0x80) return false;      /* also catches the terminating NUL */
            cp = (cp << 6) | (*s & 0x3F);
        }
        static const uint32_t min_for_len[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < min_for_len[extra]) return false;      /* overlong, e.g. C0 AF for '/' */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
    }
    return true;
}

/* Copy at most dstsz - 1 bytes without cutting a multi-byte character in half. */
static size_t utf8_truncate_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;   /* back off to a lead byte */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

int main(void)
{
    const char *price = RUPEE "100";
    printf("\"%s\": strlen = %zu bytes, %zu code points\n", price, strlen(price), utf8_count(price));
    assert(strlen(price) == 6 && utf8_count(price) == 4);

    const char *name = "Jos\xC3\xA9";                    /* "Jose" with an accented e: U+00E9 */
    printf("\"%s\": %zu bytes, %zu code points\n", name, strlen(name), utf8_count(name));

    const char *bad_inputs[] = {"\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80"};
    const char *why[] = {"overlong '/'", "truncated", "surrogate", "above U+10FFFF", "stray continuation"};
    assert(utf8_valid(price) && utf8_valid(name) && utf8_valid("plain ASCII"));
    for (int i = 0; i < 5; ++i) {
        printf("invalid (%s) rejected: %s\n", why[i], utf8_valid(bad_inputs[i]) ? "NO" : "yes");
        assert(!utf8_valid(bad_inputs[i]));
    }

    char out[4];                                          /* room for 3 bytes + NUL */
    size_t n = utf8_truncate_copy(out, sizeof out, price);
    printf("truncate \"%s\" to 3 bytes -> \"%s\" (%zu bytes, the rupee sign kept whole)\n", price, out, n);
    char out2[3];
    n = utf8_truncate_copy(out2, sizeof out2, price);
    printf("truncate to 2 bytes -> \"%s\" (%zu bytes: cutting inside the sign is not allowed)\n", out2, n);
    assert(n == 0);

    /* <ctype.h> functions take an int that must be EOF or an unsigned char value. Passing a
     * plain char >= 0x80 (negative on x86) is undefined behaviour: cast first. */
    int alpha = isalpha((unsigned char)name[3]);
    printf("isalpha((unsigned char)0xC3) in the \"C\" locale -> %s\n", alpha ? "true" : "false");
    return 0;
}
