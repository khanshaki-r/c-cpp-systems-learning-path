// m06_template_method_nvi.cpp
// TEMPLATE METHOD: a base class defines the skeleton of an algorithm and lets
// subclasses fill in steps. NVI (Non-Virtual Interface) is the C++ idiom for
// doing it safely: the public function is non-virtual and fixed; the steps are
// private virtual hooks. Callers cannot bypass the skeleton; subclasses cannot
// forget the invariant parts (locking, logging, validation).
//
// Scenario: every request handler must: validate, log start, run, log end,
// convert exceptions to an error reply. Only "run" varies.
//
// C equivalent: a fixed driver function that calls fn pointers for the steps.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06e m06_template_method_nvi.cpp

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct Request {
    std::string op;
    std::string arg;
};

class Handler {
public:
    virtual ~Handler() = default;

    // THE template method. Public, non-virtual: this sequence is law.
    std::string handle(const Request& r) {
        if (!validate(r)) return "ERR invalid " + r.op;        // hook 1 (has default)
        log("start " + r.op);
        std::string reply;
        try {
            reply = run(r);                                     // hook 2 (pure: must be provided)
        } catch (const std::exception& e) {
            reply = std::string("ERR ") + e.what();
        }
        log("end " + r.op + " -> " + reply);
        return reply;
    }

    const std::vector<std::string>& logs() const { return log_; }

private:
    // Hooks are PRIVATE virtual. Derived classes override them but cannot call
    // them directly, and nobody outside can call them out of sequence.
    virtual bool validate(const Request& r) { return !r.op.empty(); }   // default step
    virtual std::string run(const Request& r) = 0;                       // required step
    void log(std::string line) { log_.push_back(std::move(line)); }

    std::vector<std::string> log_;
};

class EchoHandler final : public Handler {
private:
    std::string run(const Request& r) override { return "ECHO " + r.arg; }
};

class DivideHandler final : public Handler {
private:
    bool validate(const Request& r) override { return !r.arg.empty(); }   // stricter precondition
    std::string run(const Request& r) override {
        int d = std::stoi(r.arg);
        if (d == 0) throw std::domain_error("divide by zero");             // converted by the skeleton
        return std::to_string(100 / d);
    }
};

// ---------------------------------------------------------------------------
// The alternative when the varying step is ONE operation: pass a callable to a
// free function. Same skeleton, no inheritance. Prefer this when you do not
// need multiple hooks or per-handler state.
// ---------------------------------------------------------------------------
template <class Run>
std::string handle_with(const Request& r, std::vector<std::string>& log, Run run) {
    if (r.op.empty()) return "ERR invalid";
    log.push_back("start " + r.op);
    std::string reply;
    try {
        reply = run(r);
    } catch (const std::exception& e) {
        reply = std::string("ERR ") + e.what();
    }
    log.push_back("end " + r.op + " -> " + reply);
    return reply;
}

int main() {
    std::vector<std::unique_ptr<Handler>> handlers;
    handlers.push_back(std::make_unique<EchoHandler>());
    handlers.push_back(std::make_unique<DivideHandler>());

    Request reqs[] = {{"echo", "hello"}, {"div", "4"}, {"div", "0"}, {"div", ""}, {"", "x"}};

    std::printf("NVI handlers:\n");
    for (auto& h : handlers) {
        for (const auto& r : reqs) std::printf("  %s(%s) -> %s\n", r.op.c_str(), r.arg.c_str(), h->handle(r).c_str());
        std::printf("  log lines: %zu\n", h->logs().size());
    }

    std::printf("callable version:\n");
    std::vector<std::string> log;
    std::printf("  %s\n", handle_with(reqs[1], log, [](const Request& r) { return "sqrt-ish " + r.arg; }).c_str());
    std::printf("  log lines: %zu\n", log.size());
    return 0;
}
