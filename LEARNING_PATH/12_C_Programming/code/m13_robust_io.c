/* m13_robust_io.c - the three POSIX I/O habits that separate production code from samples:
 *
 *   1. read() and write() may transfer FEWER bytes than asked: loop until done.
 *   2. a slow system call interrupted by a signal fails with EINTR: retry it.
 *   3. "written" is not "durable": write a temp file, fsync it, rename it over the old one,
 *      fsync the directory. Then a crash leaves either the old file or the new one, never half.
 *
 * The test makes partial reads and EINTR happen on purpose: a child process writes into a
 * pipe in small pieces with pauses, while a 20 ms interval timer keeps interrupting the
 * parent's blocking read() (the SIGALRM handler is installed WITHOUT SA_RESTART).
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m13_robust_io m13_robust_io.c
 * Run:    ./m13_robust_io
 * Watch:  strace -f -e trace=read,write,fsync,rename ./m13_robust_io
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

static unsigned long g_eintr_retries;
static unsigned long g_short_reads;

/* Read exactly n bytes unless EOF comes first. Returns bytes read (< n only at EOF) or -1. */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR) {
                g_eintr_retries++;
                continue;                /* interrupted before any data: just retry */
            }
            return -1;
        }
        if (r == 0)
            break;                       /* EOF: the other side closed */
        if ((size_t)r < n - done)
            g_short_reads++;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* Write all n bytes. Returns 0 or -1 with errno set. */
static int write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Atomically and durably replace path with data. Returns 0 or -1 with errno set. */
static int durable_replace(const char *path, const void *data, size_t n)
{
    char tmp[PATH_MAX], dir[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write_full(fd, data, n) != 0 || fsync(fd) != 0) {  /* fsync: data AND size on disk */
        int saved = errno;
        close(fd);
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (close(fd) != 0) {               /* close can report a deferred write error (NFS) */
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    if (rename(tmp, path) != 0) {       /* atomic on POSIX within one filesystem */
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    /* The rename is a change to the DIRECTORY. Until the directory is synced, a power cut
     * can bring back the old name. */
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == dir)
        slash[1] = '\0';                /* file in "/" */
    else if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof dir, ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd < 0)
        return -1;
    int rc = fsync(dfd);
    int saved = errno;
    close(dfd);
    errno = saved;
    return rc;
}

static void on_alarm(int sig)
{
    (void)sig;                          /* only here so the signal interrupts read() */
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;                               /* nanosleep reports the time left: resume it */
}

int main(void)
{
    /* ---------- partial reads and EINTR through a pipe ---------- */
    const char msg[] = "TXN|ACC-1001|DEBIT|250000|2026-10-13T10:15:00Z";
    const size_t len = sizeof msg - 1;
    int p[2];
    int rc = pipe(p);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {                     /* child: write in 5 pieces with pauses */
        close(p[0]);
        size_t piece = (len + 4) / 5;
        for (size_t off = 0; off < len; off += piece) {
            sleep_ms(60);
            size_t n = len - off < piece ? len - off : piece;
            if (write_full(p[1], msg + off, n) != 0)
                _exit(1);
        }
        _exit(0);                       /* _exit: do not flush the parent's stdio buffers */
    }
    close(p[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                    /* no SA_RESTART: blocking read() fails with EINTR */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval every20ms = { { 0, 20000 }, { 0, 20000 } };
    setitimer(ITIMER_REAL, &every20ms, NULL);

    char buf[128];
    ssize_t got = read_full(p[0], buf, len);

    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    close(p[0]);
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    assert(got == (ssize_t)len && memcmp(buf, msg, len) == 0);
    assert(g_short_reads >= 1);         /* data arrived in pieces */
    assert(g_eintr_retries >= 1);       /* the timer interrupted at least one read() */
    printf("read_full: %zd bytes intact; short reads=%s, EINTR retries=%s\n", got,
           g_short_reads >= 4 ? "4 or more" : "some", g_eintr_retries >= 1 ? "1 or more" : "0");

    /* ---------- durable replace ---------- */
    char path[] = "/tmp/m13_cfg_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    rc = durable_replace(path, "version=1\n", 10);
    assert(rc == 0);
    rc = durable_replace(path, "version=2\nlimit=500\n", 20);
    assert(rc == 0);

    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    got = read_full(fd, buf, sizeof buf);
    close(fd);
    assert(got == 20 && memcmp(buf, "version=2\nlimit=500\n", 20) == 0);

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    struct stat st;
    assert(stat(tmp, &st) != 0 && errno == ENOENT);   /* no temp file left behind */
    printf("durable_replace: new contents visible, no temp file left\n");
    unlink(path);

    /* errors are reported, not hidden */
    rc = durable_replace("/no/such/dir/cfg", "x", 1);
    assert(rc == -1 && errno == ENOENT);

    puts("all checks passed");
    return 0;
}
