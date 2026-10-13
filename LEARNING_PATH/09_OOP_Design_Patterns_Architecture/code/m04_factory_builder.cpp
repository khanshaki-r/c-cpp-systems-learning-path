// m04_factory_builder.cpp
// Creational patterns: factory function, abstract factory, builder.
// Scenario: a server that can be configured to use different storage backends
// and whose configuration is assembled step by step.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m04a m04_factory_builder.cpp

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// The product interface.
// ---------------------------------------------------------------------------
class Store {
public:
    virtual ~Store() = default;
    virtual void put(const std::string& k, const std::string& v) = 0;
    virtual std::optional<std::string> get(const std::string& k) const = 0;
    virtual std::string name() const = 0;
};

class MemoryStore final : public Store {
public:
    void put(const std::string& k, const std::string& v) override { m_[k] = v; }
    std::optional<std::string> get(const std::string& k) const override {
        auto it = m_.find(k);
        return it == m_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::string name() const override { return "memory"; }

private:
    std::map<std::string, std::string> m_;
};

// Stands in for a Berkeley DB or Postgres backed store.
class FileStore final : public Store {
public:
    explicit FileStore(std::string path) : path_(std::move(path)) {}
    void put(const std::string& k, const std::string& v) override { m_[k] = v; }
    std::optional<std::string> get(const std::string& k) const override {
        auto it = m_.find(k);
        return it == m_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::string name() const override { return "file:" + path_; }

private:
    std::string path_;
    std::map<std::string, std::string> m_;
};

// ---------------------------------------------------------------------------
// 1. Factory FUNCTION. The simplest and most common form. In C this is
//    `store* store_create(const char* kind, ...)` with a switch inside.
//    The caller gets a Store without naming the concrete class.
// ---------------------------------------------------------------------------
std::unique_ptr<Store> make_store(std::string_view kind, std::string_view arg) {
    if (kind == "memory") return std::make_unique<MemoryStore>();
    if (kind == "file") return std::make_unique<FileStore>(std::string(arg));
    throw std::invalid_argument("unknown store kind: " + std::string(kind));
}

// ---------------------------------------------------------------------------
// 1b. Registry-based factory: open for extension. New backends register a
//     creator at startup; make_store never changes. In C: a table of
//     {name, create_fn} you append to.
// ---------------------------------------------------------------------------
class StoreRegistry {
public:
    using Creator = std::function<std::unique_ptr<Store>(std::string_view arg)>;

    void add(std::string kind, Creator c) { creators_[std::move(kind)] = std::move(c); }

    std::unique_ptr<Store> create(std::string_view kind, std::string_view arg) const {
        auto it = creators_.find(std::string(kind));
        if (it == creators_.end()) throw std::invalid_argument("unregistered: " + std::string(kind));
        return it->second(arg);
    }

private:
    std::map<std::string, Creator> creators_;
};

// ---------------------------------------------------------------------------
// 2. ABSTRACT FACTORY: a family of related products that must be consistent.
//    E.g. a storage "stack" = store + lock manager + wal writer that must all
//    be from the same backend. One factory object creates all of them.
// ---------------------------------------------------------------------------
class LockManager {
public:
    virtual ~LockManager() = default;
    virtual std::string describe() const = 0;
};
class MemoryLocks final : public LockManager {
public:
    std::string describe() const override { return "in-process mutex table"; }
};
class FileLocks final : public LockManager {
public:
    std::string describe() const override { return "fcntl() byte-range locks"; }
};

class StorageFactory {
public:
    virtual ~StorageFactory() = default;
    virtual std::unique_ptr<Store> make_store() const = 0;
    virtual std::unique_ptr<LockManager> make_locks() const = 0;
};

class MemoryStorageFactory final : public StorageFactory {
public:
    std::unique_ptr<Store> make_store() const override { return std::make_unique<MemoryStore>(); }
    std::unique_ptr<LockManager> make_locks() const override { return std::make_unique<MemoryLocks>(); }
};

class FileStorageFactory final : public StorageFactory {
public:
    explicit FileStorageFactory(std::string dir) : dir_(std::move(dir)) {}
    std::unique_ptr<Store> make_store() const override { return std::make_unique<FileStore>(dir_ + "/data.db"); }
    std::unique_ptr<LockManager> make_locks() const override { return std::make_unique<FileLocks>(); }

private:
    std::string dir_;
};

// ---------------------------------------------------------------------------
// 3. BUILDER: assemble a complex, validated value step by step with a fluent
//    interface. The product is an immutable ServerConfig. In C you would fill a
//    struct and call config_validate(); the builder makes "validated" a type.
// ---------------------------------------------------------------------------
struct ServerConfig {
    std::string bind_addr;
    std::uint16_t port;
    int worker_threads;
    std::chrono::milliseconds idle_timeout;
    std::size_t max_connections;
    std::string store_kind;
    std::string store_arg;
};

class ServerConfigBuilder {
public:
    // Each setter returns *this so calls chain. Returning by reference (not
    // value) avoids copies; returning by value would allow temporaries to chain
    // too, at a copy cost per step.
    ServerConfigBuilder& bind(std::string addr, std::uint16_t port) {
        cfg_.bind_addr = std::move(addr);
        cfg_.port = port;
        return *this;
    }
    ServerConfigBuilder& workers(int n) { cfg_.worker_threads = n; return *this; }
    ServerConfigBuilder& idle_timeout(std::chrono::milliseconds t) { cfg_.idle_timeout = t; return *this; }
    ServerConfigBuilder& max_connections(std::size_t n) { cfg_.max_connections = n; return *this; }
    ServerConfigBuilder& store(std::string kind, std::string arg = "") {
        cfg_.store_kind = std::move(kind);
        cfg_.store_arg = std::move(arg);
        return *this;
    }

