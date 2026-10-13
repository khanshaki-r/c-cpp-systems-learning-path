/* m13_tcp_echo.c - a minimal TCP echo server and client in plain C with POSIX sockets.
 * The C++ RAII version of the same thing is 08_Networking_Distributed/code/m03_echo_server.cpp.
 *
 * Modes:
 *   ./m13_tcp_echo                         self-test: server in a child process on a free
 *                                          port, client in the parent, results checked
 *   ./m13_tcp_echo server 9000             iterative echo server (one client at a time)
 *   ./m13_tcp_echo client 127.0.0.1 9000 "hello"
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m13_tcp_echo m13_tcp_echo.c
 * Run:    ./m13_tcp_echo
 */
#define _POSIX_C_SOURCE 200809L   /* getaddrinfo, fork, sockets */
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

static int write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;                   /* EPIPE if the peer is gone (SIGPIPE ignored) */
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Create a listening socket. port "0" asks the kernel for any free port; *bound gets it. */
static int listen_on(const char *host, const char *port, int *bound)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));  /* not errno! */
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  /* restart without TIME_WAIT pain */
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 16) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (bound && getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
        if (ss.ss_family == AF_INET)
            *bound = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        else
            *bound = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return fd;
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {   /* try every address: IPv6 then IPv4, etc. */
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Echo everything until the client closes its sending side. Returns bytes echoed or -1. */
static long echo_connection(int cfd)
{
    char buf[4096];
    long total = 0;
    for (;;) {
        ssize_t n = read(cfd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0)
            return total;                /* orderly shutdown from the client */
        if (write_full(cfd, buf, (size_t)n) != 0)
            return -1;
        total += n;
    }
}

static int accept_retry(int lfd)
{
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0 || errno != EINTR)
            return cfd;
    }
}

/* Send msg, half-close, read the echo until EOF. Returns bytes received or -1. */
static long round_trip(const char *host, const char *port, const char *msg, size_t len,
                       char *reply, size_t cap)
{
    int fd = connect_to(host, port);
    if (fd < 0)
        return -1;
    if (write_full(fd, msg, len) != 0) {
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);              /* "I am done sending": the server sees EOF */
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, reply + got, cap - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
        if (got == cap) break;
    }
    close(fd);
    return (long)got;
}

static int self_test(void)
{
    int port = 0;
    int lfd = listen_on("127.0.0.1", "0", &port);
    assert(lfd >= 0 && port > 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {                     /* child = server: two connections, then exit */
        for (int i = 0; i < 2; i++) {
            int cfd = accept_retry(lfd);
            if (cfd < 0) _exit(1);
            long n = echo_connection(cfd);
            close(cfd);
            if (n < 0) _exit(2);
        }
        close(lfd);
        _exit(0);
    }
    close(lfd);                         /* parent does not accept */

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    static char reply[64 * 1024];
    const char *hello = "PING ACC-1001";
    long n = round_trip("127.0.0.1", portstr, hello, strlen(hello), reply, sizeof reply);
    assert(n == (long)strlen(hello) && memcmp(reply, hello, (size_t)n) == 0);
    printf("echo of \"%s\": %ld bytes back on port %d\n", hello, n, port);

    /* 32 KiB: larger than one read(), so both sides must loop */
    static char big[32 * 1024];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)('A' + i % 26);
    n = round_trip("127.0.0.1", portstr, big, sizeof big, reply, sizeof reply);
    assert(n == (long)sizeof big && memcmp(reply, big, sizeof big) == 0);
    printf("echo of %zu bytes: intact\n", sizeof big);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("all checks passed");
    return 0;
}

int main(int argc, char **argv)
{
    /* Writing to a socket whose peer has gone raises SIGPIPE, whose default action kills the
     * process. A server ignores it and handles EPIPE from write() instead. */
    signal(SIGPIPE, SIG_IGN);

    if (argc == 1)
        return self_test();

    if (argc == 3 && strcmp(argv[1], "server") == 0) {
        int lfd = listen_on(NULL, argv[2], NULL);
        if (lfd < 0) {
            perror("listen");
            return 1;
        }
        printf("echo server on port %s\n", argv[2]);
        for (;;) {                      /* iterative: a slow client blocks everyone else */
            int cfd = accept_retry(lfd);
            if (cfd < 0) {
                perror("accept");
                continue;
            }
            long n = echo_connection(cfd);
            printf("client done, %ld bytes echoed\n", n);
            close(cfd);
        }
    }

    if (argc == 5 && strcmp(argv[1], "client") == 0) {
        char reply[4096];
        long n = round_trip(argv[2], argv[3], argv[4], strlen(argv[4]), reply, sizeof reply - 1);
        if (n < 0) {
            perror("client");
            return 1;
        }
        reply[n] = '\0';
        printf("%s\n", reply);
        return 0;
    }

    fprintf(stderr, "usage: %s [server PORT | client HOST PORT MESSAGE]\n", argv[0]);
    return 2;
}
