/* m14_pthreads_basics.c - the core POSIX threads API in one program:
 *
 *   1. pthread_create / pthread_join, passing arguments in and results out
 *   2. a mutex protecting a shared counter
 *   3. pthread_once for one-time initialisation
 *   4. a read-write lock protecting a table that is read often and written rarely
 *   5. a detached thread that reports completion through a semaphore
 *
 * pthread functions return an error NUMBER (0 on success); they do not set errno.
 * PT_CHECK turns any failure into a clear message and abort.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -o m14_pthreads_basics m14_pthreads_basics.c
 * TSan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -pthread -o m14_pthreads_basics_tsan m14_pthreads_basics.c
 *         setarch $(uname -m) -R ./m14_pthreads_basics_tsan
 * Run:    ./m14_pthreads_basics
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

#define PT_CHECK(call)                                                          \
    do {                                                                        \
        int rc_ = (call);                                                       \
        if (rc_ != 0) {                                                         \
            fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, __LINE__,       \
                    #call, strerror(rc_));                                      \
            abort();                                                            \
        }                                                                       \
    } while (0)

/* ---------------- 1. arguments in, results out ---------------- */
struct sum_job {
    const int *data;      /* in: shared, read-only, so no lock needed */
    size_t     begin;     /* in */
    size_t     end;       /* in */
    long long  result;    /* out: each thread writes only its own job */
};

static void *sum_worker(void *arg)
{
    struct sum_job *job = arg;
    long long s = 0;
    for (size_t i = job->begin; i < job->end; i++)
        s += job->data[i];
    job->result = s;
    return NULL;
}

/* The other way: return a heap pointer through pthread_join. The joiner must free it. */
static void *square_worker(void *arg)
{
    long *out = malloc(sizeof *out);
    if (out)
        *out = (long)(size_t)arg * (long)(size_t)arg;
    return out;
}

/* ---------------- 2. mutex ---------------- */
static pthread_mutex_t g_count_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_counter;

static void *inc_worker(void *arg)
{
    long n = *(const long *)arg;
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&g_count_lock);
        g_counter++;                     /* read-modify-write: must be atomic as a whole */
        pthread_mutex_unlock(&g_count_lock);
    }
    return NULL;
}

/* ---------------- 3. pthread_once ---------------- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_init_calls;                 /* written only inside init_fee_table: once guarantees it */
static int *g_fee_table;

static void init_fee_table(void)
{
    g_init_calls++;
    g_fee_table = malloc(4 * sizeof *g_fee_table);
    for (int i = 0; i < 4; i++)
        g_fee_table[i] = (i + 1) * 10;
}

static void *once_worker(void *arg)
{
    (void)arg;
    pthread_once(&g_once, init_fee_table);   /* every thread calls it; it runs once */
    return (void *)(size_t)g_fee_table[3];   /* safe: once() also publishes the writes */
}

/* ---------------- 4. read-write lock ---------------- */
static pthread_rwlock_t g_rates_lock = PTHREAD_RWLOCK_INITIALIZER;
static int g_rates[4] = { 100, 100, 100, 100 };   /* invariant: all four are equal */
static long g_torn_reads;
static pthread_mutex_t g_torn_lock = PTHREAD_MUTEX_INITIALIZER;

static void *rate_reader(void *arg)
{
    (void)arg;
    long torn = 0;
    for (int i = 0; i < 20000; i++) {
        pthread_rwlock_rdlock(&g_rates_lock);    /* many readers may hold this together */
        if (g_rates[0] != g_rates[1] || g_rates[1] != g_rates[2] || g_rates[2] != g_rates[3])
            torn++;
        pthread_rwlock_unlock(&g_rates_lock);
    }
    pthread_mutex_lock(&g_torn_lock);
    g_torn_reads += torn;
    pthread_mutex_unlock(&g_torn_lock);
    return NULL;
}

