/* m15_refactored_report.c - the legacy code from m15_legacy_report.c after a safe refactor,
 * done in the order you would do it in a real codebase, each step its own commit:
 *
 *   step 1  introduce a struct: the three globals become struct ledger (state is explicit)
 *   step 2  extract function: parsing is split from applying; input is no longer modified
 *   step 3  caller-provided buffer: fmt_amount_r() replaces the static buffer
 *   step 4  compatibility shim: the old fmt_amount() API stays for old callers (strangler)
 *   step 5  THE FIX, separately: bug #4711, with exactly two characterisation rows changed
 *
 * Steps 1-4 are behaviour-preserving: the characterisation tables from the legacy file pass
 * unchanged (including the lenient atol parsing, which callers may rely on - tightening it
 * is a product decision, not a refactor). Step 5 changes behaviour on purpose.
 *
 * In a real header the struct would be opaque:
 *     typedef struct ledger ledger_t;
 *     ledger_t *ledger_create(void);
 *     void      ledger_destroy(ledger_t *);
 * Here it is defined in the same file so the test can look inside.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m15_refactored_report m15_refactored_report.c
 * Run:    ./m15_refactored_report
 */
#define _POSIX_C_SOURCE 200809L   /* strtok_r */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- step 1: explicit state ---------------- */
struct ledger {
    int  txn_count;
    long total_paise;
    char last_error[128];
};

/* ---------------- step 3 + step 5: caller buffer, sign handled separately ---------------- */
/* Returns the number of characters written (like snprintf), or -1 if buf is too small. */
static int fmt_amount_r(long paise, char *buf, size_t n)
{
    /* Work on the magnitude as unsigned long: -LONG_MIN does not fit in a long, but
     * 0UL - (unsigned long)LONG_MIN is well defined and gives the right magnitude. */
    unsigned long mag = paise < 0 ? 0UL - (unsigned long)paise : (unsigned long)paise;
    int w = snprintf(buf, n, "%s%lu.%02lu", paise < 0 ? "-" : "", mag / 100, mag % 100);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

/* ---------------- step 4: the old API, kept for old callers ---------------- */
/* Thread-safe now (one buffer per thread) but still not reentrant: two calls in one
 * printf still clash. New code calls fmt_amount_r. */
static char *fmt_amount(long paise)
{
    static _Thread_local char buf[32];
    fmt_amount_r(paise, buf, sizeof buf);
    return buf;
}

/* ---------------- step 2: parse separately from apply ---------------- */
struct txn {
    long amount;           /* signed: DEBIT is negative */
};

/* Same acceptance rules as the legacy strtok code, on a private copy of the input. */
static int parse_txn(const char *line, struct txn *out, char *err, size_t errn)
{
    char copy[256];
    if (snprintf(copy, sizeof copy, "%s", line) >= (int)sizeof copy) {
        snprintf(err, errn, "bad format");
        return -1;
    }
    char *save;
    char *type = strtok_r(copy, "|", &save);
    char *acc = strtok_r(NULL, "|", &save);
    char *amt = strtok_r(NULL, "|\n", &save);
    if (!type || !acc || !amt) {
        snprintf(err, errn, "bad format");
        return -1;
    }
    long v = atol(amt);                    /* preserved on purpose; see the header comment */
    if (strcmp(type, "DEBIT") == 0)
        v = -v;
    else if (strcmp(type, "CREDIT") != 0) {
        snprintf(err, errn, "bad type %.40s", type);
        return -1;
    }
    out->amount = v;
    return 0;
}

static int ledger_process_line(struct ledger *lg, const char *line)
{
    struct txn t;
    if (parse_txn(line, &t, lg->last_error, sizeof lg->last_error) != 0)
        return -1;
    lg->txn_count++;
    lg->total_paise += t.amount;
    return 0;
}

/* ---------------- the characterisation tables, copied from the legacy file ---------------- */
struct fmt_case {
    long        in;
    const char *out;
};

static const struct fmt_case FMT_CASES[] = {
    { 0, "0.00" },
    { 5, "0.05" },
    { 100, "1.00" },
    { 123456, "1234.56" },
    { -250, "-2.50" },
    { -50, "-0.50" },                  /* CHANGED by the fix for #4711 (was "0.50") */
    { -1, "-0.01" },                   /* CHANGED by the fix for #4711 (was "0.01") */
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

static const struct line_case LINE_CASES[] = {
    { "CREDIT|ACC-1001|2500\n", 0, 1, 2500, "" },
    { "DEBIT|ACC-1001|1000\n", 0, 2, 1500, "" },
    { "DEBIT|ACC-1001|12abc\n", 0, 3, 1488, "" },
    { "REFUND|ACC-1001|10\n", -1, 3, 1488, "bad type REFUND" },
    { "CREDIT|ACC-1001\n", -1, 3, 1488, "bad format" },
    { "CREDIT||500\n", -1, 3, 1488, "bad format" },
    { "CREDIT|ACC-1002|abc\n", 0, 4, 1488, "bad format" },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

int main(void)
{
    size_t failures = 0;
    char buf[32];

    for (size_t i = 0; i < COUNT(FMT_CASES); i++) {
        int w = fmt_amount_r(FMT_CASES[i].in, buf, sizeof buf);
        if (w < 0 || strcmp(buf, FMT_CASES[i].out) != 0 || strcmp(fmt_amount(FMT_CASES[i].in), buf) != 0) {
            printf("fmt(%ld): expected \"%s\", got \"%s\"\n", FMT_CASES[i].in, FMT_CASES[i].out, buf);
            failures++;
        }
    }

    struct ledger lg = { 0, 0, "" };
    for (size_t i = 0; i < COUNT(LINE_CASES); i++) {
        const struct line_case *c = &LINE_CASES[i];
        int r = ledger_process_line(&lg, c->line);      /* the input is const now */
        if (r != c->ret || lg.txn_count != c->count_after || lg.total_paise != c->total_after ||
            strcmp(lg.last_error, c->error_after) != 0) {
            printf("line %zu: got ret %d count %d total %ld error \"%s\"\n",
                   i, r, lg.txn_count, lg.total_paise, lg.last_error);
            failures++;
        }
    }

    /* new guarantees the refactor adds, each with its own test */
    volatile size_t tiny = 4;     /* volatile: otherwise GCC -O3 warns about the truncation we test */
    assert(fmt_amount_r(123456, buf, tiny) == -1);     /* too small: reported, not overflowed */
    struct ledger a = { 0, 0, "" }, b = { 0, 0, "" };  /* two independent ledgers now possible */
    int r1 = ledger_process_line(&a, "CREDIT|X|100\n");
    int r2 = ledger_process_line(&b, "DEBIT|Y|40\n");
    assert(r1 == 0 && r2 == 0 && a.total_paise == 100 && b.total_paise == -40);

    printf("%zu fmt cases, %zu line cases, %zu failures (2 fmt rows changed by the #4711 fix)\n",
           COUNT(FMT_CASES), COUNT(LINE_CASES), failures);
    assert(failures == 0);
    puts("all checks passed");
    return 0;
}
