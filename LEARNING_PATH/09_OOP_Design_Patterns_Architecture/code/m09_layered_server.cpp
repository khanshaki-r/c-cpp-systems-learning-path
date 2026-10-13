// m09_layered_server.cpp
// A LAYERED SERVER in one file, so the whole shape is visible at once. In a real
// repository each section below is its own directory and library (see module
// 09, section 2.2). The layers, top to bottom:
//
//   transport  : moves bytes. Knows sockets (here: a fake). Knows nothing of KV.
//   protocol   : bytes <-> Request/Response values. Pure functions, no I/O.
//   service    : business rules (validation, limits, metrics). Owns the
//                Storage INTERFACE it needs (dependency inversion).
//   storage    : implements that interface (memory map; in production a DB).
//   app        : the composition root. The only place that knows every
//                concrete type and wires them together.
//
// Dependency direction: transport -> protocol -> service <- storage.
// Storage depends UP on the interface the service declares; the service never
// includes a storage header. That is what lets tests swap the database for a
// map and lets you swap Berkeley DB for YugabyteDB without touching rules.
//
// Part 2 shows the same idea applied to legacy C business logic: one
// 80-line-style function doing parse + validate + SQL + format becomes
// separate, testable pieces, and a characterization test proves the new code
// answers exactly like the old code.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m09 m09_layered_server.cpp

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// ===========================================================================
// common/ : vocabulary types every layer may use. No dependencies.
// ===========================================================================
namespace common {

// Error POLICY: expected failures (bad input, not found, limits) are values.
// Exceptions are reserved for broken invariants and start-up failures.
enum class Errc { NotFound, InvalidArgument, TooLarge, Unavailable, Internal };

struct Error {
    Errc code;
    std::string message;
};

// Minimal "expected": either a T or an Error. C++23 has std::expected.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T v) : v_(std::move(v)) {}
    Result(Error e) : v_(std::move(e)) {}
    bool ok() const noexcept { return std::holds_alternative<T>(v_); }
    const T& value() const { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }
private:
    std::variant<T, Error> v_;
};

struct Unit {};   // "success, no value" for Result<Unit>

enum class Level { Debug, Info, Warn, Error };

// Logging is an INTERFACE passed in, not a global. Tests capture it.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(Level, std::string_view msg) = 0;
};

}  // namespace common

// ===========================================================================
// config/ : read once at start-up, validated, then immutable.
// ===========================================================================
namespace config {

struct Config {
    std::uint16_t port = 7379;
    std::size_t max_key_bytes = 256;
    std::size_t max_value_bytes = 1 << 20;
    int workers = 4;
};

// Unknown keys and bad values fail start-up loudly; never fall back silently.
common::Result<Config> parse(const std::map<std::string, std::string>& kv) {
    Config c;
    for (const auto& [k, v] : kv) {
        long n = 0;
        try { n = std::stol(v); } catch (...) { return common::Error{common::Errc::InvalidArgument, k + ": not a number"}; }
        if (k == "port" && n > 0 && n < 65536)              c.port = static_cast<std::uint16_t>(n);
        else if (k == "max_value_bytes" && n > 0)           c.max_value_bytes = static_cast<std::size_t>(n);
        else if (k == "workers" && n > 0 && n <= 256)       c.workers = static_cast<int>(n);
        else return common::Error{common::Errc::InvalidArgument, "bad config entry " + k + "=" + v};
    }
    return c;
}

}  // namespace config

// ===========================================================================
// service/ : business rules. Declares the Storage port it needs.
// ===========================================================================
namespace service {

// The PORT (interface) is owned by the service layer. Storage implements it.
class Storage {
public:
    virtual ~Storage() = default;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual void put(const std::string& key, std::string value) = 0;
    virtual bool erase(const std::string& key) = 0;
};

class KvService {
public:
    KvService(Storage& store, common::Logger& log, const config::Config& cfg)
        : store_(store), log_(log), cfg_(cfg) {}

