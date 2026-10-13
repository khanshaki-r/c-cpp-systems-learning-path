/* m11_syscalls.c - the Linux system-call essentials in one file (module 11).
 *
 * Build:  gcc -std=c11 -Wall -Wextra -Werror -O2 -g m11_syscalls.c -o m11_syscalls
 * Run:    ./m11_syscalls fileio /tmp/m11.txt     open/write/fsync/close, read back, errno demo
 *         ./m11_syscalls mmap   /etc/services    map a file and count its lines without read()
 *         ./m11_syscalls spawn                   pipe + fork + exec + waitpid (CreateProcess, Linux style)
 *         ./m11_syscalls signals                 self-pipe trick + timerfd + poll; Ctrl-C to stop
 *         strace -f ./m11_syscalls spawn         watch every syscall the demos make
 *
 * Every syscall return value is checked. That is the point of the file.
 */
#define _GNU_SOURCE /* pipe2, accept4-style flags, timerfd; glibc hides them otherwise */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Print "what: strerror(errno)" and exit. errno is read FIRST: fprintf itself may change it. */
static void die(const char *what) {
    const int saved = errno;
    fprintf(stderr, "%s: %s (errno=%d)\n", what, strerror(saved), saved);
    exit(1);
}

/* write() may write fewer bytes than asked (pipes, sockets, signals). Loop until done. */
static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue; /* interrupted before writing anything: retry */
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ fileio */
static int demo_fileio(const char *path) {
    const char text[] = "line 1: written with write()\nline 2: made durable with fsync()\n";

    /* O_CLOEXEC: the fd is closed automatically in any program we exec later. Always use it. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) die("open for write");
    if (write_all(fd, text, sizeof text - 1) < 0) die("write");

    /* write() only copied the bytes into the page cache. fsync() waits until the device has them.
     * Without it, a power cut after "commit OK" loses the data. */
    if (fsync(fd) < 0) die("fsync");

    /* close() can report a deferred write error (NFS, some filesystems). Check it. */
    if (close(fd) < 0) die("close");

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open for read");
    char buf[256];
    ssize_t n;
    printf("read back from %s:\n", path);
    /* printf buffers in user space; write() below goes straight to the kernel. Flush first, or
     * the heading appears AFTER the file contents when stdout is a pipe or a file. */
    fflush(stdout);
    while ((n = read(fd, buf, sizeof buf)) != 0) { /* 0 means end of file */
        if (n < 0) {
            if (errno == EINTR) continue;
            die("read");
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) die("write stdout");
    }
    close(fd);

    /* errno discipline: check the return value first, read errno only when it says "failed". */
    if (open("/definitely/not/here", O_RDONLY | O_CLOEXEC) < 0) {
        const int e = errno;
        printf("expected failure: open -> errno %d (%s), ENOENT is %d\n", e, strerror(e), ENOENT);
    }
    return 0;
}

/* ------------------------------------------------------------------ mmap */
static int demo_mmap(const char *path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat");
    if (st.st_size == 0) { /* mmap of length 0 fails with EINVAL */
        printf("%s is empty\n", path);
        close(fd);
        return 0;
    }
    const size_t len = (size_t)st.st_size;

    /* MAP_PRIVATE + PROT_READ: the file's page-cache pages appear in our address space.
     * No copy into a user buffer; pages are faulted in on first touch. */
    const char *data = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) die("mmap");
    close(fd); /* the mapping keeps the file referenced; the fd is no longer needed */

    /* Tell the kernel we will read front to back so it reads ahead aggressively. */
    if (madvise((void *)data, len, MADV_SEQUENTIAL) < 0) die("madvise");

    size_t lines = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '\n') ++lines;
    }
    printf("%s: %zu bytes, %zu lines, mapped at %p\n", path, len, lines, (const void *)data);

    if (munmap((void *)data, len) < 0) die("munmap");
    return 0;
}

