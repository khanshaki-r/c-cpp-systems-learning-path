// m07_rpc_demo.cpp - RPC server and client in one process over a real loopback TCP connection.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m07_rpc_demo.cpp -o rpc_demo
// Run:    ./rpc_demo
//
// Shows: a dispatch table, hand-rolled serialization, request-id correlation, error propagation
// (UnknownMethod, BadRequest, Internal), a client-side timeout where the outcome is unknown, and a
// thin "stub" layer so application code calls get_balance("ACC-1") instead of building frames.
#undef NDEBUG
#include "m07_rpc.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ----- the service contract (shared by client and server: this is the "IDL") -------------------
namespace methods {
constexpr uint16_t Echo = 1;       // req: str            -> resp: str
constexpr uint16_t Add = 2;        // req: i64 a, i64 b   -> resp: i64
constexpr uint16_t GetBalance = 3; // req: str account    -> resp: i64 paise ; Internal if unknown account
constexpr uint16_t Transfer = 4;   // req: str from, str to, i64 amount, str idempotency_key -> resp: i64 new_from_balance
constexpr uint16_t Sleep = 5;      // req: u32 ms         -> resp: (empty)   ; for the timeout demo
} // namespace methods

// ----- server-side application state -------------------------------------------------------------
struct Bank {
    std::mutex mu;
    std::map<std::string, int64_t> balances{{"ACC-1", 100000}, {"ACC-2", 5000}};
    // Idempotency keys already applied => the result they produced. A retried Transfer with the
    // same key returns the stored result instead of moving money twice.
    std::map<std::string, int64_t> applied;
};

static rpc::Dispatcher build_dispatcher(Bank& bank) {
    rpc::Dispatcher d;

    d.add(methods::Echo, [](rpc::Reader& req, rpc::Writer& resp) {
        resp.str(req.str());
        return rpc::Status::Ok;
    });

    d.add(methods::Add, [](rpc::Reader& req, rpc::Writer& resp) {
        int64_t a = req.i64();
        int64_t b = req.i64();
        if (!req.done()) throw rpc::BadPayload("trailing bytes"); // strict: catches schema skew
        resp.i64(a + b);
        return rpc::Status::Ok;
    });

    d.add(methods::GetBalance, [&bank](rpc::Reader& req, rpc::Writer& resp) {
        std::string acc = req.str();
        std::lock_guard<std::mutex> lk(bank.mu);
        auto it = bank.balances.find(acc);
        if (it == bank.balances.end()) throw std::runtime_error("no such account: " + acc); // => Internal
        resp.i64(it->second);
        return rpc::Status::Ok;
    });

    d.add(methods::Transfer, [&bank](rpc::Reader& req, rpc::Writer& resp) {
        std::string from = req.str(), to = req.str();
        int64_t amount = req.i64();
        std::string key = req.str();
        std::lock_guard<std::mutex> lk(bank.mu);
        if (auto done = bank.applied.find(key); done != bank.applied.end()) {
            resp.i64(done->second); // replay: same answer, no second debit
            return rpc::Status::Ok;
        }
        if (amount <= 0 || bank.balances[from] < amount) throw std::runtime_error("insufficient funds");
        bank.balances[from] -= amount;
        bank.balances[to] += amount;
        bank.applied[key] = bank.balances[from];
        resp.i64(bank.balances[from]);
        return rpc::Status::Ok;
    });

    d.add(methods::Sleep, [](rpc::Reader& req, rpc::Writer&) {
        uint32_t ms = req.u32();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return rpc::Status::Ok;
    });
    return d;
}

// ----- client-side stubs: the only place that knows the wire encoding of each method --------------
struct BankClient {
    rpc::Client c;
    int timeout_ms;

    BankClient(const char* host, const char* port, int timeout) : c(host, port), timeout_ms(timeout) {}

    rpc::Result echo(std::string_view s) {
        rpc::Writer w;
        w.str(s);
        return c.call(methods::Echo, w.take(), timeout_ms);
    }
    rpc::Result add(int64_t a, int64_t b) {
        rpc::Writer w;
        w.i64(a);
        w.i64(b);
        return c.call(methods::Add, w.take(), timeout_ms);
    }
    rpc::Result get_balance(std::string_view acc) {
        rpc::Writer w;
        w.str(acc);
        return c.call(methods::GetBalance, w.take(), timeout_ms);
    }
    rpc::Result transfer(std::string_view from, std::string_view to, int64_t amount, std::string_view key) {
        rpc::Writer w;
        w.str(from);
        w.str(to);
        w.i64(amount);
        w.str(key);
        return c.call(methods::Transfer, w.take(), timeout_ms);
    }
    rpc::Result sleep_ms(uint32_t ms) {
        rpc::Writer w;
        w.u32(ms);
        return c.call(methods::Sleep, w.take(), timeout_ms);
    }
};

