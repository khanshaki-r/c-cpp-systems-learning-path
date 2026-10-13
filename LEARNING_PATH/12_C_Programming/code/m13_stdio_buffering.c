/* m13_stdio_buffering.c - see stdio buffering with your own eyes by checking the file size on
 * disk (with stat) after each step.
 *
 *   1. fully buffered (the default for files): nothing reaches the kernel until fflush/fclose
 *      or the buffer fills
 *   2. line buffered: flushed at every '\n' (the default for stdout on a terminal)
 *   3. unbuffered: every call is a write() system call (the default for stderr)
 *   4. mixing FILE* and write() on the same descriptor reorders your output
 *   5. fork() with unflushed data: the child inherits a copy of the buffer, so it is written twice
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m13_stdio_buffering m13_stdio_buffering.c
 * Run:    ./m13_stdio_buffering
 * Watch the system calls:  strace -e trace=write ./m13_stdio_buffering 2>&1 | head -40
 */
#define _POSIX_C_SOURCE 200809L   /* fileno, fork, mkstemp, stat */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

static char g_path[] = "/tmp/m13_buf_XXXXXX";

static long size_on_disk(void)
{
    struct stat st;
    int rc = stat(g_path, &st);
    assert(rc == 0);
    (void)rc;
    return (long)st.st_size;
}

static FILE *open_fresh(int mode)
{
    FILE *f = fopen(g_path, "w");
    assert(f != NULL);
    if (mode >= 0) {
        int rc = setvbuf(f, NULL, mode, 0);   /* must come before any other I/O on f */
        assert(rc == 0);
        (void)rc;
    }
    return f;
}

static void read_back(char *buf, size_t n)
{
    FILE *f = fopen(g_path, "r");
    assert(f != NULL);
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
}

int main(void)
{
    int fd = mkstemp(g_path);
    assert(fd >= 0);
    close(fd);
    char buf[64];

    /* 1. fully buffered */
    FILE *f = open_fresh(-1);
    fputs("hello\n", f);
    long before = size_on_disk();
    fflush(f);
    long after = size_on_disk();
    printf("full buffering : after fputs %ld bytes on disk, after fflush %ld\n", before, after);
    assert(before == 0 && after == 6);

    /* the buffer flushes itself when it fills: glibc sizes it from st_blksize */
    struct stat st;
    fstat(fileno(f), &st);
    for (int i = 0; i < 5000; i++)
        fputc('x', f);
    long partial = size_on_disk();
    printf("full buffering : 5006 bytes written, %ld on disk (st_blksize = %ld)\n",
           partial, (long)st.st_blksize);
    assert(partial > 6 && partial < 5006);
    fclose(f);

    /* 2. line buffered */
    f = open_fresh(_IOLBF);
    fputs("abc", f);
    before = size_on_disk();
    fputs("\n", f);
    after = size_on_disk();
    printf("line buffering : before newline %ld bytes, after newline %ld\n", before, after);
    assert(before == 0 && after == 4);
    fclose(f);

    /* 3. unbuffered */
    f = open_fresh(_IONBF);
    fputs("abc", f);
    printf("no buffering   : after fputs(\"abc\") %ld bytes\n", size_on_disk());
    assert(size_on_disk() == 3);
    fclose(f);

    /* 4. mixing layers: "A" waits in the FILE buffer, "B" goes straight to the kernel */
    f = open_fresh(-1);
    fputs("A", f);
    ssize_t w = write(fileno(f), "B", 1);
    assert(w == 1);
    fclose(f);
    read_back(buf, sizeof buf);
    printf("mixing layers  : wrote A then B, file contains \"%s\"\n", buf);
    assert(strcmp(buf, "BA") == 0);

    /* 5. fork with pending output. Flush stdout first, or this program's own printf output
     *    would be duplicated by the child too - which is the very bug being shown. */
    for (int fixed = 0; fixed <= 1; fixed++) {
        f = open_fresh(-1);
        fputs("x", f);
        if (fixed)
            fflush(f);                   /* the fix: empty the buffer before fork */
        fflush(stdout);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0)
            exit(0);                     /* exit() flushes the child's copy of the buffer */
        int status;
        waitpid(pid, &status, 0);
        fclose(f);                       /* the parent flushes its own copy */
        read_back(buf, sizeof buf);
        printf("fork %s : file contains \"%s\"\n", fixed ? "after fflush " : "with pending", buf);
        assert(strcmp(buf, fixed ? "x" : "xx") == 0);
    }

    unlink(g_path);
    puts("all checks passed");
    return 0;
}
