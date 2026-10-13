/*
 * m13_win32_srwlock_condvar.c -- WINDOWS-ONLY (Vista or later). Not part of the WSL build.
 *
 * 1. The module 05 bounded producer-consumer queue in C with SRWLOCK + CONDITION_VARIABLE,
 *    including the same close-and-drain shutdown protocol. Compare line by line with
 *    BoundedQueue in m05_producer_consumer.cpp: it is the same program.
 * 2. A WaitForMultipleObjects "shutdown OR work" loop, with the shutdown event first so it wins.
 * 3. InitOnceExecuteOnce, the Win32 spelling of std::call_once.
 *
 * Build (MSVC developer prompt):  cl /W4 /O2 m13_win32_srwlock_condvar.c
 * Build (MinGW-w64 GCC):          gcc -std=c11 -Wall -Wextra -O2 m13_win32_srwlock_condvar.c -o m13_cv.exe
 * The legacy mingw.org GCC 6.3 in C:\MinGW has headers too old for INIT_ONCE: use MSVC or MinGW-w64
 * (MSYS2 "ucrt64" or "mingw64" toolchain). The SRWLOCK and CONDITION_VARIABLE parts are fine there.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600   /* SRWLOCK, CONDITION_VARIABLE, INIT_ONCE need Vista+ headers */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------------------------
 * Part 1: bounded queue
 * ------------------------------------------------------------------------------------------- */
#define QCAP 64

typedef struct {
    SRWLOCK lock;                         /* std::mutex m_;                      */
    CONDITION_VARIABLE not_empty;         /* std::condition_variable not_empty_; */
    CONDITION_VARIABLE not_full;          /* std::condition_variable not_full_;  */
    int buf[QCAP]; size_t head, tail, count;
    BOOL closed;                          /* bool closed_ = false;               */
} Queue;

static void queue_init(Queue* q) {
    InitializeSRWLock(&q->lock);          /* or = SRWLOCK_INIT; nothing to delete later */
    InitializeConditionVariable(&q->not_empty);
    InitializeConditionVariable(&q->not_full);
    q->head = q->tail = q->count = 0;
    q->closed = FALSE;
}

/* Returns FALSE if the queue is closed (the item is not accepted). */
static BOOL queue_push(Queue* q, int v) {
    AcquireSRWLockExclusive(&q->lock);
    while (q->count == QCAP && !q->closed)                     /* predicate loop: spurious wakeups */
        SleepConditionVariableSRW(&q->not_full, &q->lock, INFINITE, 0);
    if (q->closed) { ReleaseSRWLockExclusive(&q->lock); return FALSE; }
    q->buf[q->tail] = v; q->tail = (q->tail + 1) % QCAP; q->count++;
    ReleaseSRWLockExclusive(&q->lock);
    WakeConditionVariable(&q->not_empty);                      /* notify_one, after unlock */
    return TRUE;
}

/* Returns FALSE only when closed AND drained: consumers finish every accepted item. */
static BOOL queue_pop(Queue* q, int* out) {
    AcquireSRWLockExclusive(&q->lock);
    while (q->count == 0 && !q->closed)                       /* predicate loop */
        SleepConditionVariableSRW(&q->not_empty, &q->lock, INFINITE, 0);
    if (q->count == 0) { ReleaseSRWLockExclusive(&q->lock); return FALSE; }   /* closed and drained */
    *out = q->buf[q->head]; q->head = (q->head + 1) % QCAP; q->count--;
    ReleaseSRWLockExclusive(&q->lock);
    WakeConditionVariable(&q->not_full);
    return TRUE;
}

static void queue_close(Queue* q) {
    AcquireSRWLockExclusive(&q->lock);
    q->closed = TRUE;                     /* WHY under the lock: otherwise a waiter can check the
                                             predicate, miss the flag, and sleep through the wake */
    ReleaseSRWLockExclusive(&q->lock);
    WakeAllConditionVariable(&q->not_empty);   /* notify_all: every waiter must re-check */
    WakeAllConditionVariable(&q->not_full);
}

#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 100000

static Queue g_q;
/* One result slot per consumer: each thread writes only its own slot, and main reads them after
 * the join (WaitForMultipleObjects on the thread handles is the happens-before edge). No
 * interlocked operations needed, the same idea as a padded per-thread counter in module 10. */
typedef struct { long long sum; long items; } ConsumerResult;
static ConsumerResult g_results[CONSUMERS];

static unsigned __stdcall producer(void* arg) {
    int base = (int)(INT_PTR)arg * PER_PRODUCER;
    int i;
    for (i = 1; i <= PER_PRODUCER; ++i) queue_push(&g_q, base + i);
    return 0;
}

static unsigned __stdcall consumer(void* arg) {
    ConsumerResult* out = (ConsumerResult*)arg;
    int v;
    long long local = 0;
    long n = 0;
    while (queue_pop(&g_q, &v)) { local += v; ++n; }
    out->sum = local;
    out->items = n;
    return 0;
}

