// End-to-end tests: a real Server on an ephemeral port, real sockets, real threads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/client.hpp"
#include "tcpkv/log.hpp"
#include "tcpkv/metrics.hpp"
#include "tcpkv/protocol.hpp"
#include "tcpkv/server.hpp"
#include "tcpkv/socket.hpp"
#include "tcpkv/store.hpp"

using tcpkv::Client;
using tcpkv::ClientError;
using tcpkv::Metrics;
using tcpkv::Reply;
using tcpkv::Server;
using tcpkv::ServerConfig;
using tcpkv::Socket;
using tcpkv::Store;

namespace {

constexpr std::size_t kWorkers = 4;

std::string read_exactly(Socket& socket, std::size_t count) {
    std::string out;
    char buf[4096];
    while (out.size() < count) {
        const std::size_t n = socket.read_some(buf, std::min(sizeof buf, count - out.size()));
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

std::string read_until_eof(Socket& socket) {
    std::string out;
    char buf[4096];
    for (;;) {
        std::size_t n = 0;
        try {
            n = socket.read_some(buf, sizeof buf);
        } catch (const std::system_error&) {
            break;  // ECONNRESET counts as "closed" for our purposes
        }
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

class ServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        tcpkv::log::set_level(tcpkv::log::Level::Warn);
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = 0;  // ephemeral: tests can run in parallel
        config.threads = kWorkers;
        config.connection.poll_interval = std::chrono::milliseconds(20);
        config.stats_log_interval = std::chrono::seconds(0);
        server_ = std::make_unique<Server>(config, store_, metrics_);
        thread_ = std::thread([this] { server_->run(); });
    }

    void TearDown() override {
        server_->request_stop();
        thread_.join();
    }

    std::uint16_t port() const { return server_->port(); }
    Client connect() { return Client("127.0.0.1", port()); }
    Socket raw_connect() { return Socket::connect_tcp("127.0.0.1", port()); }

    Store store_;
    Metrics metrics_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

}  // namespace

TEST_F(ServerFixture, PingPong) {
    Client client = connect();
    EXPECT_EQ(client.ping(), "PONG");
}

TEST_F(ServerFixture, SetGetDelRoundTrip) {
    Client client = connect();
    EXPECT_FALSE(client.get("k").has_value());
    client.set("k", "hello");
    EXPECT_EQ(client.get("k").value_or(""), "hello");
    EXPECT_EQ(client.exists({"k", "missing"}), 1);
    EXPECT_EQ(client.del({"k", "missing"}), 1);
    EXPECT_FALSE(client.get("k").has_value());
}

TEST_F(ServerFixture, ValuesAreBinarySafe) {
    Client client = connect();
    const std::string value("line one\nline two\r\n\x00\xff tail", 26);
    client.set("bin", value);
    EXPECT_EQ(client.get("bin").value_or(""), value);

    const std::string big(tcpkv::limits::kMaxValueLength, 'B');
    client.set("big", big);
    EXPECT_EQ(client.get("big").value_or(""), big);
}

TEST_F(ServerFixture, KeysWithPrefix) {
    Client client = connect();
    client.set("user:1", "a");
    client.set("user:2", "b");
    client.set("order:1", "c");
    const std::vector<std::string> users = {"user:1", "user:2"};
    EXPECT_EQ(client.keys("user:"), users);
    EXPECT_EQ(client.keys().size(), 3u);
}

TEST_F(ServerFixture, UnknownCommandIsAnErrorButKeepsTheConnection) {
    Client client = connect();
    const Reply reply = client.execute({"FROB", "x"});
    EXPECT_EQ(reply.kind, Reply::Kind::Error);
    EXPECT_NE(reply.text.find("unknown command"), std::string::npos);
    EXPECT_EQ(client.ping(), "PONG");  // same connection still works
}

TEST_F(ServerFixture, ErrorRepliesSurfaceAsClientError) {
    Client client = connect();
    // execute() hands back error replies as data; the typed helpers turn them into exceptions.
    EXPECT_EQ(client.execute({"GET"}).kind, Reply::Kind::Error);  // wrong arity
    EXPECT_THROW(client.ttl("has space"), ClientError);           // becomes "TTL has space": arity error
}

TEST_F(ServerFixture, OversizedSetHeaderIsFatalForTheConnection) {
    Socket raw = raw_connect();
    raw.write_all("SET big " + std::to_string(tcpkv::limits::kMaxValueLength + 1) + "\n");
    EXPECT_EQ(read_until_eof(raw), "-ERR value too large\n");
}

TEST_F(ServerFixture, QuitClosesTheConnection) {
    Socket raw = raw_connect();
    raw.write_all("QUIT\n");
    EXPECT_EQ(read_until_eof(raw), "+BYE\n");
}

TEST_F(ServerFixture, PipelinedCommandsGetPipelinedReplies) {
    Socket raw = raw_connect();
    raw.write_all("PING\nSET a 1\nx\nGET a\nGET nope\nDEL a\n");
    const std::string expected = "+PONG\n+OK\n$1\nx\n$-1\n:1\n";
    EXPECT_EQ(read_exactly(raw, expected.size()), expected);
}

TEST_F(ServerFixture, BlankLinesAndCrLfAreTolerated) {
    Socket raw = raw_connect();
    raw.write_all("\r\n\nping\r\n");
    EXPECT_EQ(read_exactly(raw, 6), "+PONG\n");
}

TEST_F(ServerFixture, ExpireAndTtl) {
    Client client = connect();
    client.set("tmp", "v");
    EXPECT_TRUE(client.expire("tmp", 1));
    EXPECT_EQ(client.ttl("tmp"), 1);
    EXPECT_EQ(client.ttl("missing"), -2);
    client.set("forever", "v");
    EXPECT_EQ(client.ttl("forever"), -1);
    EXPECT_FALSE(client.expire("missing", 5));

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(client.get("tmp").has_value());
    EXPECT_EQ(client.ttl("tmp"), -2);
}

TEST_F(ServerFixture, StatsReportsCounters) {
    Client client = connect();
    client.ping();
    client.set("k", "v");
    const std::string stats = client.stats();
    EXPECT_NE(stats.find("commands_total"), std::string::npos);
    EXPECT_NE(stats.find("connections_active 1\n"), std::string::npos);
    EXPECT_GE(metrics_.commands_total.load(), 3u);
    EXPECT_EQ(metrics_.connections_accepted.load(), 1u);
}

TEST_F(ServerFixture, ManyConcurrentClientsSeeTheirOwnWrites) {
    constexpr int kClients = 16;  // more clients than workers: some wait in the pool queue
    constexpr int kOps = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            try {
                Client client = connect();
                for (int i = 0; i < kOps; ++i) {
                    const std::string key = "t" + std::to_string(c) + ":k" + std::to_string(i);
                    const std::string value = "v" + std::to_string(c * 1000 + i);
                    client.set(key, value);
                    if (client.get(key).value_or("") != value) {
                        ++failures;
                    }
                }
                client.quit();
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(store_.size(), static_cast<std::size_t>(kClients * kOps));
    EXPECT_EQ(metrics_.connections_accepted.load(), static_cast<std::uint64_t>(kClients));
}

TEST_F(ServerFixture, ActiveConnectionsDropToZeroAfterClientsLeave) {
    {
        Client a = connect();
        Client b = connect();
        a.ping();
        b.ping();
        EXPECT_EQ(metrics_.connections_active.load(), 2u);
    }
    // Workers notice EOF on their next poll (<= 20 ms); give them a moment.
    for (int i = 0; i < 100 && metrics_.connections_active.load() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(metrics_.connections_active.load(), 0u);
}

TEST(ServerShutdown, RequestStopUnblocksRunAndClosesClients) {
    tcpkv::log::set_level(tcpkv::log::Level::Warn);
    Store store;
    Metrics metrics;
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.threads = 2;
    config.connection.poll_interval = std::chrono::milliseconds(20);
    config.stats_log_interval = std::chrono::seconds(0);
    Server server(config, store, metrics);
    std::thread runner([&] { server.run(); });

    Socket idle_client = Socket::connect_tcp("127.0.0.1", server.port());
    Socket busy_client = Socket::connect_tcp("127.0.0.1", server.port());
    busy_client.write_all("PING\n");
    ASSERT_EQ(read_exactly(busy_client, 6), "+PONG\n");

    const auto t0 = std::chrono::steady_clock::now();
    server.request_stop();
    runner.join();  // must return promptly: the wake pipe interrupts poll(), workers see the flag
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_TRUE(server.stop_requested());

    // Both clients were closed by the server.
    EXPECT_EQ(read_until_eof(idle_client), "");
    EXPECT_EQ(read_until_eof(busy_client), "");
}

TEST(ServerShutdown, RequestStopBeforeRunReturnsImmediately) {
    tcpkv::log::set_level(tcpkv::log::Level::Warn);
    Store store;
    Metrics metrics;
    ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.stats_log_interval = std::chrono::seconds(0);
    Server server(config, store, metrics);
    server.request_stop();
    server.run();  // loop condition is false on entry
    EXPECT_TRUE(server.stop_requested());
}
