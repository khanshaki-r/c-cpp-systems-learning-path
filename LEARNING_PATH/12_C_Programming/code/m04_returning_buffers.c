/* m04_returning_buffers.c - four correct ways to "return a string" from a function, and the bug.
 *
 * Module: 12_C_Programming/04_memory_storage_scope_and_lifetime.md
 *
 * Normal build (bug excluded):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m04_returning_buffers.c -o m04_returning_buffers
 *
 * DELIBERATE UNDEFINED BEHAVIOUR with -DSHOW_BUG: format_id_bug() returns the address of a local
 * array. GCC warns even without -Wall (-Wreturn-local-addr), so build WITHOUT -Werror:
 *   gcc -std=c17 -Wall -Wextra -g -DSHOW_BUG m04_returning_buffers.c -o m04_bug && ./m04_bug
 * GCC 13 then compiles the function to return NULL instead of the dangling address.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1. Caller provides the buffer. Best default: no allocation, thread-safe, the caller decides
 *    where the bytes live. Returns the length it needed; truncated if that is >= outsz. */
static int format_id(char *out, size_t outsz, int id)
{
    return snprintf(out, outsz, "ACC-%06d", id);
}

/* 2. Static buffer. Simple, but NOT thread-safe and overwritten by the next call.
 *    This is how strerror, inet_ntoa, asctime and localtime work, and why they have _r versions. */
static const char *format_id_static(int id)
{
    static char buf[16];
    snprintf(buf, sizeof buf, "ACC-%06d", id);
    return buf;
}

/* 3. Heap. OWNERSHIP: the caller owns the result and must free() it. NULL on failure. */
static char *format_id_alloc(int id)
{
    char *p = malloc(16);
    if (p != NULL) snprintf(p, 16, "ACC-%06d", id);
    return p;
}

/* 4. Return a struct by value: the array travels inside the struct as a copy. */
struct id_text {
    char s[16];
};

static struct id_text format_id_value(int id)
{
    struct id_text t;
    snprintf(t.s, sizeof t.s, "ACC-%06d", id);
    return t;
}

#ifdef SHOW_BUG
/* THE BUG: buf dies when the function returns; the caller gets a dangling pointer. */
static char *format_id_bug(int id)
{
    char buf[16];
    snprintf(buf, sizeof buf, "ACC-%06d", id);
    return buf;
}
#endif

int main(void)
{
    char out[16];
    int need = format_id(out, sizeof out, 42);
    assert(need == 10 && strcmp(out, "ACC-000042") == 0);
    printf("1. caller buffer : %s\n", out);

    /* The static-buffer trap: both arguments point at the SAME buffer, so both print the value
     * from whichever call ran last. */
    printf("2. static buffer : %s and %s   <- same text twice\n", format_id_static(1), format_id_static(2));
    const char *first = format_id_static(1);
    const char *second = format_id_static(2);
    assert(first == second);

    char *heap = format_id_alloc(7);
    if (heap == NULL) return 1;
    printf("3. heap          : %s (caller frees)\n", heap);
    free(heap);

    struct id_text t = format_id_value(9);
    printf("4. struct value  : %s\n", t.s);
    assert(strcmp(t.s, "ACC-000009") == 0);

#ifdef SHOW_BUG
    char *dangling = format_id_bug(5);
    printf("BUG: format_id_bug returned %p\n", (void *)dangling);
    if (dangling != NULL) printf("BUG: and it reads as \"%s\"\n", dangling);
#endif
    return 0;
}
