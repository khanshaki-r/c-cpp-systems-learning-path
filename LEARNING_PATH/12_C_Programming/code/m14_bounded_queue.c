/* m14_bounded_queue.c - a bounded blocking queue: one mutex, two condition variables,
 * predicate loops, a timed pop, and a close() that wakes everybody for shutdown.
 * This is the heart of every producer/consumer server (the acceptor thread produces
 * connections, the worker threads consume them).
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -o m14_bounded_queue m14_bounded_queue.c
 * TSan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -pthread -o m14_bounded_queue_tsan m14_bounded_queue.c
 *         setarch $(uname -m) -R ./m14_bounded_queue_tsan
 * Run:    ./m14_bounded_queue
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

struct bq {
    int            *buf;
    size_t          cap, head, count;
    int             closed;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;   /* signalled when count goes up */
    pthread_cond_t  not_full;    /* signalled when count goes down */
};

static int bq_init(struct bq *q, size_t cap)
{
    q->buf = malloc(cap * sizeof *q->buf);
    if (!q->buf)
        return ENOMEM;
    q->cap = cap;
    q->head = q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_full, NULL);
    /* Timed waits measure against CLOCK_REALTIME by default, which jumps when an admin or
     * NTP changes the wall clock. Use the monotonic clock for timeouts. */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q->not_empty, &ca);
    pthread_condattr_destroy(&ca);
    return 0;
}

static void bq_destroy(struct bq *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
}

/* Blocks while full. Returns 0, or -1 if the queue was closed. */
static int bq_push(struct bq *q, int v)
{
    pthread_mutex_lock(&q->mu);
    /* WHILE, not if: (1) spurious wakeups are allowed by POSIX; (2) another producer may
     * have taken the free slot between the signal and our wakeup. Re-check the predicate. */
    while (q->count == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mu);   /* atomically: unlock, sleep, relock */
    if (q->closed) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->buf[(q->head + q->count) % q->cap] = v;
    q->count++;
    pthread_cond_signal(&q->not_empty);            /* one item -> wake one consumer */
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* Blocks while empty. Returns 0, or -1 once the queue is closed AND drained. */
static int bq_pop(struct bq *q, int *out)
{
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->count == 0) {                           /* closed and nothing left */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* Like bq_pop but gives up after timeout_ms. Returns 0, -1 (closed) or ETIMEDOUT. */
static int bq_pop_timed(struct bq *q, int *out, long timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);     /* absolute deadline, computed once */
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) {
        int rc = pthread_cond_timedwait(&q->not_empty, &q->mu, &deadline);
        if (rc == ETIMEDOUT && q->count == 0 && !q->closed) {
            pthread_mutex_unlock(&q->mu);
            return ETIMEDOUT;
        }
    }
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* Shutdown: set the flag under the lock and wake EVERY waiter (broadcast, not signal). */
static void bq_close(struct bq *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

/* ---------------- test ---------------- */
enum { PRODUCERS = 4, CONSUMERS = 4, PER_PRODUCER = 20000 };

struct consumer_result {
    struct bq *q;
    long long  sum;
    long       items;
};

static void *producer(void *arg)
{
    struct bq *q = arg;
    for (int i = 1; i <= PER_PRODUCER; i++)
        if (bq_push(q, i) != 0)
            break;
    return NULL;
}

static void *consumer(void *arg)
{
    struct consumer_result *r = arg;
    int v;
    while (bq_pop(r->q, &v) == 0) {
        r->sum += v;
        r->items++;
    }
    return NULL;
}

static double ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e3 + (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(void)
{
    struct bq q;
    int rc = bq_init(&q, 16);            /* small on purpose: producers block often */
    assert(rc == 0);

    pthread_t prod[PRODUCERS], cons[CONSUMERS];
    struct consumer_result res[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        res[i] = (struct consumer_result){ &q, 0, 0 };
        rc = pthread_create(&cons[i], NULL, consumer, &res[i]);
        assert(rc == 0);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        rc = pthread_create(&prod[i], NULL, producer, &q);
        assert(rc == 0);
    }
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    bq_close(&q);                        /* consumers drain what is left, then exit */
    long long sum = 0;
    long items = 0;
    for (int i = 0; i < CONSUMERS; i++) {
        pthread_join(cons[i], NULL);
        sum += res[i].sum;
        items += res[i].items;
    }
    long long expect = (long long)PRODUCERS * PER_PRODUCER * (PER_PRODUCER + 1) / 2;
    assert(items == (long)PRODUCERS * PER_PRODUCER && sum == expect);
    printf("%d producers -> %d consumers: %ld items, sum %lld (expected %lld)\n",
           PRODUCERS, CONSUMERS, items, sum, expect);

    int v;
    rc = bq_push(&q, 1);
    assert(rc == -1);                    /* closed queue refuses new work */
    bq_destroy(&q);

    /* timed pop on an empty, open queue */
    rc = bq_init(&q, 4);
    assert(rc == 0);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = bq_pop_timed(&q, &v, 50);
    double waited = ms_since(&t0);
    assert(rc == ETIMEDOUT && waited >= 45.0);
    printf("timed pop on empty queue: ETIMEDOUT, waited at least 45 ms\n");
    rc = bq_push(&q, 7);
    assert(rc == 0);
    rc = bq_pop_timed(&q, &v, 50);
    assert(rc == 0 && v == 7);
    bq_destroy(&q);

    puts("all checks passed");
    return 0;
}
