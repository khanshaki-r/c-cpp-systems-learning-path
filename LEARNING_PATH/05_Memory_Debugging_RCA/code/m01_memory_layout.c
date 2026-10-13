/* m01_memory_layout.c - print where each kind of object lives in the process.
 *
 * Build:  gcc -std=c11 -Wall -Wextra -g -O0 -o build/m01_memory_layout m01_memory_layout.c
 * Run:    ./build/m01_memory_layout
 *
 * What to look at:
 *   - The addresses are ordered low -> high: text, rodata, data, bss, heap, then a big gap,
 *     then mmap regions (large malloc, shared libs), then the stack near 0x7ff... .
 *   - Run it twice. Every address changes (ASLR). Run it under gdb: they stop changing,
 *     because gdb disables ASLR by default (set disable-randomization off to restore).
 *   - Compare the printed addresses with the /proc/self/maps dump at the end.
 *
 * Warnings: none with -Wall -Wextra.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* glibc/ld define these symbols at the end of text, data and bss. */
extern char etext, edata, end;

static int         g_bss_counter;              /* zero-initialised -> .bss (no file bytes) */
static int         g_data_counter = 42;        /* initialised     -> .data               */
static const char  g_rodata_text[] = "hello";  /* read-only       -> .rodata             */

static void a_function(void) { }

static void show(const char *what, const void *p)
{
    printf("%-28s %p\n", what, p);
}

static void dump_maps(void)
{
    /* /proc/self/maps is the kernel's own list of mappings for this process.
     * Columns: address-range perms offset dev inode path */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { perror("fopen /proc/self/maps"); return; }
    char line[512];
    puts("\n--- /proc/self/maps ---");
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
}

int main(void)
{
    int   stack_local = 7;
    char *small_heap  = malloc(100);               /* served from the brk heap (main arena) */
    char *big_heap    = malloc(1 << 20);           /* 1 MiB > mmap threshold -> its own mmap */
    void *anon_map    = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    printf("pid %d  page size %ld bytes\n\n", (int)getpid(), sysconf(_SC_PAGESIZE));

    show("text   (a_function)",      (const void *)a_function);
    show("end of text (etext)",      &etext);
    show("rodata (g_rodata_text)",   g_rodata_text);
    show("data   (g_data_counter)",  &g_data_counter);
    show("end of data (edata)",      &edata);
    show("bss    (g_bss_counter)",   &g_bss_counter);
    show("end of bss (end)",         &end);
    show("heap   malloc(100)",       small_heap);
    show("mmap   malloc(1 MiB)",     big_heap);
    show("mmap   anonymous page",    anon_map);
    show("libc   (printf)",          (const void *)printf);
    show("stack  (local variable)",  &stack_local);

    /* Touch memory so the compiler cannot drop the allocations. */
    memset(small_heap, 1, 100);
    memset(big_heap, 2, 1 << 20);
    ((char *)anon_map)[0] = 3;
    g_bss_counter += stack_local;

    dump_maps();

    free(small_heap);
    free(big_heap);
    munmap(anon_map, 4096);
    return g_bss_counter == 0;  /* keep g_bss_counter alive */
}