/* ------------------------------------------------------------------ spawn */
static int demo_spawn(void) {
    int pfd[2];
    /* pipe2 with O_CLOEXEC: neither end leaks into unrelated children. */
    if (pipe2(pfd, O_CLOEXEC) < 0) die("pipe2");

    const pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        /* Child. Only async-signal-safe calls between fork and exec in a multithreaded parent;
         * this one is single-threaded, but keep the habit. dup2 clears O_CLOEXEC on the new fd. */
        if (dup2(pfd[1], STDOUT_FILENO) < 0) _exit(126);
        execlp("uname", "uname", "-srm", (char *)NULL);
        _exit(127); /* exec only returns on failure. _exit: do not flush the parent's stdio buffers twice */
    }

    /* Parent: close our copy of the write end, or read() never sees EOF. */
    close(pfd[1]);
    char buf[512];
    size_t used = 0;
    for (;;) {
        const ssize_t n = read(pfd[0], buf + used, sizeof buf - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("read pipe");
        }
        if (n == 0) break;
        used += (size_t)n;
        if (used == sizeof buf - 1) break;
    }
    buf[used] = '\0';
    close(pfd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { /* reap the child, or it stays a zombie */
        if (errno != EINTR) die("waitpid");
    }
    if (WIFEXITED(status)) {
        printf("child %d exited %d, said: %s", (int)pid, WEXITSTATUS(status), buf);
    } else if (WIFSIGNALED(status)) {
        printf("child %d killed by signal %d\n", (int)pid, WTERMSIG(status));
    }
    return 0;
}

/* ------------------------------------------------------------------ signals + timer */
static int g_sigpipe_w = -1; /* write end of the self-pipe; the handler only touches this */

/* Async-signal-safe handler: write() is on the safe list, printf/malloc/mutex are not.
 * Save and restore errno because the interrupted code may be about to read it. */
static void on_signal(int signo) {
    const int saved = errno;
    const unsigned char b = (unsigned char)signo;
    ssize_t ignored = write(g_sigpipe_w, &b, 1);
    (void)ignored;
    errno = saved;
}

static int demo_signals(void) {
    int sp[2];
    /* Non-blocking write end: if the pipe is full the handler must not block. */
    if (pipe2(sp, O_CLOEXEC | O_NONBLOCK) < 0) die("pipe2");
    g_sigpipe_w = sp[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* restart slow syscalls; poll() still returns EINTR by design */
    if (sigaction(SIGINT, &sa, NULL) < 0) die("sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) < 0) die("sigaction SIGTERM");

    /* timerfd: a timer that is a file descriptor, so it fits in the same poll() as everything else. */
    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) die("timerfd_create");
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 500 * 1000 * 1000;    /* first expiry after 500 ms */
    its.it_interval.tv_nsec = 500 * 1000 * 1000; /* then every 500 ms */
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) die("timerfd_settime");

    printf("pid %d: ticking every 500 ms; Ctrl-C or `kill -TERM %d` to stop (auto-stop after 20 ticks)\n",
           (int)getpid(), (int)getpid());
    fflush(stdout);

    struct pollfd pfds[2] = {{.fd = sp[0], .events = POLLIN}, {.fd = tfd, .events = POLLIN}};
    unsigned long ticks = 0;
    for (;;) {
        const int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue; /* a signal arrived; the self-pipe will tell us which */
            die("poll");
        }
        if (pfds[0].revents & POLLIN) {
            unsigned char signo = 0;
            if (read(sp[0], &signo, 1) == 1) {
                /* Now we are in normal context: printf, cleanup, anything is allowed. */
                printf("\ngot signal %d (%s) after %lu ticks: cleaning up, exit 0\n", signo,
                       strsignal(signo), ticks);
                break;
            }
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t expirations = 0; /* number of expiries since last read (catches overruns) */
            if (read(tfd, &expirations, sizeof expirations) != (ssize_t)sizeof expirations) die("read timerfd");
            ticks += expirations;
            printf("tick %lu\n", ticks);
            fflush(stdout);
            if (ticks >= 20) break;
        }
    }
    close(tfd);
    close(sp[0]);
    close(sp[1]);
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s fileio PATH | mmap PATH | spawn | signals\n", argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "fileio") == 0 && argc == 3) return demo_fileio(argv[2]);
    if (strcmp(argv[1], "mmap") == 0 && argc == 3) return demo_mmap(argv[2]);
    if (strcmp(argv[1], "spawn") == 0) return demo_spawn();
    if (strcmp(argv[1], "signals") == 0) return demo_signals();
    usage(argv[0]);
    return 2;
}
