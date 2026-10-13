/*
 * m13_win32_threads.c -- WINDOWS-ONLY. Not part of the WSL build (build.sh skips it).
 *
 * Shows, in the Win32 dialect you use at work:
 *   1. _beginthreadex + WaitForMultipleObjects(join all) + CloseHandle
 *   2. a CRITICAL_SECTION-guarded counter (with spin count) vs an InterlockedIncrement counter
 *   3. an auto-reset Event "work available" signal LOSING a wake when two SetEvent calls coalesce
 *   4. the same queue signalled by a Semaphore, which counts and therefore loses nothing
 *
 * Build (MSVC developer prompt):  cl /W4 /O2 m13_win32_threads.c
 * Build (MinGW GCC):              gcc -std=c11 -Wall -Wextra -O2 m13_win32_threads.c -o m13_threads.exe
 *
 * C++17 equivalents: std::thread + join; std::mutex + std::lock_guard; std::atomic<long>;
 * an auto-reset Event has NO exact std equivalent (it is a binary semaphore that saturates at 1);
 * the Semaphore is std::counting_semaphore in C++20 or mutex + int + condition_variable in C++17.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------------------------
 * Part 1 + 2: threads and a critical section
 * ------------------------------------------------------------------------------------------- */
#define NUM_WORKERS 4
#define ITERS 1000000

static CRITICAL_SECTION g_cs;
static long g_locked_counter = 0;          /* guarded by g_cs */
static volatile LONG g_interlocked_counter = 0;   /* volatile only because the Interlocked API wants it */

static unsigned __stdcall counter_worker(void* arg) {
    int i;
    (void)arg;
    for (i = 0; i < ITERS; ++i) {
        EnterCriticalSection(&g_cs);       /* std::lock_guard<std::mutex> lk(m); */
        ++g_locked_counter;
        LeaveCriticalSection(&g_cs);
        InterlockedIncrement(&g_interlocked_counter);   /* counter.fetch_add(1) (full barrier) */
    }
    return 0;
}

static void demo_threads_and_cs(void) {
    HANDLE h[NUM_WORKERS];
    int i;
    DWORD t0;
    /* Spin count 4000 is the value the heap manager uses; spinning briefly before the kernel wait
     * is what glibc's adaptive mutex and a futex fast path also do. */
    InitializeCriticalSectionAndSpinCount(&g_cs, 4000);
    t0 = GetTickCount();
    for (i = 0; i < NUM_WORKERS; ++i) {
        /* _beginthreadex, not CreateThread: initialises the C runtime's per-thread data. */
        h[i] = (HANDLE)_beginthreadex(NULL, 0, counter_worker, NULL, 0, NULL);
        if (h[i] == 0) { fprintf(stderr, "_beginthreadex failed\n"); ExitProcess(1); }
    }
    /* Join all. WaitForMultipleObjects is capped at MAXIMUM_WAIT_OBJECTS (64) handles. */
    WaitForMultipleObjects(NUM_WORKERS, h, TRUE, INFINITE);
    for (i = 0; i < NUM_WORKERS; ++i) CloseHandle(h[i]);   /* the join does not free the handle */
    DeleteCriticalSection(&g_cs);
    printf("[1] locked=%ld interlocked=%ld expected=%d (%lu ms)\n",
           g_locked_counter, (long)g_interlocked_counter, NUM_WORKERS * ITERS,
           (unsigned long)(GetTickCount() - t0));
}

/* ---------------------------------------------------------------------------------------------
 * Part 3 + 4: a tiny request queue, signalled two ways
 * ------------------------------------------------------------------------------------------- */
#define QCAP 16

typedef struct {
    CRITICAL_SECTION cs;
    int items[QCAP];
    int head, count;
    HANDLE signal;      /* either an auto-reset Event or a Semaphore */
    BOOL is_semaphore;
} ReqQueue;

static void rq_init(ReqQueue* q, BOOL use_semaphore) {
    InitializeCriticalSection(&q->cs);
    q->head = q->count = 0;
    q->is_semaphore = use_semaphore;
    if (use_semaphore)
        q->signal = CreateSemaphoreW(NULL, 0, QCAP, NULL);       /* count = number of items */
    else
        q->signal = CreateEventW(NULL, FALSE /* auto-reset */, FALSE, NULL);   /* one bit */
}

static void rq_destroy(ReqQueue* q) {
    CloseHandle(q->signal);
    DeleteCriticalSection(&q->cs);
}

static void rq_push(ReqQueue* q, int v) {
    EnterCriticalSection(&q->cs);
    q->items[(q->head + q->count) % QCAP] = v;
    q->count++;
    LeaveCriticalSection(&q->cs);
    if (q->is_semaphore)
        ReleaseSemaphore(q->signal, 1, NULL);    /* count goes 0 -> 1 -> 2: both pushes remembered */
    else
        SetEvent(q->signal);                     /* already signalled? stays signalled: pushes merge */
}

/* The classic bug: "one wake = one item". Correct with a semaphore, wrong with an auto-reset
 * event, because two SetEvent calls with no waiter in between are one signal. */
static int rq_pop_one_per_wake(ReqQueue* q, DWORD timeout_ms, int* out) {
    DWORD r = WaitForSingleObject(q->signal, timeout_ms);
    if (r != WAIT_OBJECT_0) return 0;            /* WAIT_TIMEOUT */
    EnterCriticalSection(&q->cs);
    if (q->count == 0) { LeaveCriticalSection(&q->cs); return 0; }
    *out = q->items[q->head];
    q->head = (q->head + 1) % QCAP;
    q->count--;
    LeaveCriticalSection(&q->cs);
    return 1;
}

typedef struct { ReqQueue* q; int consumed; } ConsumerArg;

static unsigned __stdcall consumer(void* p) {
    ConsumerArg* a = (ConsumerArg*)p;
    int v;
    /* Give up after 200 ms of silence; a real server would wait INFINITE and the stranded item
     * would sit there until the NEXT request arrived (the "requests stall under light load" bug). */
    while (rq_pop_one_per_wake(a->q, 200, &v)) a->consumed++;
    return 0;
}

static void demo_signal(BOOL use_semaphore) {
    ReqQueue q;
    ConsumerArg arg;
    HANDLE th;
    int stranded;
    rq_init(&q, use_semaphore);
    /* Deterministic coalescing: both pushes happen before the consumer ever waits. */
    rq_push(&q, 101);
    rq_push(&q, 102);
    arg.q = &q; arg.consumed = 0;
    th = (HANDLE)_beginthreadex(NULL, 0, consumer, &arg, 0, NULL);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    EnterCriticalSection(&q.cs);
    stranded = q.count;
    LeaveCriticalSection(&q.cs);
    printf("[%s] pushed=2 consumed=%d stranded=%d  %s\n",
           use_semaphore ? "3 semaphore " : "2 auto-event",
           arg.consumed, stranded,
           stranded ? "<- LOST WAKE: two SetEvent calls became one signal"
                    : "<- every push has its own count");
    rq_destroy(&q);
}

int main(void) {
    demo_threads_and_cs();
    demo_signal(FALSE);   /* auto-reset Event: expect consumed=1 stranded=1 */
    demo_signal(TRUE);    /* Semaphore:        expect consumed=2 stranded=0 */
    /* Fixes for the event version: (a) drain the queue fully on every wake (loop until empty),
     * or (b) use a Semaphore, or (c) use CONDITION_VARIABLE with a predicate (see
     * m13_win32_srwlock_condvar.c), which is what std::condition_variable gives you. */
    return 0;
}
