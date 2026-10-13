// m05_shared_weak_ptr.cpp
// shared_ptr reference counting, make_shared's single allocation, weak_ptr for cycles and caches.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m05_shared_weak_ptr.cpp
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

struct Session {
    std::string user;
    explicit Session(std::string u) : user(std::move(u)) { std::cout << "  Session(" << user << ")\n"; }
    ~Session() { std::cout << "  ~Session(" << user << ")\n"; }
};

struct Node {   // parent <-> child would be a cycle with two shared_ptrs
    std::string name;
    std::shared_ptr<Node> child;    // owning edge
    std::weak_ptr<Node> parent;     // non-owning back edge: breaks the cycle
    explicit Node(std::string n) : name(std::move(n)) {}
    ~Node() { std::cout << "  ~Node(" << name << ")\n"; }
};

class SessionCache {   // a cache that must NOT keep sessions alive on its own
public:
    std::shared_ptr<Session> get(const std::string& id) {
        auto it = cache_.find(id);
        if (it == cache_.end()) return nullptr;
        return it->second.lock();   // promotes weak -> shared if the object is still alive, else null
    }
    void put(const std::string& id, const std::shared_ptr<Session>& s) { cache_[id] = s; }   // stores a weak_ptr
private:
    std::unordered_map<std::string, std::weak_ptr<Session>> cache_;
};

int main() {
    std::cout << "sizeof(shared_ptr)=" << sizeof(std::shared_ptr<Session>)
              << " sizeof(unique_ptr)=" << sizeof(std::unique_ptr<Session>)
              << " sizeof(weak_ptr)=" << sizeof(std::weak_ptr<Session>) << '\n';

    auto s1 = std::make_shared<Session>("shakir");   // ONE heap allocation: control block + Session side by side
    std::cout << "use_count=" << s1.use_count() << '\n';
    {
        std::shared_ptr<Session> s2 = s1;              // atomic increment of the strong count
        std::cout << "use_count=" << s1.use_count() << '\n';
    }                                                   // atomic decrement; object survives (count 1)
    std::cout << "use_count=" << s1.use_count() << '\n';

    SessionCache cache;
    cache.put("S1", s1);
    std::cout << "cache hit? " << (cache.get("S1") != nullptr) << " use_count=" << s1.use_count() << '\n';
    s1.reset();                                          // last strong owner gone: ~Session runs NOW
    std::cout << "cache hit after reset? " << (cache.get("S1") != nullptr) << '\n';
    // Note: with make_shared the memory block itself lives until the weak_ptr in the cache also dies,
    // because the control block and the object share one allocation.

    {
        auto parent = std::make_shared<Node>("parent");
        parent->child = std::make_shared<Node>("child");
        parent->child->parent = parent;                  // weak: parent's strong count stays 1
        std::cout << "parent use_count=" << parent.use_count() << " (the weak back edge adds nothing)\n";
        if (auto p = parent->child->parent.lock()) std::cout << "child's parent is " << p->name << '\n';
    }   // both nodes destroyed here. With a shared_ptr back edge, both would leak forever (count never hits 0).

    std::cout << "end\n";
    return 0;
}
