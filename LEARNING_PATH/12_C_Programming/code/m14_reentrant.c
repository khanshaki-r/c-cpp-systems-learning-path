/* m14_reentrant.c - thread-safe is not the same as reentrant.
 *
 *   reentrant:   safe to call again before the previous call is finished, in the SAME thread
 *                (nested calls, recursion, a signal handler). Needs no hidden shared state.
 *   thread-safe: safe to call from several threads at the same time. Hidden state is
 *                allowed if it is locked or per-thread.
 *
 *   1. strtok keeps a hidden static cursor: nested tokenising breaks even with ONE thread.
 *      strtok_r takes the cursor as an argument.
 *   2. a function returning a static buffer: the second call overwrites the first result.
 *   3. a thread_local buffer makes it thread-safe but still NOT reentrant.
 *   4. localtime returns a pointer to one shared struct; localtime_r fills yours.
 *   5. errno is per thread.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -o m14_reentrant m14_reentrant.c
 * TSan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -pthread -o m14_reentrant_tsan m14_reentrant.c
 *         setarch $(uname -m) -R ./m14_reentrant_tsan
 * Run:    ./m14_reentrant
 */
#define _POSIX_C_SOURCE 200809L   /* strtok_r, localtime_r, pthread_barrier */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ---------------- 1. strtok vs strtok_r ---------------- */
/* Count "key=value" pairs in "a=1;b=2;c=3". The inner strtok call resets the hidden cursor,
 * so the outer loop loses its place after the first pair. */
static int count_pairs_strtok(char *s)
{
    int pairs = 0;
    for (char *item = strtok(s, ";"); item; item = strtok(NULL, ";")) {
        char *key = strtok(item, "=");
        char *val = strtok(NULL, "=");
        if (key && val)
            pairs++;
    }
    return pairs;
}

static int count_pairs_strtok_r(char *s)
{
    int pairs = 0;
    char *outer, *inner;
    for (char *item = strtok_r(s, ";", &outer); item; item = strtok_r(NULL, ";", &outer)) {
        char *key = strtok_r(item, "=", &inner);
        char *val = strtok_r(NULL, "=", &inner);
        if (key && val)
            pairs++;
    }
    return pairs;
}

/* ---------------- 2 and 3. result buffers ---------------- */
static const char *fmt_static(long paise)          /* neither thread-safe nor reentrant */
{
    static char buf[32];
    snprintf(buf, sizeof buf, "%ld.%02ld", paise / 100, paise % 100);
    return buf;
}

static const char *fmt_tls(long paise)             /* thread-safe, still not reentrant */
{
    static _Thread_local char buf[32];
    snprintf(buf, sizeof buf, "%ld.%02ld", paise / 100, paise % 100);
    return buf;
}

static char *fmt_r(long paise, char *buf, size_t n)  /* reentrant and thread-safe */
{
    snprintf(buf, n, "%ld.%02ld", paise / 100, paise % 100);
    return buf;
}

/* ---------------- 5. errno per thread ---------------- */
static pthread_barrier_t g_barrier;

struct errno_probe {
    int set_to;
    int seen_after_barrier;
    const char *tls_ptr;
};

static void *errno_worker(void *arg)
{
    struct errno_probe *p = arg;
    errno = p->set_to;
    p->tls_ptr = fmt_tls(100);                     /* address of THIS thread's buffer */
    pthread_barrier_wait(&g_barrier);              /* both threads have set errno now */
    p->seen_after_barrier = errno;
    return NULL;
}

int main(void)
{
    /* 1 */
    char s1[] = "a=1;b=2;c=3";
    char s2[] = "a=1;b=2;c=3";
    int bad = count_pairs_strtok(s1);
    int good = count_pairs_strtok_r(s2);
    printf("pairs counted with strtok: %d, with strtok_r: %d\n", bad, good);
    assert(bad == 1 && good == 3);

    /* 2 */
    const char *a = fmt_static(12345);
    const char *b = fmt_static(99);
    assert(a == b);                                /* the same buffer ... */
    assert(strcmp(a, "0.99") == 0);                /* ... so "a" now reads 0.99, not 123.45 */

    /* 3: within one thread the thread_local version has the same problem */
    a = fmt_tls(12345);
    b = fmt_tls(99);
    assert(a == b && strcmp(a, "0.99") == 0);

    /* the caller-buffer version is correct */
    char x[32], y[32];
    assert(strcmp(fmt_r(12345, x, sizeof x), "123.45") == 0);
    assert(strcmp(fmt_r(99, y, sizeof y), "0.99") == 0 && strcmp(x, "123.45") == 0);

    /* 4 */
    time_t t1 = 0, t2 = 86400;
    struct tm *p1 = localtime(&t1);
    struct tm *p2 = localtime(&t2);
    assert(p1 == p2);                              /* one shared struct for the process */
    struct tm r1, r2;
    localtime_r(&t1, &r1);
    localtime_r(&t2, &r2);
    assert(r2.tm_yday != r1.tm_yday || r2.tm_year != r1.tm_year);   /* two different days */

    /* 5 */
    int rc = pthread_barrier_init(&g_barrier, NULL, 2);
    assert(rc == 0);
    struct errno_probe pa = { EINVAL, 0, NULL }, pb = { ENOMEM, 0, NULL };
    pthread_t ta, tb;
    pthread_create(&ta, NULL, errno_worker, &pa);
    pthread_create(&tb, NULL, errno_worker, &pb);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    pthread_barrier_destroy(&g_barrier);
    assert(pa.seen_after_barrier == EINVAL && pb.seen_after_barrier == ENOMEM);
    assert(pa.tls_ptr != pb.tls_ptr);              /* thread_local: a buffer per thread */
    printf("errno per thread: %d and %d survived each other\n", pa.seen_after_barrier, pb.seen_after_barrier);

    puts("all checks passed");
    return 0;
}
