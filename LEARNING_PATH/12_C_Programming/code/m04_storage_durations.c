/* m04_storage_durations.c - automatic, static, allocated and thread storage side by side.
 *
 * Module: 12_C_Programming/04_memory_storage_scope_and_lifetime.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m04_storage_durations.c -o m04_storage_durations
 * Uses C11 <threads.h>. glibc 2.34 and later need no -pthread; on older glibc add -pthread.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

int g_total_calls;                  /* static storage, file scope, EXTERNAL linkage, starts at 0 */
static int s_module_calls;          /* static storage, file scope, INTERNAL linkage, starts at 0 */
static _Thread_local int t_calls;   /* thread storage: every thread gets its own copy, starts at 0 */

static int next_sequence(void)
{
    static int seq = 100;           /* static storage, BLOCK scope: initialised once, before main,
                                       and keeps its value between calls */
    ++g_total_calls;
    ++s_module_calls;
    return seq++;
}

static int automatic_demo(void)
{
    int local = 5;                  /* automatic: a fresh object every call, gone at return */
    ++local;
    return local;                   /* returning the VALUE is fine; its address would dangle */
}

static int worker(void *arg)
{
    int n = *(const int *)arg;
    for (int i = 0; i < n; ++i)
        ++t_calls;                  /* no lock: no other thread can see this thread's t_calls */
    return t_calls;
}

int main(void)
{
    assert(next_sequence() == 100);
    assert(next_sequence() == 101); /* seq survived the return */
    assert(automatic_demo() == 6 && automatic_demo() == 6);   /* local did not survive */
    printf("static: seq continued 100 -> 101, g_total_calls = %d, s_module_calls = %d\n",
           g_total_calls, s_module_calls);
    printf("automatic: automatic_demo() returned 6 twice (fresh local each call)\n");

    /* Allocated: lives from malloc until free, independent of any scope. */
    int *heap = malloc(4 * sizeof *heap);
    if (heap == NULL) return 1;
    heap[3] = 33;
    int *alias = heap;              /* a second copy of the address: the block has no single "owner" */
    (void)alias;
    free(heap);                     /* ends the object: heap AND alias are now dangling */
    heap = NULL;                    /* fixes heap only; nothing in the language warns about alias */
    printf("allocated: block freed, and the copy of its address is now dangling\n");

    thrd_t a, b;
    int na = 1000, nb = 250, ra = 0, rb = 0;
    if (thrd_create(&a, worker, &na) != thrd_success) return 1;
    if (thrd_create(&b, worker, &nb) != thrd_success) return 1;
    thrd_join(a, &ra);
    thrd_join(b, &rb);
    printf("thread: A counted %d, B counted %d, main's own t_calls is still %d\n", ra, rb, t_calls);
    assert(ra == 1000 && rb == 250 && t_calls == 0);
    return 0;
}
