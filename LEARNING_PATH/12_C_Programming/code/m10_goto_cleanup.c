/* m10_goto_cleanup.c - the goto-cleanup idiom in its two common shapes, and fault injection
 * that proves every error path releases everything.
 *
 *   copy_file()      Linux-kernel style: one label per resource, released in reverse order.
 *   load_accounts()  single "cleanup:" label, every resource starts as NULL, ownership of the
 *                    result is transferred to the caller only on success.
 *
 * The test makes the 1st, 2nd, 3rd ... allocation or fopen fail on purpose and checks that
 * after each failure no memory is live and no file is open. This is how you test error paths
 * that never happen on a developer machine.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m10_goto_cleanup m10_goto_cleanup.c
 * Run:    ./m10_goto_cleanup
 * Extra:  valgrind --leak-check=full ./m10_goto_cleanup   (should report 0 bytes lost)
 */
#define _POSIX_C_SOURCE 200809L   /* mkstemp, unlink, close */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- fault injection layer ----------------
 * In a real codebase these wrappers live in one file and are switched on by a test build.
 * g_fail_at = N makes the Nth wrapped call fail; 0 means never fail. */
static int g_fail_at;
static int g_calls;
static int g_live_allocs;
static int g_open_files;

static int should_fail(void)
{
    g_calls++;
    return g_fail_at != 0 && g_calls == g_fail_at;
}

static void *t_malloc(size_t n)
{
    if (should_fail()) return NULL;
    void *p = malloc(n);
    if (p) g_live_allocs++;
    return p;
}

static void *t_realloc(void *old, size_t n)
{
    if (should_fail()) return NULL;          /* like real realloc: old block untouched */
    void *p = realloc(old, n);
    if (p && !old) g_live_allocs++;
    return p;
}

static void t_free(void *p)
{
    if (p) {
        g_live_allocs--;
        free(p);
    }
}

static FILE *t_fopen(const char *path, const char *mode)
{
    if (should_fail()) {
        errno = EMFILE;                      /* pretend we ran out of descriptors */
        return NULL;
    }
    FILE *f = fopen(path, mode);
    if (f) g_open_files++;
    return f;
}

static int t_fclose(FILE *f)
{
    g_open_files--;
    return fclose(f);
}

static void reset_injection(int fail_at)
{
    g_fail_at = fail_at;
    g_calls = 0;
}

/* ---------------- shape 1: kernel style, one label per resource ---------------- */
#define COPY_BUF_SIZE 4096

