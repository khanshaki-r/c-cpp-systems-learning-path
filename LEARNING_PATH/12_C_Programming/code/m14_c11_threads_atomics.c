/* m14_c11_threads_atomics.c - the portable C11 layer: <threads.h> and <stdatomic.h>.
 *
 *   1. thrd_create / thrd_join, mtx_t, cnd_t with a predicate loop, call_once
 *   2. atomic counter with memory_order_relaxed (only the count matters)
 *   3. a spinlock built from atomic_flag (acquire on lock, release on unlock)
 *   4. message passing: plain data published with a release store, read after an acquire
 *      load (the pattern behind every lock-free queue)
 *   5. thread_local storage
 *
 * glibc has shipped <threads.h> since 2.28; MSVC since Visual Studio 2022 17.8.
 * pthreads remain more complete (no rwlock, no timed condvar on a monotonic clock in C11).
 *
 * TSan LIMITATION (verified with GCC 13.3 and clang 18.1 on glibc 2.39): a TSan build that
 * uses glibc's <threads.h> crashes with "SEGV on unknown address 0x18". glibc's thrd_create,
 * mtx_lock and cnd_wait call glibc's internal pthread functions directly, so TSan's
 * interceptors never see the new thread or the locks. To race-check this code, build with
 * -DUSE_PTHREADS: the small shim below maps the C11 names onto pthreads, which TSan does
 * intercept. (The same kind of shim is how C11-threads code is ported to old platforms.)
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -o m14_c11_threads_atomics m14_c11_threads_atomics.c
 * TSan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -pthread -DUSE_PTHREADS -o m14_c11_tsan m14_c11_threads_atomics.c
 *         setarch $(uname -m) -R ./m14_c11_tsan
 * Run:    ./m14_c11_threads_atomics
 */
#ifdef USE_PTHREADS
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#ifndef USE_PTHREADS
#include <threads.h>
#else
/* ---- minimal <threads.h> shim over pthreads: only what this file uses ---- */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
typedef pthread_t       thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t  cnd_t;
typedef pthread_once_t  once_flag;
typedef int (*thrd_start_t)(void *);
enum { thrd_success = 0, thrd_error = 2 };
enum { mtx_plain = 0 };
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#define thread_local   _Thread_local

struct thrd_tramp { thrd_start_t fn; void *arg; };
static void *thrd_tramp_main(void *p)
{
    struct thrd_tramp t = *(struct thrd_tramp *)p;
    free(p);
    return (void *)(intptr_t)t.fn(t.arg);      /* C11 threads return int, pthreads void* */
}
static int thrd_create(thrd_t *th, thrd_start_t fn, void *arg)
{
    struct thrd_tramp *t = malloc(sizeof *t);
    if (!t) return thrd_error;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(th, NULL, thrd_tramp_main, t) != 0) { free(t); return thrd_error; }
    return thrd_success;
}
static int thrd_join(thrd_t th, int *res)
{
    void *r;
    if (pthread_join(th, &r) != 0) return thrd_error;
    if (res) *res = (int)(intptr_t)r;
    return thrd_success;
}
static void thrd_yield(void)                     { sched_yield(); }
static int  mtx_init(mtx_t *m, int type)         { (void)type; return pthread_mutex_init(m, NULL); }
static int  mtx_lock(mtx_t *m)                   { return pthread_mutex_lock(m); }
static int  mtx_unlock(mtx_t *m)                 { return pthread_mutex_unlock(m); }
static void mtx_destroy(mtx_t *m)                { pthread_mutex_destroy(m); }
static int  cnd_init(cnd_t *c)                   { return pthread_cond_init(c, NULL); }
static int  cnd_signal(cnd_t *c)                 { return pthread_cond_signal(c); }
static int  cnd_wait(cnd_t *c, mtx_t *m)         { return pthread_cond_wait(c, m); }
static void cnd_destroy(cnd_t *c)                { pthread_cond_destroy(c); }
static void call_once(once_flag *f, void (*fn)(void)) { pthread_once(f, fn); }
#endif

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

enum { T = 4, ITERS = 100000 };

/* ---------------- 1. mtx_t + cnd_t + call_once ---------------- */
static mtx_t     g_mtx;
static cnd_t     g_cnd;
static int       g_ready_workers;            /* protected by g_mtx */
static once_flag g_once = ONCE_FLAG_INIT;
static int       g_once_runs;

static void init_once(void)
{
    g_once_runs++;
    mtx_init(&g_mtx, mtx_plain);
    cnd_init(&g_cnd);
}