static void *rate_writer(void *arg)
{
    (void)arg;
    for (int v = 101; v <= 300; v++) {
        pthread_rwlock_wrlock(&g_rates_lock);    /* exclusive: no reader sees a half update */
        for (int i = 0; i < 4; i++)
            g_rates[i] = v;
        pthread_rwlock_unlock(&g_rates_lock);
    }
    return NULL;
}

/* ---------------- 5. detached thread ---------------- */
static sem_t g_done;
static int g_audit_written;

static void *audit_worker(void *arg)
{
    (void)arg;
    g_audit_written = 1;
    sem_post(&g_done);                   /* nobody will join us: signal completion instead */
    return NULL;
}

int main(void)
{
    /* 1 */
    enum { N = 1000000, T = 4 };
    int *data = malloc(N * sizeof *data);
    assert(data != NULL);
    for (int i = 0; i < N; i++)
        data[i] = i % 7;
    pthread_t th[T];
    struct sum_job jobs[T];
    for (int t = 0; t < T; t++) {
        jobs[t] = (struct sum_job){ data, (size_t)t * N / T, (size_t)(t + 1) * N / T, 0 };
        PT_CHECK(pthread_create(&th[t], NULL, sum_worker, &jobs[t]));
    }
    long long total = 0;
    for (int t = 0; t < T; t++) {
        PT_CHECK(pthread_join(th[t], NULL));   /* join = wait + the results are now visible */
        total += jobs[t].result;
    }
    long long expect = 0;
    for (int i = 0; i < N; i++)
        expect += data[i];
    assert(total == expect);
    free(data);
    printf("parallel sum over %d threads: %lld\n", T, total);

    pthread_t sq;
    PT_CHECK(pthread_create(&sq, NULL, square_worker, (void *)(size_t)12));
    void *ret;
    PT_CHECK(pthread_join(sq, &ret));
    assert(ret != NULL && *(long *)ret == 144);
    free(ret);

    /* 2 */
    long per_thread = 100000;
    for (int t = 0; t < T; t++)
        PT_CHECK(pthread_create(&th[t], NULL, inc_worker, &per_thread));
    for (int t = 0; t < T; t++)
        PT_CHECK(pthread_join(th[t], NULL));
    assert(g_counter == T * per_thread);
    printf("mutex counter: %ld (expected %ld)\n", g_counter, T * per_thread);

    /* 3 */
    for (int t = 0; t < T; t++)
        PT_CHECK(pthread_create(&th[t], NULL, once_worker, NULL));
    for (int t = 0; t < T; t++) {
        PT_CHECK(pthread_join(th[t], &ret));
        assert((size_t)ret == 40);
    }
    assert(g_init_calls == 1);
    free(g_fee_table);

    /* 4 */
    pthread_t readers[3], writer;
    for (int i = 0; i < 3; i++)
        PT_CHECK(pthread_create(&readers[i], NULL, rate_reader, NULL));
    PT_CHECK(pthread_create(&writer, NULL, rate_writer, NULL));
    for (int i = 0; i < 3; i++)
        PT_CHECK(pthread_join(readers[i], NULL));
    PT_CHECK(pthread_join(writer, NULL));
    assert(g_torn_reads == 0 && g_rates[0] == 300);
    printf("rwlock: 60000 reads during 200 updates, torn reads = %ld\n", g_torn_reads);

    /* 5 */
    int rc = sem_init(&g_done, 0, 0);    /* sem_* DO use errno, unlike pthread_* */
    assert(rc == 0);
    pthread_attr_t attr;
    pthread_t det;
    PT_CHECK(pthread_attr_init(&attr));
    PT_CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
    PT_CHECK(pthread_create(&det, &attr, audit_worker, NULL));
    PT_CHECK(pthread_attr_destroy(&attr));
    while (sem_wait(&g_done) != 0)       /* sem_wait can fail with EINTR */
        ;
    assert(g_audit_written == 1);        /* sem_post/sem_wait order the memory too */
    sem_destroy(&g_done);

    puts("all checks passed");
    return 0;
}
