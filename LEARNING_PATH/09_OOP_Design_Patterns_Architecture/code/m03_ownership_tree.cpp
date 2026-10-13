// m03_ownership_tree.cpp
// Ownership design for a server: Server owns Listener and Connections; Connection owns
// its Session and in-flight Requests; everything else is an observer (raw pointer or
// reference) or a handle (integer id). No shared_ptr anywhere.
//
// This is a single-threaded simulation so the ownership story is visible without
// locking noise. Module 07 adds threads.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m03a m03_ownership_tree.cpp

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// A handle is an integer that names an object without pointing at it. The owner
// resolves it. If the object is gone, resolution fails cleanly instead of
// dereferencing freed memory.
// ---------------------------------------------------------------------------
struct ConnId {
    std::uint64_t value;
    bool operator==(ConnId o) const { return value == o.value; }
};
struct ConnIdHash {
    std::size_t operator()(ConnId id) const { return std::hash<std::uint64_t>{}(id.value); }
};

// ---------------------------------------------------------------------------
// Leaf objects. Request is a VALUE: it is created, processed, destroyed. It
// never outlives the Connection that holds it.
// ---------------------------------------------------------------------------
struct Request {
    std::uint32_t seq;
    std::string payload;
};

// A Session holds per-client state (auth, selected database, etc).
class Session {
public:
    explicit Session(std::string user) : user_(std::move(user)) {}
    const std::string& user() const { return user_; }
    void touch() { ++ops_; }
    std::uint32_t ops() const { return ops_; }

private:
    std::string user_;
    std::uint32_t ops_ = 0;
};

// Something that connections need from the server but do not own.
class Metrics {
public:
    void inc_requests() { ++requests_; }
    void inc_connections() { ++connections_; }
    void dec_connections() { --connections_; }
    std::uint64_t requests() const { return requests_; }
    std::int64_t connections() const { return connections_; }

private:
    std::uint64_t requests_ = 0;
    std::int64_t connections_ = 0;
};

// ---------------------------------------------------------------------------
// Connection OWNS its session (unique_ptr: optional + heap, lazily created on
// auth) and its queue of pending requests (vector of values). It OBSERVES
// Metrics via a reference because Metrics outlives every connection.
// ---------------------------------------------------------------------------
class Connection {
public:
    Connection(ConnId id, Metrics& metrics) : id_(id), metrics_(metrics) {
        metrics_.inc_connections();
    }
    ~Connection() { metrics_.dec_connections(); }

    // Non-copyable, non-movable: its address is handed out as an observer pointer
    // by the Server's map, and it owns resources with identity (in real code: an fd).
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ConnId id() const { return id_; }

    void authenticate(std::string user) { session_ = std::make_unique<Session>(std::move(user)); }
    Session* session() { return session_.get(); }  // may be null before auth: observer pointer

    void enqueue(Request r) { pending_.push_back(std::move(r)); }

    // Process one request. The Request lives in pending_ until we pop it; the
    // handler receives a const reference and cannot keep it.
    std::optional<std::string> process_one() {
        if (pending_.empty()) return std::nullopt;
        Request r = std::move(pending_.front());
        pending_.erase(pending_.begin());
        metrics_.inc_requests();
        if (session_) session_->touch();
        return "reply(" + std::to_string(r.seq) + ":" + r.payload + ")";
    }

    std::size_t pending() const { return pending_.size(); }

private:
    ConnId id_;
    Metrics& metrics_;                   // observer: Server guarantees Metrics outlives us
    std::unique_ptr<Session> session_;   // owned, optional
    std::vector<Request> pending_;       // owned, values
};

// ---------------------------------------------------------------------------
// Server OWNS Metrics (as a plain member: constructed first, destroyed last)
// and all Connections (unique_ptr in a map keyed by handle). It hands out
// ConnId handles, never Connection pointers, to code outside the server.
// ---------------------------------------------------------------------------
class Server {
public:
    ConnId accept() {
        ConnId id{next_id_++};
        // make_unique: if the Connection constructor throws, nothing leaks.
        conns_.emplace(id, std::make_unique<Connection>(id, metrics_));
        return id;
    }

    // Resolution: handle -> observer pointer, valid until the next call that may
    // close connections. Callers must not store it across such a call.
    Connection* find(ConnId id) {
        auto it = conns_.find(id);
        return it == conns_.end() ? nullptr : it->second.get();
    }

    void close(ConnId id) { conns_.erase(id); }  // unique_ptr destructor runs ~Connection

    // One scheduler tick: give every connection a chance to process one request.
    std::vector<std::string> tick() {
        std::vector<std::string> replies;
        for (auto& [id, conn] : conns_) {
            (void)id;
            if (auto r = conn->process_one()) replies.push_back(std::move(*r));
        }
        return replies;
    }

    const Metrics& metrics() const { return metrics_; }

private:
    // Declaration order IS destruction order (reversed). metrics_ is declared
    // before conns_, so it is destroyed AFTER conns_, so every ~Connection can
    // still call metrics_.dec_connections(). Reorder these and you have a
    // use-after-destroy at shutdown.
    Metrics metrics_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<ConnId, std::unique_ptr<Connection>, ConnIdHash> conns_;
};

int main() {
    Server server;

    ConnId c1 = server.accept();
    ConnId c2 = server.accept();

    if (Connection* c = server.find(c1)) {
        c->authenticate("shakir");
        c->enqueue(Request{1, "GET acct:1001"});
        c->enqueue(Request{2, "PUT acct:1001 500"});
    }
    if (Connection* c = server.find(c2)) {
        c->enqueue(Request{1, "PING"});
    }

    for (int round = 0; round < 3; ++round) {
        auto replies = server.tick();
        std::printf("tick %d: %zu replies\n", round, replies.size());
        for (auto& r : replies) std::printf("  %s\n", r.c_str());
    }

    // Close c2. Any stale Connection* we held for c2 would now dangle;
    // the ConnId does not: find() simply returns nullptr.
    server.close(c2);
    std::printf("find(c2) after close -> %s\n", server.find(c2) ? "still there (BUG)" : "nullptr (correct)");

    const Metrics& m = server.metrics();
    std::printf("requests=%llu connections=%lld\n",
                static_cast<unsigned long long>(m.requests()), static_cast<long long>(m.connections()));

    // At scope exit: ~Server -> ~conns_ (each ~Connection calls metrics_.dec) -> ~metrics_.
    return 0;
}
