// m04_prototype_pool.cpp
// Prototype: create new objects by copying a configured template instead of
// re-running expensive setup. Object pool: recycle expensive-to-create objects
// (database connections) rather than constructing per request.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m04c m04_prototype_pool.cpp

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PROTOTYPE. The classic form: virtual clone() so you can copy through a base
// pointer (copy constructors are not virtual). In C: a memcpy of a template
// struct plus deep-copying any pointers, done by hand per type.
// ---------------------------------------------------------------------------
class MessageTemplate {
public:
    virtual ~MessageTemplate() = default;
    virtual std::unique_ptr<MessageTemplate> clone() const = 0;   // covariant copy
    virtual void set_field(const std::string& k, const std::string& v) = 0;
    virtual std::string render() const = 0;
};

// An ISO-8583-like message: parsing the template spec is "expensive"; cloning
// a parsed template is cheap.
class Iso8583Message final : public MessageTemplate {
public:
    explicit Iso8583Message(std::string mti, std::map<std::string, std::string> defaults)
        : mti_(std::move(mti)), fields_(std::move(defaults)) {
        std::printf("  (expensive parse of template %s)\n", mti_.c_str());
    }

    std::unique_ptr<MessageTemplate> clone() const override {
        // Uses the implicitly generated copy constructor: no "expensive parse".
        return std::make_unique<Iso8583Message>(*this);
    }
    void set_field(const std::string& k, const std::string& v) override { fields_[k] = v; }
    std::string render() const override {
        std::string out = mti_;
        for (auto& [k, v] : fields_) out += "|" + k + "=" + v;
        return out;
    }

private:
    // The implicit copy constructor stays public so make_unique<Iso8583Message>(*this)
    // works. Callers holding a MessageTemplate* cannot copy by value anyway: that
    // would slice. clone() is the only way to copy through the base pointer.
    std::string mti_;
    std::map<std::string, std::string> fields_;
};

// The modern alternative when you do not need polymorphism: a plain VALUE type.
// Copying a value IS the prototype pattern; no clone() needed.
struct ConnectionParams {
    std::string host = "127.0.0.1";
    int port = 5432;
    int connect_timeout_ms = 5000;
    bool tls = false;
};

// ---------------------------------------------------------------------------
// OBJECT POOL for connections. Creation is slow (TCP + auth handshake), so keep
// a bounded set and lend them out. A unique_ptr with a custom deleter returns
// the connection automatically, including on exceptions.
// ---------------------------------------------------------------------------
class DbConnection {
public:
    explicit DbConnection(int id, const ConnectionParams& p) : id_(id) {
        std::printf("  (slow connect #%d to %s:%d)\n", id_, p.host.c_str(), p.port);
    }
    int id() const { return id_; }
    std::string query(const std::string& sql) { return "conn#" + std::to_string(id_) + " ran [" + sql + "]"; }

private:
    int id_;
};

class ConnectionPool {
public:
    struct Returner {
        ConnectionPool* pool;
        void operator()(DbConnection* c) const { pool->give_back(c); }
    };
    using Lease = std::unique_ptr<DbConnection, Returner>;

    ConnectionPool(ConnectionParams params, std::size_t size) : params_(std::move(params)) {
        for (std::size_t i = 0; i < size; ++i) {
            all_.push_back(std::make_unique<DbConnection>(static_cast<int>(i + 1), params_));
            idle_.push_back(all_.back().get());
        }
    }

    // Returns nullopt when exhausted. Production code would wait on a
    // condition_variable with a timeout (Module 07). Here we fail fast.
    std::optional<Lease> acquire() {
        std::lock_guard<std::mutex> lk(mu_);
        if (idle_.empty()) return std::nullopt;
        DbConnection* c = idle_.back();
        idle_.pop_back();
        return Lease(c, Returner{this});
    }

    std::size_t idle() {
        std::lock_guard<std::mutex> lk(mu_);
        return idle_.size();
    }

private:
    void give_back(DbConnection* c) {
        std::lock_guard<std::mutex> lk(mu_);
        idle_.push_back(c);
    }

    ConnectionParams params_;
    std::mutex mu_;
    std::vector<std::unique_ptr<DbConnection>> all_;   // owns
    std::vector<DbConnection*> idle_;                  // observes
};

int main() {
    // --- prototype ---
    std::printf("prototype:\n");
    Iso8583Message proto("0200", {{"PAN", "************1234"}, {"CUR", "356"}});
    auto a = proto.clone();
    auto b = proto.clone();        // no "expensive parse" printed for a or b
    a->set_field("AMT", "50000");
    b->set_field("AMT", "125");
    std::printf("  a: %s\n  b: %s\n", a->render().c_str(), b->render().c_str());

    // value-type prototype: just copy
    ConnectionParams base;          // defaults
    ConnectionParams replica = base;
    replica.host = "10.0.0.2";
    std::printf("  base=%s replica=%s\n", base.host.c_str(), replica.host.c_str());

    // --- pool ---
    std::printf("pool:\n");
    ConnectionPool pool(base, 2);
    {
        auto l1 = pool.acquire();
        auto l2 = pool.acquire();
        auto l3 = pool.acquire();
        std::printf("  l1=%s l2=%s l3=%s idle=%zu\n", l1 ? "ok" : "none", l2 ? "ok" : "none",
                    l3 ? "ok" : "none", pool.idle());
        if (l1) std::printf("  %s\n", (*l1)->query("SELECT 1").c_str());
    }   // l1, l2 returned by the deleter here
    std::printf("  idle after scope=%zu\n", pool.idle());
    return 0;
}
