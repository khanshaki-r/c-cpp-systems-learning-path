// m03_echo_client.cpp - echo client that sends lines and verifies the echo byte for byte.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_echo_client.cpp -o echo_client
// Run:    ./echo_client 127.0.0.1 7000            (interactive: type lines)
//         seq 1 100000 | ./echo_client 127.0.0.1 7000   (bulk: verifies 100k lines)
//
// The point of this program is the read side: after sending n bytes we must read EXACTLY n
// bytes back, and they may arrive in any number of recv() calls. read_exact() does that loop.
#include "m03_socket.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const char* port = argc > 2 ? argv[2] : "7000";

    net::Fd sock;
    try {
        sock = net::connect_tcp(host, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "connect to %s:%s failed: %s\n", host, port, e.what());
        return EXIT_FAILURE;
    }
    net::set_nodelay(sock.get());
    // If the server hangs, do not hang forever with it. 5 s is generous for an echo.
    net::set_recv_timeout(sock.get(), 5000);

    std::string line;
    std::vector<char> back;
    unsigned long lines = 0, bytes = 0;
    try {
        while (std::getline(std::cin, line)) {
            line.push_back('\n');
            net::write_all(sock.get(), line);

            back.resize(line.size());
            if (!net::read_exact(sock.get(), back.data(), back.size())) {
                std::fprintf(stderr, "server closed the connection\n");
                return EXIT_FAILURE;
            }
            if (std::string_view(back.data(), back.size()) != line) {
                std::fprintf(stderr, "MISMATCH on line %lu\n", lines + 1);
                return EXIT_FAILURE;
            }
            ++lines;
            bytes += line.size();
        }
    } catch (const std::system_error& e) {
        if (e.code().value() == EAGAIN || e.code().value() == EWOULDBLOCK)
            std::fprintf(stderr, "timeout waiting for echo\n");
        else
            std::fprintf(stderr, "I/O error: %s\n", e.what());
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // Half-close: tell the server we are done sending. The server's read() returns 0, it closes,
    // and because WE sent FIN first, WE hold TIME_WAIT (check with: ss -tan | grep 7000).
    ::shutdown(sock.get(), SHUT_WR);
    std::fprintf(stderr, "ok: %lu lines, %lu bytes echoed correctly\n", lines, bytes);
    return EXIT_SUCCESS;
}
