/* m03_chunk_inspect.c - look at glibc malloc chunk headers from inside the program.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -g -O0 -o build/m03_chunk_inspect m03_chunk_inspect.c
 * Run:    ./build/m03_chunk_inspect
 *
 * This reads bytes just BEFORE the pointer malloc returned. That is deliberately outside
 * the allocation (undefined behaviour by the C standard), but with glibc on x86-64 it is
 * the chunk header and this program exists to show you what lives there. Do not run it
 * under ASan; ASan will (correctly) report a heap-buffer-overflow on the header read.
 *
 * What to look at:
 *   - size field = (request + 8, rounded up to 16, minimum 32) | flags.
 *     malloc(24) -> 0x21 : chunk 32 bytes, PREV_INUSE set.
 *     malloc(25) -> 0x31 : chunk 48 bytes.
 *     malloc(100)-> 0x71 : chunk 112 bytes.
 *   - The user pointer is header + 16. Two consecutive mallocs of the same size are exactly
 *     one chunk size apart.
 *   - After free(), the first 8 bytes of the OLD user data now hold a tcache "next" pointer
 *     and the next 8 the tcache key. That is why a use-after-free often reads a pointer-looking
 *     value and why the first fields of a freed object get "mysteriously" overwritten.
 *   - A 1 MiB request has IS_MMAPPED (0x2) set and a page-aligned size.
 *
 * Warnings: none with -Wall -Wextra.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <malloc.h>   /* malloc_usable_size */

#define PREV_INUSE      0x1u
#define IS_MMAPPED      0x2u
#define NON_MAIN_ARENA  0x4u

static void show_chunk(const char *label, void *user_ptr)
{
    /* glibc layout (x86-64):
     *   [ prev_size : 8 ][ size|flags : 8 ][ user data ... ]
     *   ^ chunk start                      ^ pointer returned by malloc            */
    uint64_t *hdr  = (uint64_t *)user_ptr - 1;          /* size field  */
    uint64_t  raw  = *hdr;
    uint64_t  size = raw & ~(uint64_t)0x7;              /* strip the 3 flag bits */

    printf("%-18s user=%p  size_field=0x%-6llx chunk=%4llu bytes  usable=%3zu  flags:%s%s%s\n",
           label, user_ptr, (unsigned long long)raw, (unsigned long long)size,
           malloc_usable_size(user_ptr),
           (raw & PREV_INUSE)     ? " PREV_INUSE" : "",
           (raw & IS_MMAPPED)     ? " IS_MMAPPED" : "",
           (raw & NON_MAIN_ARENA) ? " NON_MAIN_ARENA" : "");
}

int main(void)
{
    char *a = malloc(24);
    char *b = malloc(24);
    char *c = malloc(25);
    char *d = malloc(100);
    char *e = malloc(1 << 20);

    show_chunk("malloc(24)  a", a);
    show_chunk("malloc(24)  b", b);
    show_chunk("malloc(25)  c", c);
    show_chunk("malloc(100) d", d);
    show_chunk("malloc(1MiB) e", e);

    printf("\nb - a = %td bytes (one 32-byte chunk apart)\n", b - a);

    /* Write a recognisable pattern, free, then look at what glibc put there. */
    for (int i = 0; i < 24; i++) a[i] = 'A';
    uint64_t before0 = ((uint64_t *)a)[0], before1 = ((uint64_t *)a)[1];
    free(a);
    uint64_t after0 = ((uint64_t *)a)[0], after1 = ((uint64_t *)a)[1];   /* UAF on purpose */

    printf("\na[0..7]  before free: 0x%016llx   after free: 0x%016llx  <- tcache next (mangled ptr or 0)\n",
           (unsigned long long)before0, (unsigned long long)after0);
    printf("a[8..15] before free: 0x%016llx   after free: 0x%016llx  <- tcache key\n",
           (unsigned long long)before1, (unsigned long long)after1);

    /* Free b too: b's "next" now points at a (tcache is LIFO, singly linked). glibc >= 2.32
     * stores the pointer "safe-linked": (addr_of_next_field >> 12) ^ next. */
    free(b);
    printf("b[0..7]  after free:  0x%016llx  (safe-linked pointer to a: (&b>>12) ^ a)\n",
           (unsigned long long)((uint64_t *)b)[0]);

    char *a2 = malloc(24);   /* comes back from tcache: same address as b (LIFO) */
    printf("\nmalloc(24) again -> %p  (== b? %s)\n", (void *)a2, a2 == b ? "yes" : "no");

    free(a2); free(c); free(d); free(e);
    return 0;
}