    // build() is the only way to get a ServerConfig, and it validates. Callers
    // can therefore trust any ServerConfig they are handed.
    ServerConfig build() const {
        if (cfg_.port == 0) throw std::invalid_argument("port must be set");
        if (cfg_.worker_threads <= 0) throw std::invalid_argument("workers must be > 0");
        if (cfg_.store_kind.empty()) throw std::invalid_argument("store kind must be set");
        if (cfg_.max_connections == 0) throw std::invalid_argument("max_connections must be > 0");
        return cfg_;
    }

private:
    // Sensible defaults live in one place.
    ServerConfig cfg_{"0.0.0.0", 0, 4, std::chrono::milliseconds(30000), 1024, "", ""};
};

int main() {
    // Factory function.
    auto s1 = make_store("memory", "");
    auto s2 = make_store("file", "/var/lib/tcpkv/data.db");
    std::printf("factory fn: %s, %s\n", s1->name().c_str(), s2->name().c_str());

    // Registry factory: extend without editing make_store.
    StoreRegistry reg;
    reg.add("memory", [](std::string_view) { return std::make_unique<MemoryStore>(); });
    reg.add("file", [](std::string_view arg) { return std::make_unique<FileStore>(std::string(arg)); });
    auto s3 = reg.create("file", "/tmp/x.db");
    std::printf("registry:   %s\n", s3->name().c_str());

    // Abstract factory: a consistent family.
    std::unique_ptr<StorageFactory> fac = std::make_unique<FileStorageFactory>("/var/lib/tcpkv");
    auto store = fac->make_store();
    auto locks = fac->make_locks();
    std::printf("abstract:   %s with %s\n", store->name().c_str(), locks->describe().c_str());

    // Builder.
    ServerConfig cfg = ServerConfigBuilder{}
                           .bind("127.0.0.1", 7000)
                           .workers(8)
                           .idle_timeout(std::chrono::milliseconds(60000))
                           .store("file", "/var/lib/tcpkv/data.db")
                           .build();
    std::printf("config:     %s:%u workers=%d idle=%lldms max_conn=%zu store=%s(%s)\n", cfg.bind_addr.c_str(),
                static_cast<unsigned>(cfg.port), cfg.worker_threads,
                static_cast<long long>(cfg.idle_timeout.count()), cfg.max_connections, cfg.store_kind.c_str(),
                cfg.store_arg.c_str());

    // Builder catches mistakes at the one choke point.
    try {
        ServerConfigBuilder{}.workers(2).build();
    } catch (const std::invalid_argument& e) {
        std::printf("rejected:   %s\n", e.what());
    }
    return 0;
}