    common::Result<std::string> get(const std::string& key) const {
        if (auto e = check_key(key)) return *e;
        auto v = store_.get(key);
        if (!v) return common::Error{common::Errc::NotFound, "no such key"};
        return *v;
    }

    common::Result<common::Unit> set(const std::string& key, std::string value) {
        if (auto e = check_key(key)) return *e;
        if (value.size() > cfg_.max_value_bytes)
            return common::Error{common::Errc::TooLarge, "value too large"};
        store_.put(key, std::move(value));
        return common::Unit{};
    }

    common::Result<bool> del(const std::string& key) {
        if (auto e = check_key(key)) return *e;
        const bool removed = store_.erase(key);
        if (removed) log_.log(common::Level::Debug, "deleted " + key);
        return removed;
    }

private:
    std::optional<common::Error> check_key(const std::string& key) const {
        if (key.empty() || key.size() > cfg_.max_key_bytes)
            return common::Error{common::Errc::InvalidArgument, "invalid key"};
        return std::nullopt;
    }
    Storage& store_;               // not owned: the composition root owns it
    common::Logger& log_;
    const config::Config& cfg_;
};

}  // namespace service

// ===========================================================================
// storage/ : implements service::Storage. Depends on service's header only.
// ===========================================================================
namespace storage {

class MemoryStorage final : public service::Storage {
public:
    std::optional<std::string> get(const std::string& k) const override {
        std::shared_lock<std::shared_mutex> g(m_);
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    void put(const std::string& k, std::string v) override {
        std::unique_lock<std::shared_mutex> g(m_);
        map_[k] = std::move(v);
    }
    bool erase(const std::string& k) override {
        std::unique_lock<std::shared_mutex> g(m_);
        return map_.erase(k) > 0;
    }
private:
    mutable std::shared_mutex m_;
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace storage

// ===========================================================================
// protocol/ : text <-> values. Pure: no sockets, no store, no logger.
// ===========================================================================
namespace protocol {

struct Get  { std::string key; };
struct Set  { std::string key; std::string value; };
struct Del  { std::string key; };
struct Ping {};
using Request = std::variant<Get, Set, Del, Ping>;

// Parsing failure is a protocol error, distinct from a service error.
common::Result<Request> parse(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, a, b;
    in >> cmd >> a >> b;
    if (cmd == "PING" && a.empty()) return Request{Ping{}};
    if (cmd == "GET" && !a.empty() && b.empty()) return Request{Get{a}};
    if (cmd == "DEL" && !a.empty() && b.empty()) return Request{Del{a}};
    if (cmd == "SET" && !a.empty() && !b.empty()) return Request{Set{a, b}};
    return common::Error{common::Errc::InvalidArgument, "syntax error"};
}

std::string encode_error(const common::Error& e) {
    // ONE place maps internal error codes to wire text. Internal details
    // (SQL text, file paths) never leak to the client.
    switch (e.code) {
        case common::Errc::NotFound:        return "$-1";
        case common::Errc::InvalidArgument: return "-ERR " + e.message;
        case common::Errc::TooLarge:        return "-ERR " + e.message;
        case common::Errc::Unavailable:     return "-ERR try again";
        case common::Errc::Internal:        return "-ERR internal error";
    }
    return "-ERR internal error";
}

}  // namespace protocol

// ===========================================================================
// transport/ : moves lines. Here a fake; in tcpkv a Socket.
// ===========================================================================
namespace transport {

class Connection {
public:
    virtual ~Connection() = default;
    virtual std::optional<std::string> read_line() = 0;
    virtual void write_line(const std::string&) = 0;
};

class ScriptedConnection final : public Connection {
public:
    explicit ScriptedConnection(std::vector<std::string> in) : in_(std::move(in)) {}
    std::optional<std::string> read_line() override {
        if (pos_ == in_.size()) return std::nullopt;   // peer closed
        return in_[pos_++];
    }
    void write_line(const std::string& s) override { out.push_back(s); }
    std::vector<std::string> out;
private:
    std::vector<std::string> in_;
    std::size_t pos_ = 0;
};

}  // namespace transport

// ===========================================================================
// app/session : the glue loop. Depends on transport, protocol and service
// INTERFACES; still knows no concrete storage.
// ===========================================================================
namespace app {

template <class... F> struct overloaded : F... { using F::operator()...; };
template <class... F> overloaded(F...) -> overloaded<F...>;

void serve(transport::Connection& conn, service::KvService& svc) {
    while (auto line = conn.read_line()) {
        auto req = protocol::parse(*line);
        if (!req.ok()) { conn.write_line(protocol::encode_error(req.error())); continue; }
        std::string reply = std::visit(overloaded{
            [&](const protocol::Ping&) -> std::string { return "+PONG"; },
            [&](const protocol::Get& g) -> std::string {
                auto r = svc.get(g.key);
                return r.ok() ? "$" + r.value() : protocol::encode_error(r.error());
            },
            [&](const protocol::Set& s) -> std::string {
                auto r = svc.set(s.key, s.value);
                return r.ok() ? "+OK" : protocol::encode_error(r.error());
            },
            [&](const protocol::Del& d) -> std::string {
                auto r = svc.del(d.key);
                return r.ok() ? ":" + std::to_string(r.value() ? 1 : 0) : protocol::encode_error(r.error());
            },
        }, req.value());
        conn.write_line(reply);
    }
}

class StderrLogger final : public common::Logger {
public:
    void log(common::Level l, std::string_view m) override {
        if (l >= common::Level::Info) std::fprintf(stderr, "%.*s\n", static_cast<int>(m.size()), m.data());
    }
};

class CapturingLogger final : public common::Logger {
public:
    void log(common::Level, std::string_view m) override { lines.emplace_back(m); }
    std::vector<std::string> lines;
};

}  // namespace app

// ===========================================================================
// PART 2: legacy C business logic, and the same logic split into modules.
// ===========================================================================
namespace legacy {

// What a 2008-era loan server handler looks like: parse, validate, "SQL",
// business rule and formatting in one function with globals. Kept as-is for
// the characterization test. (Simplified: real ones are 800 lines.)
static int g_customer_age[100];        // stands in for "SELECT age FROM customer"
static long g_customer_income[100];

int process_loan_request(const char* req, char* resp, std::size_t resp_len) {
    int cust = 0;
    long amount = 0;
    char product[16] = {0};
    if (std::sscanf(req, "%d|%15[^|]|%ld", &cust, product, &amount) != 3) {
        std::snprintf(resp, resp_len, "E01|BAD FORMAT");
        return -1;
    }
    if (cust < 0 || cust >= 100) { std::snprintf(resp, resp_len, "E02|NO CUSTOMER"); return -1; }
    int age = g_customer_age[cust];
    long income = g_customer_income[cust];
    if (age < 21 || age > 60) { std::snprintf(resp, resp_len, "E03|AGE"); return -1; }
    long limit = std::strcmp(product, "PL") == 0 ? income * 10 : income * 60;
    if (amount > limit) { std::snprintf(resp, resp_len, "E04|LIMIT %ld", limit); return -1; }
    std::snprintf(resp, resp_len, "OK|%d|%ld", cust, amount);
    return 0;
}

}  // namespace legacy

namespace loan {

// 1. Data access behind an interface (later: a stored procedure call).
struct Customer { int age; long monthly_income; };
class CustomerRepository {
public:
    virtual ~CustomerRepository() = default;
    virtual std::optional<Customer> find(int id) const = 0;
};

// 2. Wire format isolated.
struct LoanRequest { int customer; std::string product; long amount; };
std::optional<LoanRequest> parse(const std::string& s) {
    LoanRequest r{};
    char product[16] = {0};
    if (std::sscanf(s.c_str(), "%d|%15[^|]|%ld", &r.customer, product, &r.amount) != 3) return std::nullopt;
    r.product = product;
    return r;
}

// 3. Rules as pure functions: no I/O, trivially unit-tested.
struct Decision { bool ok; std::string code; std::string detail; };
long limit_for(const std::string& product, long income) { return product == "PL" ? income * 10 : income * 60; }
Decision decide(const LoanRequest& r, const Customer& c) {
    if (c.age < 21 || c.age > 60) return {false, "E03", "AGE"};
    const long limit = limit_for(r.product, c.monthly_income);
    if (r.amount > limit) return {false, "E04", "LIMIT " + std::to_string(limit)};
    return {true, "OK", std::to_string(r.customer) + "|" + std::to_string(r.amount)};
}

// 4. The service orchestrates; it is the only piece that touches everything.
class LoanService {
public:
    explicit LoanService(const CustomerRepository& repo) : repo_(repo) {}
    std::string handle(const std::string& wire) const {
        auto req = parse(wire);
        if (!req) return "E01|BAD FORMAT";
        auto cust = repo_.find(req->customer);
        if (!cust) return "E02|NO CUSTOMER";
        Decision d = decide(*req, *cust);
        return d.code + "|" + d.detail;
    }
private:
    const CustomerRepository& repo_;
};

// Adapter over the legacy globals, so old and new read the SAME data.
class LegacyTableRepository final : public CustomerRepository {
public:
    std::optional<Customer> find(int id) const override {
        if (id < 0 || id >= 100) return std::nullopt;
        return Customer{legacy::g_customer_age[id], legacy::g_customer_income[id]};
    }
};

}  // namespace loan

// ===========================================================================
// main = composition root + self-checks.
// ===========================================================================
int main() {
    // --- Part 1: wire the layers.
    auto cfg = config::parse({{"port", "7400"}, {"max_value_bytes", "8"}});
    assert(cfg.ok() && cfg.value().port == 7400);
    assert(!config::parse({{"prot", "1"}}).ok());                    // typo caught at start-up
    assert(!config::parse({{"workers", "lots"}}).ok());

    storage::MemoryStorage store;                                     // concrete: only main sees it
    app::CapturingLogger log;
    service::KvService svc(store, log, cfg.value());

    transport::ScriptedConnection conn({"PING", "SET a hello", "GET a", "GET zz", "SET b waytoolongvalue",
                                        "DEL a", "DEL a", "FROB", "GET"});
    app::serve(conn, svc);
    const std::vector<std::string> expect{"+PONG", "+OK", "$hello", "$-1", "-ERR value too large",
                                          ":1", ":0", "-ERR syntax error", "-ERR syntax error"};
    assert(conn.out == expect);
    assert(log.lines.size() == 1 && log.lines[0] == "deleted a");

    // --- Part 2: characterization test. Feed the SAME inputs to legacy and
    // new code; every answer must match byte for byte before the switch-over.
    for (int i = 0; i < 100; ++i) {
        legacy::g_customer_age[i] = 18 + (i * 7) % 50;
        legacy::g_customer_income[i] = 20000 + 1000L * i;
    }
    loan::LegacyTableRepository repo;
    loan::LoanService svc2(repo);
    const char* inputs[] = {"5|PL|100000", "5|HL|5000000", "7|PL|9999999", "200|PL|1", "garbage",
                            "42|HL|100", "3|PL|250000", "99|HL|3000000"};
    for (const char* in : inputs) {
        char buf[64];
        legacy::process_loan_request(in, buf, sizeof buf);
        assert(svc2.handle(in) == std::string(buf));
    }

    // And the pure rule is testable without any repository at all.
    assert(loan::decide({1, "PL", 500}, {30, 100}).ok);
    assert(loan::decide({1, "PL", 1001}, {30, 100}).code == "E04");

    app::StderrLogger real;
    real.log(common::Level::Info, "m09_layered_server: all checks passed");
    return 0;
}
