/* m11_epoll_preview.c - a single-threaded epoll echo server (module 11 preview).
 *
 * Build:  gcc -std=c11 -Wall -Wextra -Werror -O2 -g m11_epoll_preview.c -o epoll_echo
 * Run:    ./epoll_echo 7100
 * Test:   for i in 1 2 3; do (echo "hello $i"; sleep 1) | nc localhost 7100 & done; wait
 * Watch:  strace -f -e trace=epoll_wait,accept4,read,write ./epoll_echo 7100
 *
 * One thread serves every connection: the kernel tells us which fds are ready, we never block.
 * 08_Networking_Distributed/06_nonblocking_io_epoll_event_loops.md builds the real event loop
 * (per-connection output buffers, EPOLLOUT, edge-triggered mode, timers). This file is the shape.
 */
#define _GNU_SOURCE /* accept4 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 64

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int signo) {
    (void)signo;
    g_stop = 1; /* the only thing we do in the handler: one store to a sig_atomic_t */
}

static int make_listener(unsigned short port) {
    /* SOCK_NONBLOCK: accept4 on a listener with no pending connection returns EAGAIN, never blocks. */
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    const int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) goto fail;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) goto fail;
    if (listen(fd, SOMAXCONN) < 0) goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    const long port = argc > 1 ? strtol(argv[1], NULL, 10) : 7100;
    if (port < 1 || port > 65535) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN); /* writing to a closed socket: get EPIPE, not death */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); /* no SA_RESTART: epoll_wait returns EINTR and we see g_stop */
    sigaction(SIGTERM, &sa, NULL);

    const int lfd = make_listener((unsigned short)port);
    if (lfd < 0) {
        perror("listen socket");
        return 1;
    }

    /* The epoll instance is itself an fd. Its interest list lives in the kernel (a red-black tree);
     * epoll_wait returns only the ready subset, so cost scales with activity, not with fd count. */
    const int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN; /* level-triggered: reported again on every wait while data remains */
    ev.data.fd = lfd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        perror("epoll_ctl listener");
        return 1;
    }
    printf("epoll echo on port %ld, pid %d\n", port, (int)getpid());
    fflush(stdout);

    struct epoll_event events[MAX_EVENTS];
    int clients = 0;
    while (!g_stop) {
        const int n = epoll_wait(ep, events, MAX_EVENTS, 1000 /* ms */);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            if (fd == lfd) {
                /* Drain every pending connection: there may be more than one per wakeup. */
                for (;;) {
                    const int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* none left */
                        if (errno == EINTR) continue;
                        perror("accept4"); /* EMFILE: out of fds. Log and keep serving. */
                        break;
                    }
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        perror("epoll_ctl client");
                        close(cfd);
                        continue;
                    }
                    ++clients;
                    printf("client fd %d connected (%d open)\n", cfd, clients);
                }
                continue;
            }

            /* A client fd is readable (data, EOF or error). Read what is there and echo it. */
            char buf[4096];
            const ssize_t r = read(fd, buf, sizeof buf);
            if (r > 0) {
                /* Preview simplification: a real server queues unsent bytes and waits for EPOLLOUT.
                 * Here a short or EAGAIN write just drops the rest. */
                const ssize_t w = write(fd, buf, (size_t)r);
                if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) goto close_client;
                continue;
            }
            if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue; /* spurious wakeup */
        close_client:
            /* close() removes the fd from every epoll set it is in (if no dup of it exists). */
            close(fd);
            --clients;
            printf("client fd %d closed (%d open)\n", fd, clients);
        }
        fflush(stdout);
    }

    printf("shutting down, %d client(s) still open\n", clients);
    close(ep);
    close(lfd);
    return 0;
}
