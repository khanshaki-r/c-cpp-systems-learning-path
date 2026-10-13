// m03_echo_server.cpp - blocking, one-client-at-a-time echo server.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_echo_server.cpp -o echo_server
// Run:    ./echo_server 7000
// Test:   nc 127.0.0.1 7000      (type lines; they come back)
//
// This is the smallest correct TCP server: correct because it loops on read until EOF, loops on
// write until everything is sent, closes the connection on every path (RAII), and survives
// SIGPIPE, EINTR and a client that resets mid-stream.
#include "m03_socket.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static void serve_one(net::Fd client) {
    const std::string who = net::peer_name(client.get());
    std::printf("[+] %s connected\n", who.c_str());
    net::set_nodelay(client.get()); // echo is request/response; we want each reply out now

    char buf[4096];
    try {
        for (;;) {
            // read_some returns 0 on EOF: the peer sent FIN. That is the normal way a
            // connection ends. It is NOT an error, and we must close our side (or we sit
            // in CLOSE_WAIT forever).
            size_t n = net::read_some(client.get(), buf, sizeof buf);
            if (n == 0) break;
            // Echo back exactly what arrived. n may be 1 byte or 4096 bytes; TCP decides.
            net::write_all(client.get(), buf, n);
        }
    } catch (const std::system_error& e) {
        // ECONNRESET: peer sent RST. EPIPE: we wrote after the peer went away.
        // Both mean "that client is gone"; log and move on. Never crash the server.
        std::fprintf(stderr, "[!] %s: %s (errno %d)\n", who.c_str(), e.what(), e.code().value());
    }
    std::printf("[-] %s disconnected\n", who.c_str());
    // `client` goes out of scope here => close() => no leak on any path above.
}

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "7000";
    net::ignore_sigpipe();

    net::Fd listener;
    try {
        listener = net::listen_tcp(nullptr, port, /*backlog=*/128);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cannot listen on %s: %s\n", port, e.what());
        return EXIT_FAILURE;
    }
    std::printf("echo server listening on port %s (one client at a time)\n", port);

    for (;;) {
        net::Fd client = net::accept_tcp(listener.get());
        if (!client) break; // listener closed
        serve_one(std::move(client));
    }
    return EXIT_SUCCESS;
}
