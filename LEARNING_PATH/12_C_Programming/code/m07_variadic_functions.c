/* m07_variadic_functions.c - stdarg.h: count-based, sentinel-terminated and printf-style functions.
 *
 * Module: 12_C_Programming/07_functions_function_pointers_and_callbacks.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m07_variadic_functions.c -o m07_variadic_functions
 */
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1. Count first. The callee has NO way to know how many arguments or their types: it trusts
 * `count` and reads every argument as int64_t. Passing a plain int (4 bytes) where int64_t is
 * read is undefined behaviour, hence INT64_C() at the call site. */
static int64_t sum_i64(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int64_t s = 0;
    for (int i = 0; i < count; ++i) s += va_arg(ap, int64_t);
    va_end(ap);
    return s;
}

/* 2. Sentinel-terminated: stop at a NULL pointer. The sentinel must be a POINTER: plain NULL can
 * be the int 0, which is the wrong size where pointers are 8 bytes. GCC's sentinel attribute
 * checks call sites for a trailing (char *)NULL. */
__attribute__((sentinel))
static size_t join_words(char *out, size_t outsz, const char *first, ...)
{
    va_list ap;
    va_start(ap, first);
    size_t used = 0;
    out[0] = '\0';
    for (const char *w = first; w != NULL; w = va_arg(ap, const char *)) {
        int n = snprintf(out + used, outsz - used, "%s%s", used ? " " : "", w);
        if (n < 0 || (size_t)n >= outsz - used) { used = outsz - 1; break; }
        used += (size_t)n;
    }
    va_end(ap);
    return used;
}

/* 3. A printf-style function. The format attribute makes GCC check every call like printf. */
__attribute__((format(printf, 1, 2)))
static char *format_alloc(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);                         /* a va_list can be walked ONCE: copy it first */
    int n = vsnprintf(NULL, 0, fmt, ap);      /* pass 1: measure */
    va_end(ap);
    char *s = NULL;
    if (n >= 0 && (s = malloc((size_t)n + 1)) != NULL)
        vsnprintf(s, (size_t)n + 1, fmt, ap2);  /* pass 2: format */
    va_end(ap2);
    return s;                                 /* caller frees */
}

/* 4. Forwarding: a logger that adds a prefix and hands the rest to vfprintf. */
__attribute__((format(printf, 2, 3)))
static void log_line(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", level);
    vprintf(fmt, ap);                         /* the v-variants take a va_list */
    va_end(ap);
    putchar('\n');
}

int main(void)
{
    int64_t s = sum_i64(3, INT64_C(100), INT64_C(250), INT64_C(-50));
    printf("sum_i64(3, 100, 250, -50) = %" PRId64 "\n", s);
    assert(s == 300);

    char buf[32];
    join_words(buf, sizeof buf, "NEFT", "batch", "07", "settled", (char *)NULL);
    printf("join_words -> \"%s\"\n", buf);
    assert(strcmp(buf, "NEFT batch 07 settled") == 0);

    char *msg = format_alloc("acct=%s amount=%" PRId64 ".%02d", "ACC-000042", (int64_t)1234, 5);
    if (msg == NULL) return 1;
    printf("format_alloc -> \"%s\" (%zu bytes allocated)\n", msg, strlen(msg) + 1);
    free(msg);

    log_line("INFO", "server listening on port %d", 7000);

    /* Format-string safety: data from outside is ALWAYS an argument, never the format.
     *   printf(user_text);        -> attacker writes %s%s%s to crash, %x to leak the stack,
     *                                %n to write memory. GCC: -Wformat-security.
     *   printf("%s", user_text);  -> prints the bytes, nothing more. */
    const char *user_text = "100% done %s %n";
    printf("%s\n", user_text);
    log_line("WARN", "%s", user_text);
    return 0;
}
