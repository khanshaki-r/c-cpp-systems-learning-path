// m02_solid_di.cpp
// The five SOLID principles in one small "request processing" slice of a server.
// Each section shows the principle being respected; the comments say what the
// violation would have looked like.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m02 m02_solid_di.cpp

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// S: Single Responsibility. One class, one reason to change.
// BAD would be: class RequestHandler { parse(); validate(); talkToDb(); writeLog(); }
// Here each concern is its own small type.
// ---------------------------------------------------------------------------
struct Request {
    std::string op;      // "GET" or "PUT"
    std::string key;
    std::string value;   // empty for GET
};

class RequestParser {
public:
    // Parsing knows about the wire format and nothing else.
    std::optional<Request> parse(std::string_view line) const {
        Request r;
        auto sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) return std::nullopt;
        r.op = std::string(line.substr(0, sp1));
        auto rest = line.substr(sp1 + 1);
        auto sp2 = rest.find(' ');
        if (sp2 == std::string_view::npos) {
            r.key = std::string(rest);
        } else {
            r.key = std::string(rest.substr(0, sp2));
            r.value = std::string(rest.substr(sp2 + 1));
        }
        return r;
    }
};

// ---------------------------------------------------------------------------
// D: Dependency Inversion. High-level policy (the service) depends on an
// abstraction (Store, Logger), not on concrete classes. Concrete classes are
// injected through the constructor. The service does not know Postgres exists.
// ---------------------------------------------------------------------------
class Store {
public:
    virtual ~Store() = default;
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
};

// ---------------------------------------------------------------------------
// I: Interface Segregation. Clients should not depend on functions they do not
// use. A Logger that also had flush(), rotate(), set_level() would force every
// fake logger in tests to implement them. Keep the interface minimal.
// ---------------------------------------------------------------------------
class Logger {
public:
    virtual ~Logger() = default;
    virtual void info(std::string_view msg) = 0;
};

// Optional capability expressed as a SEPARATE interface, so only those who
// need it depend on it.
class Flushable {
public:
    virtual ~Flushable() = default;
    virtual void flush() = 0;
};

// ---------------------------------------------------------------------------
// Concrete implementations. These are the "details" the service must not see.
// ---------------------------------------------------------------------------
class InMemoryStore final : public Store {
public:
    void put(const std::string& key, const std::string& value) override { data_[key] = value; }
    std::optional<std::string> get(const std::string& key) const override {
        auto it = data_.find(key);
        return it == data_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }

private:
    std::map<std::string, std::string> data_;
};

class StdoutLogger final : public Logger, public Flushable {
public:
    void info(std::string_view msg) override { std::printf("[info] %.*s\n", static_cast<int>(msg.size()), msg.data()); }
    void flush() override { std::fflush(stdout); }
};

// A test double. Possible only because the service depends on the interface.
class RecordingLogger final : public Logger {
public:
    void info(std::string_view msg) override { lines.emplace_back(msg); }
    std::vector<std::string> lines;
};

// ---------------------------------------------------------------------------
// O: Open/Closed. Adding a new operation ("DEL", "INCR") must not require editing
// KvService. Operations are registered in a table keyed by name. New operations
// are new classes (or lambdas) added at wiring time.
// ---------------------------------------------------------------------------
class Operation {
public:
    virtual ~Operation() = default;
    virtual std::string apply(Store& store, const Request& r) = 0;
};

class GetOp final : public Operation {
public:
    std::string apply(Store& store, const Request& r) override {
        auto v = store.get(r.key);
        return v ? "VALUE " + *v : "NOT_FOUND";
    }
};

class PutOp final : public Operation {
public:
    std::string apply(Store& store, const Request& r) override {
        store.put(r.key, r.value);
        return "OK";
    }
};

// ---------------------------------------------------------------------------
// L: Liskov Substitution. Anything that IS-A Store must behave like a Store.
// A "ReadOnlyStore" whose put() throws would violate this: callers of Store
// may not expect put() to fail by type. Model read-only as a different
// interface (or return a status) instead. We demonstrate the compliant way:
// a caching store that still honours the contract.
// ---------------------------------------------------------------------------
class WriteThroughCache final : public Store {
public:
    explicit WriteThroughCache(Store& backing) : backing_(backing) {}
    void put(const std::string& key, const std::string& value) override {
        backing_.put(key, value);   // contract: after put, get returns value. Still true.
        cache_[key] = value;
    }
    std::optional<std::string> get(const std::string& key) const override {
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        auto v = backing_.get(key);
        if (v) cache_[key] = *v;
        return v;
    }

private:
    Store& backing_;                                    // non-owning: caller guarantees lifetime
    mutable std::map<std::string, std::string> cache_;  // mutable: logically const get() may fill cache
};

// ---------------------------------------------------------------------------
// The service. Depends only on abstractions. Everything is injected.
// ---------------------------------------------------------------------------
class KvService {
public:
    KvService(Store& store, Logger& log) : store_(store), log_(log) {}

    void register_op(std::string name, std::unique_ptr<Operation> op) {
        ops_[std::move(name)] = std::move(op);
    }

    std::string handle(std::string_view line) {
        auto req = parser_.parse(line);
        if (!req) {
            log_.info("bad request");
            return "ERR bad request";
        }
        auto it = ops_.find(req->op);
        if (it == ops_.end()) {
            log_.info("unknown op " + req->op);
            return "ERR unknown op";
        }
        auto reply = it->second->apply(store_, *req);
        log_.info(req->op + " " + req->key + " -> " + reply);
        return reply;
    }

private:
    Store& store_;
    Logger& log_;
    RequestParser parser_;
    std::map<std::string, std::unique_ptr<Operation>> ops_;  // service OWNS the operations
};

// ---------------------------------------------------------------------------
// Composition root: the ONE place that knows about concrete types.
// In a real server this is main() or a wire() function.
// ---------------------------------------------------------------------------
int main() {
    // Production wiring.
    InMemoryStore backing;
    WriteThroughCache cache(backing);
    StdoutLogger logger;
    KvService svc(cache, logger);
    svc.register_op("GET", std::make_unique<GetOp>());
    svc.register_op("PUT", std::make_unique<PutOp>());

    std::printf("%s\n", svc.handle("PUT acct:1001 balance=500").c_str());
    std::printf("%s\n", svc.handle("GET acct:1001").c_str());
    std::printf("%s\n", svc.handle("GET acct:9999").c_str());
    std::printf("%s\n", svc.handle("DEL acct:1001").c_str());
    logger.flush();

    // Test wiring: same service, fake logger, no stdout noise.
    InMemoryStore test_store;
    RecordingLogger rec;
    KvService test_svc(test_store, rec);
    test_svc.register_op("PUT", std::make_unique<PutOp>());
    test_svc.handle("PUT k v");
    std::printf("test logged %zu line(s); first = \"%s\"\n", rec.lines.size(),
                rec.lines.empty() ? "" : rec.lines.front().c_str());
    return 0;
}
