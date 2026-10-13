/* m10_errno_conventions.c - errno used correctly, errno used wrongly, and assert versus
 * runtime checks.
 *
 *   parse_long()        the only correct way to use strtol: errno = 0 first, check endptr.
 *   read_header()       POSIX convention: return -1 and leave the reason in errno, and
 *                       preserve errno across cleanup calls that may overwrite it.
 *   open_for_write_*()  a real bug: a logging call between the failure and the errno read.
 *   withdraw()          assert() for programmer bugs, return codes for runtime conditions.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m10_errno_conventions m10_errno_conventions.c
 * Run:    ./m10_errno_conventions
 */
#define _POSIX_C_SOURCE 200809L   /* open, read, close, strerror_r (XSI version, returns int) */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- 1. strtol done right ----------------
 * Returns 0 and sets *out, or returns EINVAL (no digits, trailing junk) or ERANGE. */
static int parse_long(const char *s, long *out)
{
    char *end;
    errno = 0;                         /* strtol sets errno only on failure; clear it first */
    long v = strtol(s, &end, 10);
    if (end == s)
        return EINVAL;                 /* no digits at all: "", "abc", "-" */
    if (errno == ERANGE)
        return ERANGE;                 /* v is LONG_MAX or LONG_MIN, not the real number */
    while (*end == ' ' || *end == '\n')
        end++;
    if (*end != '\0')
        return EINVAL;                 /* "12abc": atoi would silently return 12 */
    *out = v;
    return 0;
}

/* ---------------- 2. return -1 + errno, and preserve errno during cleanup ---------------- */
static int read_header(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;                     /* errno already set by open() */
    ssize_t r = read(fd, buf, n);
    if (r < 0) {
        int saved = errno;             /* close() may change errno ... */
        close(fd);
        errno = saved;                 /* ... so put the real reason back */
        return -1;
    }
    close(fd);
    if ((size_t)r < n) {
        errno = EIO;                   /* short file: choose a code and document it */
        return -1;
    }
    return 0;
}

/* ---------------- 3. the errno-clobbering bug ---------------- */
static void log_to_file(const char *msg)
{
    /* Any library call may change errno, even on success. Here the log file cannot be
     * opened, so fopen sets errno = ENOENT. */
    FILE *f = fopen("/nonexistent-dir/app.log", "a");
    if (f) {
        fputs(msg, f);
        fclose(f);
    }
}

static int open_for_write_buggy(const char *path)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_to_file("open failed\n");  /* BUG: clobbers errno before the caller reads it */
        return -1;
    }
    return fd;
}

static int open_for_write_fixed(const char *path)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        int saved = errno;
        log_to_file("open failed\n");
        errno = saved;
        return -1;
    }
    return fd;
}

/* ---------------- 4. assert versus runtime checks ---------------- */
struct account {
    long long balance_paise;
};

enum withdraw_result { WD_OK = 0, WD_INSUFFICIENT = 1, WD_BAD_AMOUNT = 2 };

static enum withdraw_result withdraw(struct account *acc, long long amount)
{
    /* Programmer error: no correct caller ever passes NULL. assert documents and catches it
     * in debug and test builds. It disappears with -DNDEBUG, so it must never be the only
     * defence against bad INPUT. */
    assert(acc != NULL);

    /* Runtime conditions: these come from users and data. Always checked, in every build. */
    if (amount <= 0)
        return WD_BAD_AMOUNT;
    if (amount > acc->balance_paise)
        return WD_INSUFFICIENT;
    acc->balance_paise -= amount;
    return WD_OK;
}

/* Compile-time checks cost nothing at run time: use them for layout assumptions. */
_Static_assert(sizeof(long long) == 8, "money is stored in 64-bit paise");

int main(void)
{
    long v = 0;
    int rc;

    rc = parse_long("12345", &v);
    assert(rc == 0 && v == 12345);
    rc = parse_long("-42\n", &v);
    assert(rc == 0 && v == -42);
    rc = parse_long("12abc", &v);
    assert(rc == EINVAL);
    rc = parse_long("", &v);
    assert(rc == EINVAL);
    rc = parse_long("99999999999999999999999", &v);
    assert(rc == ERANGE);
    assert(atoi("12abc") == 12);       /* why atoi is banned in input handling */
    printf("parse_long(\"12abc\") -> %s; atoi(\"12abc\") -> %d\n", strerror(EINVAL), atoi("12abc"));

    char buf[16];
    rc = read_header("/no/such/file", buf, sizeof buf);
    assert(rc == -1 && errno == ENOENT);

    /* The directory "/" cannot be opened for writing: open() fails with EISDIR. */
    rc = open_for_write_buggy("/");
    int buggy_errno = errno;
    rc = open_for_write_fixed("/");
    int fixed_errno = errno;
    assert(rc == -1);
    assert(fixed_errno == EISDIR);
    assert(buggy_errno == ENOENT);     /* the wrong reason reaches the caller */
    char msg[128];
    int sr = strerror_r(buggy_errno, msg, sizeof msg);   /* XSI version: returns 0 or an error */
    assert(sr == 0);
    printf("buggy: errno says '%s'\n", msg);
    sr = strerror_r(fixed_errno, msg, sizeof msg);
    assert(sr == 0);
    printf("fixed: errno says '%s'\n", msg);

    struct account a = { .balance_paise = 10000 };
    enum withdraw_result wr = withdraw(&a, 2500);      /* call outside assert: it has effects */
    assert(wr == WD_OK && a.balance_paise == 7500);
    wr = withdraw(&a, 8000);
    assert(wr == WD_INSUFFICIENT && a.balance_paise == 7500);
    wr = withdraw(&a, -1);
    assert(wr == WD_BAD_AMOUNT);

    puts("all checks passed");
    return 0;
}
