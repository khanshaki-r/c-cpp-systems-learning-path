// m09_interfaces_composition.cpp
// Pure interfaces, dependency injection via composition, static polymorphism, the NVI idiom.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m09_interfaces_composition.cpp
#include <cctype>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// An interface: only pure virtual functions and a virtual destructor. No data members.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(const std::string& msg) = 0;
};

class ConsoleLogger : public Logger {
public:
    void log(const std::string& msg) override { std::cout << "[console] " << msg << '\n'; }
};

class BufferedLogger : public Logger {
public:
    void log(const std::string& msg) override { lines_.push_back(msg); }
    std::size_t pending() const { return lines_.size(); }
private:
    std::vector<std::string> lines_;
};

// Composition: Server HAS-A Logger, injected from outside. Swap implementations in tests without
// touching Server. Inheriting from ConsoleLogger instead would hard-wire one implementation forever.
class Server {
public:
    explicit Server(std::unique_ptr<Logger> logger) : logger_(std::move(logger)) {}
    void handle(const std::string& req) {
        logger_->log("handled " + req);   // one virtual call per request: negligible next to the I/O
        ++handled_;
    }
    int handled() const { return handled_; }
private:
    std::unique_ptr<Logger> logger_;   // owned; Server controls its lifetime
    int handled_ = 0;
};

// Static polymorphism: when the concrete type is known at compile time, a template gives the same
// flexibility with no vtable, no indirect call, and full inlining.
template <class Codec>
std::string encode_with(const Codec& codec, const std::string& payload) { return codec.encode(payload); }

struct UpperCodec {
    std::string encode(const std::string& s) const {
        std::string r = s;
        for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
};
struct ReverseCodec {
    std::string encode(const std::string& s) const { return std::string(s.rbegin(), s.rend()); }
};

// Non-Virtual Interface (NVI): the public entry point is non-virtual and enforces the protocol
// (logging, locking, timing); derived classes customise only the private hook.
class Task {
public:
    virtual ~Task() = default;
    void run() {
        std::cout << "begin\n";
        do_run();
        std::cout << "end\n";
    }
private:
    virtual void do_run() = 0;
};
class PingTask : public Task {
    void do_run() override { std::cout << "  ping\n"; }   // private override is fine: callers use run()
};

int main() {
    Server prod(std::make_unique<ConsoleLogger>());
    prod.handle("GET /balance");

    auto buffered = std::make_unique<BufferedLogger>();
    BufferedLogger* observer = buffered.get();   // non-owning raw pointer: valid exactly as long as 'test' lives
    Server test(std::move(buffered));
    test.handle("PUT /k");
    test.handle("GET /k");
    std::cout << "buffered pending=" << observer->pending() << " handled=" << test.handled() << '\n';

    std::cout << encode_with(UpperCodec{}, "abc") << ' ' << encode_with(ReverseCodec{}, "abc") << '\n';

    PingTask t;
    t.run();
    return 0;
}
