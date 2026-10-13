/* m15_legacy_report.c - characterisation tests ("golden master" tests) around legacy C code.
 *
 * The top half is legacy code written in the style you meet in old servers: global state,
 * a static result buffer, strtok, atol, a function that modifies its input. It has a real
 * bug (amounts between -0.99 and -0.01 lose their minus sign). It is NOT cleaned up here:
 * the rule is "pin the current behaviour first, change it second".
 *
 * The bottom half pins its behaviour - including the bug - in tables. Generate a table from
 * the running code with --print, read it, then paste it in as the expected values.
 * m15_refactored_report.c then refactors the code and must pass the same tables.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m15_legacy_report m15_legacy_report.c
 * Run:    ./m15_legacy_report            (check current behaviour against the pinned tables)
 *         ./m15_legacy_report --print    (print current behaviour as C table rows)
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ======================= LEGACY CODE (as found) ======================= */
char g_last_error[128];
int  g_txn_count;
long g_total_paise;

/* Returns a pointer to a static buffer: valid until the next call. */
char *fmt_amount(long paise)
{
    static char buf[40];               /* GCC -Wformat-overflow: up to 39 bytes + NUL */
    long rupees = paise / 100;
    long frac = paise % 100;
    if (frac < 0)
        frac = -frac;
    sprintf(buf, "%ld.%02ld", rupees, frac);   /* BUG #4711: -50 -> rupees 0 -> "0.50" */
    return buf;
}

/* "CREDIT|ACC-1001|2500" or "DEBIT|ACC-1001|2500". Modifies line (strtok). */
int process_line(char *line)
{
    char *type, *acc, *amt;
    long v;

    type = strtok(line, "|");
    acc = strtok(NULL, "|");
    amt = strtok(NULL, "|\n");
    if (type == NULL || acc == NULL || amt == NULL) {
        strcpy(g_last_error, "bad format");
        return -1;
    }
    v = atol(amt);                     /* "12abc" -> 12, "abc" -> 0: no error detection */
    if (strcmp(type, "DEBIT") == 0)
        v = -v;
    else if (strcmp(type, "CREDIT") != 0) {
        sprintf(g_last_error, "bad type %.40s", type);
        return -1;
    }
    g_txn_count++;
    g_total_paise += v;
    return 0;
}
/* ======================= END OF LEGACY CODE ======================= */

/* ---------------- characterisation tables ---------------- */
struct fmt_case {
    long        in;
    const char *out;
};

/* Pinned from the running code on 2026-10-13 (gcc 13, x86-64 Linux). The -50 row records
 * the CURRENT, WRONG behaviour on purpose; the fix will change exactly that row. */
static const struct fmt_case FMT_CASES[] = {
    { 0, "0.00" },
    { 5, "0.05" },
    { 100, "1.00" },
    { 123456, "1234.56" },
    { -250, "-2.50" },
    { -50, "0.50" },                   /* KNOWN BUG #4711: sign lost */
    { -1, "0.01" },                    /* KNOWN BUG #4711: sign lost */
    { LONG_MAX, "92233720368547758.07" },
    { LONG_MIN, "-92233720368547758.08" },
};

struct line_case {
    const char *line;
    int         ret;
    int         count_after;
    long        total_after;
    const char *error_after;
};

/* Applied in order: each row sees the state left by the previous rows. */
static const struct line_case LINE_CASES[] = {
    { "CREDIT|ACC-1001|2500\n", 0, 1, 2500, "" },
    { "DEBIT|ACC-1001|1000\n", 0, 2, 1500, "" },
    { "DEBIT|ACC-1001|12abc\n", 0, 3, 1488, "" },           /* atol accepts the prefix */
    { "REFUND|ACC-1001|10\n", -1, 3, 1488, "bad type REFUND" },
    { "CREDIT|ACC-1001\n", -1, 3, 1488, "bad format" },
    { "CREDIT||500\n", -1, 3, 1488, "bad format" },           /* strtok merges "||" */
    { "CREDIT|ACC-1002|abc\n", 0, 4, 1488, "bad format" },  /* "abc" -> 0, error not cleared */
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void reset_globals(void)
{
    g_last_error[0] = '\0';
    g_txn_count = 0;
    g_total_paise = 0;
}

static void print_tables(void)
{
    puts("FMT_CASES:");
    for (size_t i = 0; i < COUNT(FMT_CASES); i++)
        printf("    { %ld, \"%s\" },\n", FMT_CASES[i].in, fmt_amount(FMT_CASES[i].in));
    puts("LINE_CASES:");
    reset_globals();
    for (size_t i = 0; i < COUNT(LINE_CASES); i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", LINE_CASES[i].line);
        int r = process_line(buf);
        printf("    { line %zu: ret %d, count %d, total %ld, error \"%s\" },\n",
               i, r, g_txn_count, g_total_paise, g_last_error);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--print") == 0) {
        print_tables();
        return 0;
    }

    size_t failures = 0;
    for (size_t i = 0; i < COUNT(FMT_CASES); i++) {
        const char *got = fmt_amount(FMT_CASES[i].in);
        if (strcmp(got, FMT_CASES[i].out) != 0) {
            printf("fmt_amount(%ld): expected \"%s\", got \"%s\"\n", FMT_CASES[i].in, FMT_CASES[i].out, got);
            failures++;
        }
    }

    reset_globals();
    for (size_t i = 0; i < COUNT(LINE_CASES); i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", LINE_CASES[i].line);   /* process_line writes into it */
        int r = process_line(buf);
        const struct line_case *c = &LINE_CASES[i];
        if (r != c->ret || g_txn_count != c->count_after || g_total_paise != c->total_after ||
            strcmp(g_last_error, c->error_after) != 0) {
            printf("line %zu: expected ret %d count %d total %ld error \"%s\"; "
                   "got ret %d count %d total %ld error \"%s\"\n",
                   i, c->ret, c->count_after, c->total_after, c->error_after,
                   r, g_txn_count, g_total_paise, g_last_error);
            failures++;
        }
    }

    /* Hidden coupling through the static buffer, pinned as well. */
    char *first = fmt_amount(100);
    char *second = fmt_amount(200);
    assert(first == second && strcmp(first, "2.00") == 0);

    printf("%zu fmt cases, %zu line cases, %zu failures\n", COUNT(FMT_CASES), COUNT(LINE_CASES), failures);
    assert(failures == 0);
    puts("all checks passed");
    return 0;
}