static int64_t as_i64(const rpc::Result& r) {
    rpc::Reader rd(r.payload);
    return rd.i64();
}

static void show(const char* what, const rpc::Result& r) {
    if (r.status == rpc::Status::Ok)
        std::printf("%-28s OK\n", what);
    else
        std::printf("%-28s %s: %s\n", what, rpc::status_name(r.status), r.payload.c_str());
}

int main() {
    net::ignore_sigpipe();
    Bank bank;
    rpc::Dispatcher dispatcher = build_dispatcher(bank);

    // Listen on an ephemeral loopback port and discover which one we got.
    net::Fd listener = net::listen_tcp("127.0.0.1", "0", 16);
    sockaddr_in addr{};
    socklen_t alen = sizeof addr;
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &alen) < 0) {
        std::perror("getsockname");
        return 1;
    }
    std::string port = std::to_string(ntohs(addr.sin_port));
    std::printf("rpc server on 127.0.0.1:%s\n", port.c_str());

    // Thread-per-connection server (module 05 has the pool version). Joined at the end.
    std::vector<std::thread> conns;
    std::mutex conns_mu;
    std::thread acceptor([&] {
        for (;;) {
            net::Fd c = net::accept_tcp(listener.get());
            if (!c) return;
            std::lock_guard<std::mutex> lk(conns_mu);
            conns.emplace_back([&dispatcher](net::Fd fd) {
                try {
                    rpc::serve_connection(fd.get(), dispatcher);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[server] connection ended: %s\n", e.what());
                }
            }, std::move(c));
        }
    });

    {
        BankClient cli("127.0.0.1", port.c_str(), /*timeout_ms=*/500);

        rpc::Result r = cli.echo("hello rpc");
        show("echo", r);
        assert(r.status == rpc::Status::Ok && rpc::Reader(r.payload).str() == "hello rpc");

        r = cli.add(40, 2);
        show("add(40, 2)", r);
        assert(r.status == rpc::Status::Ok && as_i64(r) == 42);

        r = cli.add(-5, 3);
        assert(r.status == rpc::Status::Ok && as_i64(r) == -2); // negative numbers survive the wire

        r = cli.get_balance("ACC-1");
        show("get_balance(ACC-1)", r);
        assert(r.status == rpc::Status::Ok && as_i64(r) == 100000);

        r = cli.get_balance("ACC-9");
        show("get_balance(ACC-9)", r);
        assert(r.status == rpc::Status::Internal); // server exception became a status, server alive

        // Idempotent retry: the same key twice debits once.
        r = cli.transfer("ACC-1", "ACC-2", 2500, "txn-0001");
        show("transfer #1 key=txn-0001", r);
        assert(r.status == rpc::Status::Ok && as_i64(r) == 97500);
        r = cli.transfer("ACC-1", "ACC-2", 2500, "txn-0001");
        show("transfer retry same key", r);
        assert(r.status == rpc::Status::Ok && as_i64(r) == 97500); // not 95000
        r = cli.get_balance("ACC-2");
        assert(r.status == rpc::Status::Ok && as_i64(r) == 7500);

        // Malformed payload: Add expects 16 bytes, we send 3.
        r = cli.c.call(methods::Add, "abc", 500);
        show("add with 3-byte payload", r);
        assert(r.status == rpc::Status::BadRequest);

        // Unknown method: permanent error, do not retry.
        r = cli.c.call(999, "", 500);
        show("method 999", r);
        assert(r.status == rpc::Status::UnknownMethod);

        // Timeout: the server sleeps 2 s, client deadline is 0.5 s. Outcome unknown; the client
        // abandons the connection so a late reply can never be mismatched to a later request.
        r = cli.sleep_ms(2000);
        show("sleep(2000) with 500ms deadline", r);
        assert(r.status == rpc::Status::Timeout);
        assert(!cli.c.connected());

        r = cli.echo("after timeout");
        show("echo after timeout", r);
        assert(r.status == rpc::Status::Unavailable); // caller must reconnect (a pool would do it)
    }

    // Shut the server down: closing the listener ends accept(); client sockets are already closed.
    ::shutdown(listener.get(), SHUT_RDWR);
    acceptor.join();
    for (auto& t : conns) t.join();
    std::puts("rpc demo passed");
    return 0;
}
