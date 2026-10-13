/* m15_diagnostics.c - the diagnostics you add to legacy C code before (or instead of)
 * attaching a debugger, especially on a customer machine where you cannot:
 *
 *   LOG(level, fmt, ...)   one line per event: UTC time, thread, file:line, function, message
 *   TRACE(what, a, b)      a cheap in-memory ring of the last 64 events, per thread; dumped
 *                          when something goes wrong, so you see what led up to it
 *   CHECK(cond, fmt, ...)  a "soft assert" for production: logs the failed condition with
 *                          context and the trace, then lets the caller return an error
 *   hexdump()              the exact bytes of a bad message, for the bug report
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m15_diagnostics m15_diagnostics.c
 * Run:    ./m15_diagnostics
 */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, gmtime_r, open_memstream */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- logging ---------------- */
static FILE *g_log;                          /* stderr by default; the test captures it */
static atomic_int g_next_tid = 1;
static _Thread_local int t_tid;              /* small stable ids read better than pthread_t */

static int my_tid(void)
{
    if (t_tid == 0)
        t_tid = atomic_fetch_add(&g_next_tid, 1);
    return t_tid;
}

static void log_write(const char *level, const char *file, int line, const char *func,
                      const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char when[32];
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);                /* gmtime_r: the reentrant version */
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* One fprintf per line: stdio locks the FILE per call, so lines from different
     * threads do not interleave mid-line. */
    fprintf(g_log ? g_log : stderr, "%s.%03ldZ %-5s t%d %s:%d %s(): %s\n", when,
            ts.tv_nsec / 1000000L, level, my_tid(), file, line, func, msg);
}

#define LOG(level, ...) log_write(#level, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* ---------------- trace ring ---------------- */
struct trace_ev {
    unsigned long seq;
    const char   *what;                      /* always a string literal: no copy, no lifetime issue */
    long          a, b;
};

#define TRACE_N 64u
static _Thread_local struct trace_ev t_ring[TRACE_N];
static _Thread_local unsigned long   t_seq;

/* Macro parameter names end in an underscore on purpose. With TRACE(what, a, b) the
 * preprocessor would also replace the member names in ev_->what, ev_->a and ev_->b with
 * the arguments - a real bug this file had in its first draft. */
#define TRACE(what_, a_, b_)                                                   \
    do {                                                                       \
        struct trace_ev *ev_ = &t_ring[t_seq % TRACE_N];                       \
        ev_->seq = t_seq++;                                                    \
        ev_->what = (what_);                                                   \
        ev_->a = (long)(a_);                                                   \
        ev_->b = (long)(b_);                                                   \
    } while (0)

static void trace_dump(FILE *out, unsigned max)
{
    unsigned long n = t_seq < TRACE_N ? t_seq : TRACE_N;
    if (n > max)
        n = max;
    fprintf(out, "  last %lu trace events of t%d (oldest first):\n", n, my_tid());
    for (unsigned long s = t_seq - n; s < t_seq; s++) {
        const struct trace_ev *ev = &t_ring[s % TRACE_N];
        fprintf(out, "    #%lu %-10s a=%ld b=%ld\n", ev->seq, ev->what, ev->a, ev->b);
    }
}

/* ---------------- hexdump ---------------- */
static void hexdump(FILE *out, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t off = 0; off < n; off += 16) {
        fprintf(out, "  %08zx ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < n)
                fprintf(out, " %02x", p[off + i]);
            else
                fputs("   ", out);
        }
        fputs("  |", out);
        for (size_t i = 0; i < 16 && off + i < n; i++) {
            unsigned char c = p[off + i];
            fputc(c >= 0x20 && c < 0x7f ? c : '.', out);
        }
        fputs("|\n", out);
    }
}

/* ---------------- soft assert ---------------- */
static unsigned long g_check_failures;

static void check_failed(const char *cond, const char *file, int line, const char *func,
                         const char *fmt, ...)
{
    char ctx[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx, sizeof ctx, fmt, ap);
    va_end(ap);
    g_check_failures++;
    log_write("ERROR", file, line, func, "check failed: %s (%s)", cond, ctx);
    trace_dump(g_log ? g_log : stderr, 8);
}

/* Evaluates to 1 if cond holds, else logs everything and evaluates to 0. */
#define CHECK(cond, ...) \
    ((cond) ? 1 : (check_failed(#cond, __FILE__, __LINE__, __func__, __VA_ARGS__), 0))

/* ---------------- the code being diagnosed ----------------
 * Message: [u8 type][u16 payload length, little-endian][payload]. */
static int process_message(const unsigned char *msg, size_t len)
{
    TRACE("recv", len, msg[0]);
    if (!CHECK(len >= 3, "len=%zu", len))
        return -1;
    size_t declared = (size_t)msg[1] | ((size_t)msg[2] << 8);
    TRACE("header", msg[0], declared);
    if (!CHECK(declared == len - 3, "type=%u declared=%zu actual=%zu", msg[0], declared, len - 3)) {
        hexdump(g_log, msg, len);
        return -1;
    }
    TRACE("ok", msg[0], declared);
    return 0;
}

int main(void)
{
    char *captured = NULL;
    size_t captured_len = 0;
    g_log = open_memstream(&captured, &captured_len);   /* capture the log to test it */
    assert(g_log != NULL);

    LOG(INFO, "server starting, %d workers", 4);
    const unsigned char good1[] = { 1, 4, 0, 'P', 'I', 'N', 'G' };
    const unsigned char good2[] = { 1, 2, 0, 'O', 'K' };
    const unsigned char bad[] = { 2, 0xff, 0, 'A', 'C', 'C', '-', '1', '0', '0', '1' };
    int r1 = process_message(good1, sizeof good1);
    int r2 = process_message(good2, sizeof good2);
    int r3 = process_message(bad, sizeof bad);
    fclose(g_log);
    g_log = NULL;

    assert(r1 == 0 && r2 == 0 && r3 == -1 && g_check_failures == 1);
    assert(strstr(captured, "INFO  t1 ") != NULL);
    assert(strstr(captured, "check failed: declared == len - 3 (type=2 declared=255 actual=8)") != NULL);
    assert(strstr(captured, "#6 recv       a=11 b=2") != NULL);
    assert(strstr(captured, "  00000000  02 ff 00 41 43 43 2d 31 30 30 31") != NULL);
    assert(strstr(captured, "|...ACC-1001|") != NULL);
    assert(t_seq == 8);                                  /* 3 + 3 + 2 events */

    fputs(captured, stdout);
    free(captured);
    puts("all checks passed");
    return 0;
}
