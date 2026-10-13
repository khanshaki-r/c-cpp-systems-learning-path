/* m05_safe_string_helpers.c - small string helpers that always terminate and always report truncation.
 *
 * Module: 12_C_Programming/05_strings_and_buffers.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m05_safe_string_helpers.c -o m05_safe_string_helpers
 */
#define _POSIX_C_SOURCE 200809L  /* strnlen is POSIX (and C23), so -std=c17 hides it without this */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Copy with the strlcpy contract (BSD; also in glibc since 2.38, not in MSVC):
 * always NUL-terminates when dstsz > 0 and returns strlen(src).
 * Truncation happened if and only if the return value >= dstsz. */
static size_t str_copy(char *dst, size_t dstsz, const char *src)
{
    size_t srclen = strlen(src);
    if (dstsz > 0) {
        size_t n = srclen < dstsz - 1 ? srclen : dstsz - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}

/* Append with the strlcat contract: returns the length the full result would have had. */
static size_t str_append(char *dst, size_t dstsz, const char *src)
{
    size_t dlen = strnlen(dst, dstsz);          /* strnlen: POSIX and C23, never reads past dstsz */
    if (dlen == dstsz) return dstsz + strlen(src);   /* dst was not terminated: refuse */
    return dlen + str_copy(dst + dlen, dstsz - dlen, src);
}

/* Fixed-width record fields (Berkeley DB records, COBOL copybooks, mainframe files) are
 * space-padded and NOT NUL-terminated. Turn one into a C string, trimming trailing spaces. */
static size_t field_to_cstr(char *dst, size_t dstsz, const char *field, size_t width)
{
    size_t n = width;
    while (n > 0 && (field[n - 1] == ' ' || field[n - 1] == '\0')) --n;
    if (dstsz == 0) return n;
    size_t copy = n < dstsz - 1 ? n : dstsz - 1;
    memcpy(dst, field, copy);
    dst[copy] = '\0';
    return n;
}

/* A string builder over a caller-supplied buffer. Appends never overflow; once anything is cut
 * off, `truncated` stays true so the caller checks once at the end. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;
} strbuf;

static void sb_init(strbuf *sb, char *storage, size_t cap)
{
    assert(cap > 0);
    sb->buf = storage;
    sb->cap = cap;
    sb->len = 0;
    sb->truncated = false;
    storage[0] = '\0';
}

__attribute__((format(printf, 2, 3)))           /* lets -Wformat check every call site */
static void sb_printf(strbuf *sb, const char *fmt, ...)
{
    if (sb->truncated) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->truncated = true;
        sb->len = sb->cap - 1;                  /* vsnprintf filled and terminated the rest */
    } else {
        sb->len += (size_t)n;
    }
}

int main(void)
{
    char small[8];
    size_t need = str_copy(small, sizeof small, "ACCOUNT-0012345");
    printf("str_copy into 8 bytes: \"%s\", needed %zu -> %s\n", small, need,
           need >= sizeof small ? "truncated" : "complete");
    assert(strcmp(small, "ACCOUNT") == 0 && need == 15);

    char path[24] = "/srv/";
    need = str_append(path, sizeof path, "bank/");
    need = str_append(path, sizeof path, "ledger.dat");
    printf("str_append: \"%s\" (%zu chars, %s)\n", path, need, need >= sizeof path ? "truncated" : "fits");
    assert(strcmp(path, "/srv/bank/ledger.dat") == 0);

    /* A 12-byte loan number field from a packed record: "LN0042      " with no terminator. */
    const char record[] = {'L', 'N', '0', '0', '4', '2', ' ', ' ', ' ', ' ', ' ', ' ', 'X'};
    char loan_no[13];
    size_t len = field_to_cstr(loan_no, sizeof loan_no, record, 12);
    printf("field_to_cstr: \"%s\" (%zu chars)\n", loan_no, len);
    assert(strcmp(loan_no, "LN0042") == 0);

    char storage[40];
    strbuf sb;
    sb_init(&sb, storage, sizeof storage);
    sb_printf(&sb, "acct=%s ", "ACC-000042");
    sb_printf(&sb, "amount=%lld.%02lld ", 1234LL, 5LL);
    printf("strbuf: \"%s\" truncated=%s\n", sb.buf, sb.truncated ? "yes" : "no");
    assert(!sb.truncated);
    sb_printf(&sb, "status=%s", "SETTLED-AFTER-REVIEW");
    printf("strbuf: \"%s\" truncated=%s\n", sb.buf, sb.truncated ? "yes" : "no");
    assert(sb.truncated && strlen(sb.buf) == sizeof storage - 1);
    return 0;
}
