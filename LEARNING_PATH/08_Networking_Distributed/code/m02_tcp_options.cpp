// m02_tcp_options.cpp - inspect and set the TCP socket options discussed in module 02.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_tcp_options.cpp -o tcp_options
// Run:    ./tcp_options
//
// Creates a listening socket on an ephemeral loopback port, connects to it, accepts, and prints
// the options of the accepted socket before and after tuning. No helper header: this file is
// deliberately self-contained so you see every raw call once.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

[[noreturn]] static void die(const char* what) {
    std::fprintf(stderr, "%s: %s (errno %d)\n", what, std::strerror(errno), errno);
    std::exit(EXIT_FAILURE);
}

static int get_int_opt(int fd, int level, int opt, const char* name) {
    int v = 0;
    socklen_t len = sizeof v;
    if (getsockopt(fd, level, opt, &v, &len) < 0) die(name);
    return v;
}

static void set_int_opt(int fd, int level, int opt, int v, const char* name) {
    if (setsockopt(fd, level, opt, &v, sizeof v) < 0) die(name);
}

static void dump(int fd, const char* title) {
    std::printf("--- %s ---\n", title);
    std::printf("TCP_NODELAY   = %d   (1 = Nagle disabled)\n", get_int_opt(fd, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY"));
    std::printf("SO_KEEPALIVE  = %d\n", get_int_opt(fd, SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE"));
    std::printf("TCP_KEEPIDLE  = %d s\n", get_int_opt(fd, IPPROTO_TCP, TCP_KEEPIDLE, "TCP_KEEPIDLE"));
    std::printf("TCP_KEEPINTVL = %d s\n", get_int_opt(fd, IPPROTO_TCP, TCP_KEEPINTVL, "TCP_KEEPINTVL"));
    std::printf("TCP_KEEPCNT   = %d\n", get_int_opt(fd, IPPROTO_TCP, TCP_KEEPCNT, "TCP_KEEPCNT"));
    std::printf("SO_RCVBUF     = %d bytes (kernel reports 2x what was set)\n", get_int_opt(fd, SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF"));
    std::printf("SO_SNDBUF     = %d bytes\n", get_int_opt(fd, SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF"));
    std::printf("TCP_MAXSEG    = %d bytes (MSS; 65483 on loopback, ~1460 on Ethernet)\n", get_int_opt(fd, IPPROTO_TCP, TCP_MAXSEG, "TCP_MAXSEG"));

    // tcp_info is the same structure `ss -ti` prints: RTT, cwnd, retransmits, state.
    tcp_info info{};
    socklen_t len = sizeof info;
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) < 0) die("TCP_INFO");
    std::printf("TCP_INFO: state=%u rto=%u us rtt=%u us snd_cwnd=%u segments retrans=%u\n",
                static_cast<unsigned>(info.tcpi_state), info.tcpi_rto, info.tcpi_rtt, info.tcpi_snd_cwnd,
                info.tcpi_total_retrans);
}

int main() {
    // 1. Listening socket on 127.0.0.1, port 0 => kernel picks a free port.
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) die("socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) die("bind");
    if (listen(lfd, 8) < 0) die("listen");

    socklen_t alen = sizeof addr;
    if (getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen) < 0) die("getsockname");
    std::printf("listening on 127.0.0.1:%u\n", static_cast<unsigned>(ntohs(addr.sin_port)));

    // 2. Connect to ourselves. The connect completes inside the kernel (handshake on loopback)
    //    even before we call accept(), because the kernel finishes the handshake on its own.
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) die("socket(client)");
    if (connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) die("connect");

    int sfd = accept(lfd, nullptr, nullptr);
    if (sfd < 0) die("accept");

    dump(sfd, "accepted socket, defaults");

    // 3. Tune like a production RPC server would.
    set_int_opt(sfd, IPPROTO_TCP, TCP_NODELAY, 1, "TCP_NODELAY");
    set_int_opt(sfd, SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE");
    set_int_opt(sfd, IPPROTO_TCP, TCP_KEEPIDLE, 60, "TCP_KEEPIDLE");
    set_int_opt(sfd, IPPROTO_TCP, TCP_KEEPINTVL, 10, "TCP_KEEPINTVL");
    set_int_opt(sfd, IPPROTO_TCP, TCP_KEEPCNT, 5, "TCP_KEEPCNT");

    // 4. Move a few bytes so TCP_INFO has an RTT sample.
    const char msg[] = "ping";
    if (send(cfd, msg, sizeof msg, MSG_NOSIGNAL) < 0) die("send");
    char buf[16];
    if (recv(sfd, buf, sizeof buf, 0) < 0) die("recv");

    dump(sfd, "accepted socket, tuned");

    close(cfd);
    close(sfd);
    close(lfd);
    return EXIT_SUCCESS;
}
