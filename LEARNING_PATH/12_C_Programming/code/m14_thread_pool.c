/* m14_thread_pool.c - a small fixed-size thread pool in C: a linked-list task queue, one
 * mutex, two condition variables, wait-for-idle, and an orderly shutdown.
 * The C++ version is 04_Concurrency/code/m09_thread_pool.hpp; compare how much of this file
 * is cleanup that RAII would do for you.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -o m14_thread_pool m14_thread_pool.c
 * TSan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=thread -pthread -o m14_thread_pool_tsan m14_thread_pool.c
 *         setarch $(uname -m) -R ./m14_thread_pool_tsan
 * Run:    ./m14_thread_pool
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

typedef void (*task_fn)(void *arg);

struct task {
    task_fn      fn;
    void        *arg;
    struct task *next;
};

struct pool {
    pthread_mutex_t mu;
    pthread_cond_t  has_work;     /* workers wait here */
    pthread_cond_t  idle;         /* pool_wait() waits here */
    struct task    *head, *tail;
    size_t          pending;      /* queued + running */
    int             stopping;
    pthread_t      *threads;
    size_t          nthreads;
};

static void *worker_main(void *arg)
{
    struct pool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->stopping)
            pthread_cond_wait(&p->has_work, &p->mu);
        if (!p->head) {                     /* stopping and nothing left */
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        struct task *t = p->head;
        p->head = t->next;
        if (!p->head)
            p->tail = NULL;
        pthread_mutex_unlock(&p->mu);

        t->fn(t->arg);                      /* run WITHOUT the lock: tasks may be slow */
        free(t);

        pthread_mutex_lock(&p->mu);
        if (--p->pending == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->mu);
    }
}

static void pool_destroy(struct pool *p);

/* Returns 0 and sets *out, or an error number. On failure every thread already started is
 * stopped and joined: a half-built pool must never leak running threads. */
static int pool_create(struct pool **out, size_t nthreads)
{
    struct pool *p = calloc(1, sizeof *p);
    if (!p)
        return ENOMEM;
    p->threads = calloc(nthreads, sizeof *p->threads);
    if (!p->threads) {
        free(p);
        return ENOMEM;
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->has_work, NULL);
    pthread_cond_init(&p->idle, NULL);
    for (size_t i = 0; i < nthreads; i++) {
        int rc = pthread_create(&p->threads[i], NULL, worker_main, p);
        if (rc != 0) {
            p->nthreads = i;                /* only join the ones that exist */
            pool_destroy(p);
            return rc;
        }
    }
    p->nthreads = nthreads;
    *out = p;
    return 0;
}

static int pool_submit(struct pool *p, task_fn fn, void *arg)
{
    struct task *t = malloc(sizeof *t);     /* allocate outside the lock */
    if (!t)
        return ENOMEM;
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->stopping) {
        pthread_mutex_unlock(&p->mu);
        free(t);
        return ECANCELED;
    }
    if (p->tail)
        p->tail->next = t;
    else
        p->head = t;
    p->tail = t;
    p->pending++;
    pthread_cond_signal(&p->has_work);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

/* Block until every submitted task has finished. */
static void pool_wait(struct pool *p)
{
    pthread_mutex_lock(&p->mu);
    while (p->pending > 0)
        pthread_cond_wait(&p->idle, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

/* Drain: queued tasks still run, then the workers exit and are joined. */
static void pool_destroy(struct pool *p)
{
    pthread_mutex_lock(&p->mu);
    p->stopping = 1;
    pthread_cond_broadcast(&p->has_work);
    pthread_mutex_unlock(&p->mu);
    for (size_t i = 0; i < p->nthreads; i++)
        pthread_join(p->threads[i], NULL);
    pthread_cond_destroy(&p->idle);
    pthread_cond_destroy(&p->has_work);
    pthread_mutex_destroy(&p->mu);
    free(p->threads);
    free(p);
}

/* ---------------- test workload ---------------- */
struct square_job {
    long input;
    long output;            /* each task writes only its own job: no lock needed */
};

static void square_task(void *arg)
{
    struct square_job *j = arg;
    j->output = j->input * j->input;
}

static atomic_long g_hits;

static void count_task(void *arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&g_hits, 1, memory_order_relaxed);   /* a counter needs no ordering */
}

int main(void)
{
    struct pool *p = NULL;
    int rc = pool_create(&p, 4);
    assert(rc == 0);

    enum { JOBS = 10000 };
    struct square_job *jobs = malloc(JOBS * sizeof *jobs);
    assert(jobs != NULL);
    for (long i = 0; i < JOBS; i++) {
        jobs[i].input = i;
        jobs[i].output = -1;
        rc = pool_submit(p, square_task, &jobs[i]);
        assert(rc == 0);
    }
    pool_wait(p);                           /* after this, every output is visible to us */
    for (long i = 0; i < JOBS; i++)
        assert(jobs[i].output == i * i);
    printf("%d square tasks done on 4 workers\n", JOBS);

    for (int i = 0; i < 50000; i++) {
        rc = pool_submit(p, count_task, NULL);
        assert(rc == 0);
    }
    pool_destroy(p);                        /* drains the queue before joining */
    assert(atomic_load(&g_hits) == 50000);
    printf("50000 counter tasks drained at shutdown: hits = %ld\n", atomic_load(&g_hits));

    free(jobs);
    puts("all checks passed");
    return 0;
}