/* Returns 0 or a positive errno value. */
static int copy_file(const char *src_path, const char *dst_path)
{
    int err = 0;
    FILE *in, *out;
    char *buf;
    size_t n;

    in = t_fopen(src_path, "rb");
    if (!in) {
        err = errno;
        goto out;
    }
    out = t_fopen(dst_path, "wb");
    if (!out) {
        err = errno;
        goto close_in;
    }
    buf = t_malloc(COPY_BUF_SIZE);
    if (!buf) {
        err = ENOMEM;
        goto close_out;
    }

    while ((n = fread(buf, 1, COPY_BUF_SIZE, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            err = EIO;
            goto free_buf;
        }
    }
    if (ferror(in))
        err = EIO;

free_buf:
    t_free(buf);
close_out:
    /* fclose flushes the last buffer: a full disk shows up HERE, so its result matters
     * for an output file. Keep the first error if there already was one. */
    if (t_fclose(out) != 0 && err == 0)
        err = errno;
close_in:
    t_fclose(in);
out:
    return err;
}

/* ---------------- shape 2: one cleanup label, NULL-initialised resources ---------------- */
struct account {
    char      id[16];
    long long balance_paise;
};

struct account_table {
    size_t          count;
    size_t          cap;
    struct account *items;
};

/* Accepts NULL, like free(): makes cleanup code simpler. */
static void account_table_free(struct account_table *t)
{
    if (!t) return;
    t_free(t->items);
    t_free(t);
}

/* "ACC-1001,250000\n" -> id + balance. Returns 0 or EINVAL. */
static int parse_account(const char *line, struct account *a)
{
    const char *comma = strchr(line, ',');
    if (!comma) return EINVAL;
    size_t idlen = (size_t)(comma - line);
    if (idlen == 0 || idlen >= sizeof a->id) return EINVAL;
    memcpy(a->id, line, idlen);
    a->id[idlen] = '\0';

    char *end;
    errno = 0;
    long long v = strtoll(comma + 1, &end, 10);
    if (errno != 0 || end == comma + 1) return EINVAL;
    if (*end != '\n' && *end != '\0') return EINVAL;
    a->balance_paise = v;
    return 0;
}

#define LINE_CAP 128

/* Returns 0 and sets *out (caller owns it, release with account_table_free), or returns a
 * positive errno value and leaves *out untouched. On EINVAL, *bad_line gets the line number. */
static int load_accounts(const char *path, struct account_table **out, int *bad_line)
{
    int err = 0;
    FILE *f = NULL;
    char *line = NULL;
    struct account_table *t = NULL;
    int lineno = 0;

    assert(path != NULL && out != NULL);     /* caller bug, not a runtime condition */

    f = t_fopen(path, "r");
    if (!f) {
        err = errno;
        goto cleanup;
    }
    line = t_malloc(LINE_CAP);
    if (!line) {
        err = ENOMEM;
        goto cleanup;
    }
    t = t_malloc(sizeof *t);
    if (!t) {
        err = ENOMEM;
        goto cleanup;
    }
    t->count = 0;
    t->cap = 0;
    t->items = NULL;

    while (fgets(line, LINE_CAP, f)) {
        struct account a;
        lineno++;
        if (parse_account(line, &a) != 0) {
            err = EINVAL;
            if (bad_line) *bad_line = lineno;
            goto cleanup;
        }
        if (t->count == t->cap) {
            size_t ncap = t->cap ? t->cap * 2 : 4;
            struct account *grown = t_realloc(t->items, ncap * sizeof *grown);
            if (!grown) {                    /* t->items is still valid and is freed below */
                err = ENOMEM;
                goto cleanup;
            }
            t->items = grown;
            t->cap = ncap;
        }
        t->items[t->count++] = a;
    }
    if (ferror(f)) {
        err = EIO;
        goto cleanup;
    }

    *out = t;        /* ownership moves to the caller ...           */
    t = NULL;        /* ... so the cleanup below must not free it   */

cleanup:
    account_table_free(t);
    t_free(line);
    if (f)
        t_fclose(f);
    return err;
}

/* ---------------- test helpers ---------------- */
static void write_temp(char *path_template, const char *content)
{
    int fd = mkstemp(path_template);
    assert(fd >= 0);
    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);     /* never put the real work inside assert() */
    assert(w == (ssize_t)len);
    (void)w;
    close(fd);
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int ca, cb, same = fa && fb;
    while (same) {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) same = 0;
        if (ca == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

int main(void)
{
    char src[] = "/tmp/m10_src_XXXXXX";
    char dst[] = "/tmp/m10_dst_XXXXXX";
    char acc[] = "/tmp/m10_acc_XXXXXX";
    char bad[] = "/tmp/m10_bad_XXXXXX";
    write_temp(src, "line one\nline two\n");
    write_temp(dst, "");
    write_temp(acc,
               "ACC-1001,250000\nACC-1002,-1500\nACC-1003,0\nACC-1004,99\nACC-1005,1\n"
               "ACC-1006,2\nACC-1007,3\nACC-1008,4\nACC-1009,5\nACC-1010,6\n");
    write_temp(bad, "ACC-1,10\nACC-2,20\nACC-3,twenty\n");

    /* copy_file: success, then every failure point. */
    reset_injection(0);
    int rc = copy_file(src, dst);
    assert(rc == 0);
    assert(files_equal(src, dst));
    int copy_points = g_calls;
    for (int n = 1; n <= copy_points; n++) {
        reset_injection(n);
        int err = copy_file(src, dst);
        assert(err != 0);
        assert(g_live_allocs == 0 && g_open_files == 0);
    }
    printf("copy_file: %d failure points injected, 0 leaks, 0 open files\n", copy_points);

    /* load_accounts: success. 10 lines -> capacity 4 -> 8 -> 16: two reallocs. */
    reset_injection(0);
    struct account_table *t = NULL;
    rc = load_accounts(acc, &t, NULL);
    assert(rc == 0);
    assert(t != NULL && t->count == 10);
    assert(strcmp(t->items[0].id, "ACC-1001") == 0 && t->items[0].balance_paise == 250000);
    assert(t->items[1].balance_paise == -1500);
    int load_points = g_calls;
    account_table_free(t);
    assert(g_live_allocs == 0 && g_open_files == 0);

    for (int n = 1; n <= load_points; n++) {
        struct account_table *never = (struct account_table *)&never;   /* sentinel */
        reset_injection(n);
        int err = load_accounts(acc, &never, NULL);
        assert(err == ENOMEM || err == EMFILE);
        assert(never == (struct account_table *)&never);   /* *out untouched on failure */
        assert(g_live_allocs == 0 && g_open_files == 0);
    }
    printf("load_accounts: %d failure points injected, 0 leaks, 0 open files\n", load_points);

    /* Parse error on line 3: EINVAL, line number reported, nothing leaked. */
    reset_injection(0);
    int bad_line = 0;
    struct account_table *t2 = NULL;
    rc = load_accounts(bad, &t2, &bad_line);
    assert(rc == EINVAL);
    assert(bad_line == 3 && t2 == NULL);
    assert(g_live_allocs == 0 && g_open_files == 0);
    printf("load_accounts(bad file): %s at line %d\n", strerror(rc), bad_line);

    /* Missing file: the errno from fopen is passed through. */
    rc = load_accounts("/no/such/dir/accounts.csv", &t2, NULL);
    assert(rc == ENOENT);
    assert(g_live_allocs == 0 && g_open_files == 0);
    printf("load_accounts(missing file): %s\n", strerror(rc));

    unlink(src);
    unlink(dst);
    unlink(acc);
    unlink(bad);
    puts("all checks passed");
    return 0;
}