static int checkin_worker(void *arg)
{
    (void)arg;
    call_once(&g_once, init_once);
    mtx_lock(&g_mtx);
    g_ready_workers++;
    cnd_signal(&g_cnd);
    mtx_unlock(&g_mtx);
    return 0;
}

/* ---------------- 2. relaxed atomic counter ---------------- */
static atomic_long g_hits;

static int hit_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++)
        atomic_fetch_add_explicit(&g_hits, 1, memory_order_relaxed);
    return 0;
}

/* ---------------- 3. spinlock from atomic_flag ---------------- */
static atomic_flag g_spin = ATOMIC_FLAG_INIT;
static long        g_spin_protected;          /* plain variable guarded by the spinlock */

static void spin_lock(atomic_flag *f)
{
    /* acquire: everything the previous owner wrote before its release is visible to us */
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire))
        thrd_yield();                          /* be polite; a real spinlock would pause */
}

static void spin_unlock(atomic_flag *f)
{
    atomic_flag_clear_explicit(f, memory_order_release);
}

static int spin_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        spin_lock(&g_spin);
        g_spin_protected++;
        spin_unlock(&g_spin);
    }
    return 0;
}

/* ---------------- 4. release/acquire message passing ---------------- */
struct message {
    int  account;
    long amount;
};
static struct message g_msg;                   /* plain, non-atomic payload */
static atomic_int     g_msg_ready;

static int publisher(void *arg)
{
    (void)arg;
    g_msg.account = 1001;                      /* 1. write the payload */
    g_msg.amount = 250000;
    atomic_store_explicit(&g_msg_ready, 1, memory_order_release);  /* 2. publish */
    return 0;
}

static int subscriber(void *arg)
{
    struct message *out = arg;
    while (!atomic_load_explicit(&g_msg_ready, memory_order_acquire))
        thrd_yield();                          /* 3. wait for the flag ... */
    *out = g_msg;                              /* 4. ... then the payload is guaranteed visible */
    return 0;
}

/* ---------------- 5. thread_local ---------------- */
static thread_local int tls_calls;            /* one copy per thread, zero-initialised */

static int tls_worker(void *arg)
{
    int n = *(int *)arg;
    for (int i = 0; i < n; i++)
        tls_calls++;                           /* no lock: nobody else can see this copy */
    return tls_calls;                          /* thread's result via thrd_join */
}

int main(void)
{
    thrd_t th[T];
    int rc;

    /* 1 */
    call_once(&g_once, init_once);
    for (int i = 0; i < T; i++) {
        rc = thrd_create(&th[i], checkin_worker, NULL);
        assert(rc == thrd_success);
    }
    mtx_lock(&g_mtx);
    while (g_ready_workers < T)                /* predicate loop, same rule as pthreads */
        cnd_wait(&g_cnd, &g_mtx);
    mtx_unlock(&g_mtx);
    for (int i = 0; i < T; i++)
        thrd_join(th[i], NULL);
    assert(g_once_runs == 1);

    /* 2 */
    for (int i = 0; i < T; i++)
        thrd_create(&th[i], hit_worker, NULL);
    for (int i = 0; i < T; i++)
        thrd_join(th[i], NULL);
    assert(atomic_load(&g_hits) == (long)T * ITERS);

    /* 3 */
    for (int i = 0; i < T; i++)
        thrd_create(&th[i], spin_worker, NULL);
    for (int i = 0; i < T; i++)
        thrd_join(th[i], NULL);
    assert(g_spin_protected == (long)T * ITERS);
    printf("atomic counter = %ld, spinlock counter = %ld\n", atomic_load(&g_hits), g_spin_protected);

    /* 4 */
    struct message got = { 0, 0 };
    thrd_t pub, sub;
    thrd_create(&sub, subscriber, &got);
    thrd_create(&pub, publisher, NULL);
    thrd_join(pub, NULL);
    thrd_join(sub, NULL);
    assert(got.account == 1001 && got.amount == 250000);
    printf("message passed with release/acquire: account %d amount %ld\n", got.account, got.amount);

    /* 5 */
    int counts[T] = { 10, 20, 30, 40 };
    for (int i = 0; i < T; i++)
        thrd_create(&th[i], tls_worker, &counts[i]);
    for (int i = 0; i < T; i++) {
        int res;
        thrd_join(th[i], &res);
        assert(res == counts[i]);              /* each thread counted only its own calls */
    }
    assert(tls_calls == 0);                    /* main's copy was never touched */

    mtx_destroy(&g_mtx);
    cnd_destroy(&g_cnd);
    puts("all checks passed");
    return 0;
}
