// m06_state_machine_connection.cpp
// STATE pattern, classic form: each state is a class; the context delegates
// to the current state object; transitions return the next state.
//
// Scenario: a connection's protocol life cycle:
//   Connecting -> Authenticating -> Ready -> Closing -> Closed
// with error paths to Closing from any state.
//
// C equivalent: an enum + a switch in every event handler, or a 2-D table
// state x event -> handler. The enum+switch is often BETTER for small, closed
// machines (the compiler checks exhaustiveness). The class form wins when each
// state has significant state-specific data or behaviour. Module 08 shows the
// std::variant form, which combines both advantages.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06d m06_state_machine_connection.cpp

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class Connection;   // the context

// ---------------------------------------------------------------------------
// The State interface. Each handler returns the next state (or nullptr for
// "stay"). Returning a new state object is how transitions happen; the
// context swaps its unique_ptr.
// ---------------------------------------------------------------------------
class State {
public:
    virtual ~State() = default;
    virtual std::string_view name() const = 0;
    virtual std::unique_ptr<State> on_bytes(Connection& c, std::string_view bytes) = 0;
    virtual std::unique_ptr<State> on_timeout(Connection& c) = 0;
    virtual std::unique_ptr<State> on_close(Connection& c) = 0;
    virtual void on_enter(Connection&) {}   // optional hooks
    virtual void on_exit(Connection&) {}
};

// ---------------------------------------------------------------------------
// The context. Owns the current state. Exposes what states need (send, log,
// session data). Delegates every event.
// ---------------------------------------------------------------------------
class Connection {
public:
    Connection();   // defined after the states (needs a complete Connecting)

    void feed(std::string_view bytes) { transition(state_->on_bytes(*this, bytes)); }
    void timeout() { transition(state_->on_timeout(*this)); }
    void close() { transition(state_->on_close(*this)); }

    void send(std::string_view s) { std::printf("    -> send: %.*s\n", static_cast<int>(s.size()), s.data()); }
    void set_user(std::string u) { user_ = std::move(u); }
    const std::string& user() const { return user_; }
    std::string_view state_name() const { return state_->name(); }
    int attempts = 0;   // used by Authenticating; lives in context so it survives re-entry

private:
    void transition(std::unique_ptr<State> next) {
        if (!next) return;   // nullptr means "stay in current state"
        std::printf("  [%s -> %s]\n", std::string(state_->name()).c_str(), std::string(next->name()).c_str());
        state_->on_exit(*this);
        state_ = std::move(next);
        state_->on_enter(*this);
    }
    std::unique_ptr<State> state_;
    std::string user_;
};

// ---------------------------------------------------------------------------
// Concrete states. Note that each one only knows about the states it can
// transition to; the transition table is distributed across the classes.
// That is the main readability cost of this pattern compared with a table.
// ---------------------------------------------------------------------------
class Closed final : public State {
public:
    std::string_view name() const override { return "Closed"; }
    std::unique_ptr<State> on_bytes(Connection&, std::string_view) override { return nullptr; }   // ignore
    std::unique_ptr<State> on_timeout(Connection&) override { return nullptr; }
    std::unique_ptr<State> on_close(Connection&) override { return nullptr; }
};

class Closing final : public State {
public:
    std::string_view name() const override { return "Closing"; }
    void on_enter(Connection& c) override { c.send("BYE"); }
    std::unique_ptr<State> on_bytes(Connection&, std::string_view) override { return nullptr; }   // drain
    std::unique_ptr<State> on_timeout(Connection&) override { return std::make_unique<Closed>(); }
    std::unique_ptr<State> on_close(Connection&) override { return std::make_unique<Closed>(); }
};

class Ready final : public State {
public:
    std::string_view name() const override { return "Ready"; }
    std::unique_ptr<State> on_bytes(Connection& c, std::string_view bytes) override {
        if (bytes == "QUIT") return std::make_unique<Closing>();
        c.send("OK " + std::string(bytes) + " (as " + c.user() + ")");
        return nullptr;
    }
    std::unique_ptr<State> on_timeout(Connection& c) override {
        c.send("PING");   // keepalive; stay
        return nullptr;
    }
    std::unique_ptr<State> on_close(Connection&) override { return std::make_unique<Closing>(); }
};

class Authenticating final : public State {
public:
    std::string_view name() const override { return "Authenticating"; }
    void on_enter(Connection& c) override { c.send("AUTH?"); }
    std::unique_ptr<State> on_bytes(Connection& c, std::string_view bytes) override {
        if (bytes.substr(0, 5) == "USER ") {
            c.set_user(std::string(bytes.substr(5)));
            return std::make_unique<Ready>();
        }
        if (++c.attempts >= 3) {
            c.send("TOO MANY ATTEMPTS");
            return std::make_unique<Closing>();
        }
        c.send("AUTH FAILED");
        return nullptr;
    }
    std::unique_ptr<State> on_timeout(Connection& c) override {
        c.send("AUTH TIMEOUT");
        return std::make_unique<Closing>();
    }
    std::unique_ptr<State> on_close(Connection&) override { return std::make_unique<Closing>(); }
};

class Connecting final : public State {
public:
    std::string_view name() const override { return "Connecting"; }
    std::unique_ptr<State> on_bytes(Connection& c, std::string_view bytes) override {
        if (bytes == "HELLO") return std::make_unique<Authenticating>();
        c.send("EXPECTED HELLO");
        return std::make_unique<Closing>();
    }
    std::unique_ptr<State> on_timeout(Connection&) override { return std::make_unique<Closing>(); }
    std::unique_ptr<State> on_close(Connection&) override { return std::make_unique<Closed>(); }
};

Connection::Connection() : state_(std::make_unique<Connecting>()) {}

int main() {
    std::printf("session 1: happy path\n");
    Connection c1;
    c1.feed("HELLO");
    c1.feed("bad password");
    c1.feed("USER shakir");
    c1.feed("GET acct:1001");
    c1.timeout();
    c1.feed("QUIT");
    c1.timeout();
    std::printf("  final state: %s\n\n", std::string(c1.state_name()).c_str());

    std::printf("session 2: too many auth attempts\n");
    Connection c2;
    c2.feed("HELLO");
    c2.feed("x");
    c2.feed("y");
    c2.feed("z");
    std::printf("  final state: %s\n", std::string(c2.state_name()).c_str());
    return 0;
}