static void demo_queue(void) {
    HANDLE p[PRODUCERS], c[CONSUMERS];
    int i;
    long long expected = 0, sum = 0;
    long items = 0;
    queue_init(&g_q);
    for (i = 0; i < CONSUMERS; ++i) c[i] = (HANDLE)_beginthreadex(NULL, 0, consumer, &g_results[i], 0, NULL);
    for (i = 0; i < PRODUCERS; ++i) p[i] = (HANDLE)_beginthreadex(NULL, 0, producer, (void*)(INT_PTR)i, 0, NULL);
    WaitForMultipleObjects(PRODUCERS, p, TRUE, INFINITE);   /* producers done ... */
    queue_close(&g_q);                                      /* ... then close: consumers drain and exit */
    WaitForMultipleObjects(CONSUMERS, c, TRUE, INFINITE);
    for (i = 0; i < PRODUCERS; ++i) CloseHandle(p[i]);
    for (i = 0; i < CONSUMERS; ++i) CloseHandle(c[i]);
    for (i = 0; i < CONSUMERS; ++i) { sum += g_results[i].sum; items += g_results[i].items; }
    for (i = 0; i < PRODUCERS; ++i) {
        long long base = (long long)i * PER_PRODUCER;
        expected += base * PER_PRODUCER + (long long)PER_PRODUCER * (PER_PRODUCER + 1) / 2;
    }
    printf("[1] items=%ld (expected %d) sum=%lld (expected %lld)\n",
           items, PRODUCERS * PER_PRODUCER, sum, expected);
}

/* ---------------------------------------------------------------------------------------------
 * Part 2: WaitForMultipleObjects -- shutdown OR work
 * ------------------------------------------------------------------------------------------- */
static HANDLE g_shutdown;    /* manual-reset: once set, stays set for every waiter */
static HANDLE g_work;        /* semaphore: one count per work item */

static unsigned __stdcall wfmo_worker(void* arg) {
    /* Shutdown FIRST: when several handles are signalled, WFMO returns the lowest index. */
    HANDLE h[2];
    int done = 0;
    (void)arg;
    h[0] = g_shutdown; h[1] = g_work;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, h, FALSE /* any */, INFINITE);
        if (r == WAIT_OBJECT_0) break;                 /* shutdown */
        if (r == WAIT_OBJECT_0 + 1) { ++done; continue; }   /* one unit of work */
        fprintf(stderr, "WFMO failed: %lu\n", (unsigned long)GetLastError());
        break;
    }
    printf("[2] worker processed %d items before shutdown\n", done);
    return 0;
}

static void demo_wfmo(void) {
    HANDLE th;
    g_shutdown = CreateEventW(NULL, TRUE /* manual */, FALSE, NULL);
    g_work = CreateSemaphoreW(NULL, 0, 1000, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, wfmo_worker, NULL, 0, NULL);
    ReleaseSemaphore(g_work, 5, NULL);
    Sleep(50);                       /* let the worker take the 5 items */
    SetEvent(g_shutdown);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th); CloseHandle(g_work); CloseHandle(g_shutdown);
    /* Portable C++17 replacement: one queue whose pop() returns "closed" as a value, i.e. the
     * composite predicate (closed || !empty) of Part 1. No handle array needed. */
}

/* ---------------------------------------------------------------------------------------------
 * Part 3: InitOnceExecuteOnce == std::call_once
 * ------------------------------------------------------------------------------------------- */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_init_runs = 0;
static int* g_config = NULL;

static BOOL CALLBACK init_config(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    static int storage[4] = {1, 2, 3, 4};
    (void)once; (void)param; (void)ctx;
    InterlockedIncrement(&g_init_runs);
    Sleep(20);                        /* slow init: other callers block until it returns */
    g_config = storage;
    return TRUE;                      /* FALSE would leave it uninitialised; next caller retries */
}

static unsigned __stdcall once_user(void* arg) {
    (void)arg;
    InitOnceExecuteOnce(&g_once, init_config, NULL, NULL);
    return g_config && g_config[3] == 4 ? 0 : 1;   /* published: every caller sees the data */
}

static void demo_init_once(void) {
    HANDLE h[16];
    int i, bad = 0;
    DWORD code;
    for (i = 0; i < 16; ++i) h[i] = (HANDLE)_beginthreadex(NULL, 0, once_user, NULL, 0, NULL);
    WaitForMultipleObjects(16, h, TRUE, INFINITE);
    for (i = 0; i < 16; ++i) {
        if (GetExitCodeThread(h[i], &code) && code != 0) ++bad;
        CloseHandle(h[i]);
    }
    printf("[3] InitOnceExecuteOnce: init ran %ld time(s) for 16 threads, bad reads=%d\n",
           (long)g_init_runs, bad);
}

int main(void) {
    demo_queue();
    demo_wfmo();
    demo_init_once();
    return 0;
}
