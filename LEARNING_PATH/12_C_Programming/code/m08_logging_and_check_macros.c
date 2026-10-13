/* m08_logging_and_check_macros.c - logging macros with __FILE__/__LINE__/__func__, and a CHECK macro
 * for goto-cleanup error handling.
 *
 * Module: 12_C_Programming/08_the_preprocessor_and_macros.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m08_logging_and_check_macros.c -o m08_logging_and_check_macros
 * Strip debug logs at compile time: add -DLOG_COMPILED_MIN=1
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum log_level { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };
static const char *const k_level[] = {"DEBUG", "INFO", "WARN", "ERROR"};

#ifndef LOG_COMPILED_MIN
#define LOG_COMPILED_MIN LOG_DEBUG   /* levels below this vanish from the binary entirely */
#endif

static enum log_level g_runtime_min = LOG_INFO;   /* can be changed while running */
static int g_lines_written;

/* __FILE__ is the path as given to the compiler: keep only the last component. */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash > slash) slash = bslash;
    return slash ? slash + 1 : path;
}

__attribute__((format(printf, 5, 6)))
static void log_write(enum log_level lvl, const char *file, int line, const char *func, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);          /* format once, then emit in one call */
    va_end(ap);
    printf("%-5s %s:%d %s(): %s\n", k_level[lvl], base_name(file), line, func, msg);
    ++g_lines_written;
}

/* The format string travels inside __VA_ARGS__, so LOG(LOG_INFO, "x") has one variadic
 * argument and LOG(LOG_INFO, "x=%d", x) has two. Portable C17: no ", ##__VA_ARGS__" needed.
 * The level test happens BEFORE the arguments are evaluated: a filtered-out debug line costs
 * one comparison. The constant LOG_COMPILED_MIN test lets the compiler delete the call. */
#define LOG(lvl, ...)                                                                   \
    do {                                                                                \
        if ((lvl) >= LOG_COMPILED_MIN && (lvl) >= g_runtime_min)                        \
            log_write((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);                \
    } while (0)

#define LOG_DEBUGF(...) LOG(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFOF(...)  LOG(LOG_INFO, __VA_ARGS__)
#define LOG_ERRORF(...) LOG(LOG_ERROR, __VA_ARGS__)

/* CHECK: on failure, record the code, log the failing expression as text, jump to cleanup.
 * Requires a local `int rc` and a `cleanup:` label in the calling function. */
#define CHECK(cond, code)                                                               \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            rc = (code);                                                                \
            LOG_ERRORF("check failed: %s -> rc=%d", #cond, rc);                         \
            goto cleanup;                                                               \
        }                                                                               \
    } while (0)

/* The goto-cleanup shape: every resource is released exactly once, on every path. */
static int load_customer_file(const char *path, size_t want_bytes)
{
    int rc = 0;
    FILE *f = NULL;
    char *buf = NULL;

    LOG_INFOF("loading %s (%zu bytes)", path, want_bytes);
    buf = malloc(want_bytes);
    CHECK(buf != NULL, -1);
    f = fopen(path, "rb");
    CHECK(f != NULL, -2);
    CHECK(fread(buf, 1, want_bytes, f) == want_bytes, -3);
    LOG_INFOF("loaded %s", path);

cleanup:
    if (f != NULL) fclose(f);
    free(buf);
    return rc;
}

int main(void)
{
    LOG_DEBUGF("hidden: runtime level is INFO");      /* filtered: arguments never evaluated */
    LOG_INFOF("server starting, pid file %s", "/run/bank.pid");
    g_runtime_min = LOG_DEBUG;
    LOG_DEBUGF("debug now visible, %d workers", 8);

    int rc = load_customer_file("/nonexistent/customers.dat", 64);
    assert(rc == -2);
    printf("load_customer_file returned %d; %d log lines written\n", rc, g_lines_written);
    assert(g_lines_written == (LOG_COMPILED_MIN <= LOG_DEBUG ? 4 : 3));   /* 3 when DEBUG is compiled out */
    return 0;
}
