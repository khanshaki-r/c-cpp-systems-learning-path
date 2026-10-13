// m08_variant_state_machine.cpp
// A protocol state machine where STATES and EVENTS are both std::variant.
// Transitions are a single overload set: one function per (state, event) pair
// that matters, plus a generic fallback. Each state struct carries exactly the
// data that state needs. No heap, no vtables, no base class.
//
// Compared with m06_state_machine_connection.cpp (state classes) this has:
//   + all transitions in ONE place (the overload set)
//   + per-state data with automatic destruction on transition
//   + no allocation per transition
//   + the compiler forces a decision for every (state, event) pair you list
//   - the generic fallback can hide a forgotten pair: keep it loud (log) or omit it
//
// Scenario: a length-prefixed message framer:
//   WaitHeader(4 bytes) -> ReadBody(len bytes) -> Complete -> WaitHeader ...
//   with an Error state on bad length and an Idle timeout.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m08c m08_variant_state_machine.cpp

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ------------------------------- states ------------------------------------
struct WaitHeader { std::string header_bytes; };                 // accumulating up to 4 bytes
struct ReadBody   { std::uint32_t expected; std::string body; }; // accumulating body
struct Complete   { std::string message; };                      // one full message ready
struct Failed     { std::string reason; };

using State = std::variant<WaitHeader, ReadBody, Complete, Failed>;

// ------------------------------- events ------------------------------------
struct Bytes   { std::string_view data; };   // some bytes arrived (may be partial or several messages)
struct Timeout {};
struct Reset   {};

using Event = std::variant<Bytes, Timeout, Reset>;

// ------------------------------- transitions -------------------------------
// Returns the new state and how many bytes of the input it consumed (so the
// driver can feed the remainder to the next state).
struct Step {
    State next;
    std::size_t consumed;
};

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

constexpr std::uint32_t kMaxBody = 1 << 20;

static std::uint32_t parse_len(const std::string& h) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(h[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(h[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(h[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(h[3]));
}

Step step(State s, const Event& e) {
    return std::visit(overloaded{
        // --- WaitHeader ---
        [](WaitHeader st, const Bytes& b) -> Step {
            std::size_t need = 4 - st.header_bytes.size();
            std::size_t take = b.data.size() < need ? b.data.size() : need;
            st.header_bytes.append(b.data.substr(0, take));
            if (st.header_bytes.size() < 4) return {std::move(st), take};
            std::uint32_t len = parse_len(st.header_bytes);
            if (len == 0 || len > kMaxBody) return {Failed{"bad length " + std::to_string(len)}, take};
            return {ReadBody{len, {}}, take};
        },
        // --- ReadBody ---
        [](ReadBody st, const Bytes& b) -> Step {
            std::size_t need = st.expected - st.body.size();
            std::size_t take = b.data.size() < need ? b.data.size() : need;
            st.body.append(b.data.substr(0, take));
            if (st.body.size() < st.expected) return {std::move(st), take};
            return {Complete{std::move(st.body)}, take};
        },
        // --- Complete: caller must consume the message (Reset) before feeding more ---
        [](Complete st, const Bytes&) -> Step { return {Failed{"bytes while message pending (" + st.message + ")"}, 0}; },
        [](Complete, const Reset&) -> Step { return {WaitHeader{}, 0}; },
        // --- timeouts: only matter mid-frame ---
        [](WaitHeader st, const Timeout&) -> Step {
            if (st.header_bytes.empty()) return {std::move(st), 0};   // idle: fine
            return {Failed{"timeout mid-header"}, 0};
        },
        [](ReadBody, const Timeout&) -> Step { return {Failed{"timeout mid-body"}, 0}; },
        // --- Reset from anywhere else ---
        [](auto, const Reset&) -> Step { return {WaitHeader{}, 0}; },
        // --- generic fallback: loud, so a forgotten pair is visible in logs ---
        [](auto st, const auto&) -> Step {
            std::printf("    (ignored event in state index %zu)\n", State(st).index());
            return {State(std::move(st)), 0};
        },
    }, std::move(s), e);
}

// ------------------------------- driver ------------------------------------
class Framer {
public:
    // Feed a chunk; returns every complete message found in it.
    std::vector<std::string> feed(std::string_view chunk) {
        std::vector<std::string> out;
        while (!chunk.empty()) {
            Step r = step(std::move(state_), Event{Bytes{chunk}});
            state_ = std::move(r.next);
            chunk.remove_prefix(r.consumed);
            if (auto* c = std::get_if<Complete>(&state_)) {
                out.push_back(std::move(c->message));
                state_ = step(std::move(state_), Event{Reset{}}).next;
            } else if (std::holds_alternative<Failed>(state_)) {
                break;   // stop consuming; caller inspects error()
            }
        }
        return out;
    }
    void timeout() { state_ = step(std::move(state_), Event{Timeout{}}).next; }
    void reset() { state_ = step(std::move(state_), Event{Reset{}}).next; }
    const char* error() const {
        auto* f = std::get_if<Failed>(&state_);
        return f ? f->reason.c_str() : nullptr;
    }
    std::size_t state_index() const { return state_.index(); }

private:
    State state_ = WaitHeader{};
};

static std::string frame(std::string_view body) {
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    std::string f;
    f.push_back(static_cast<char>(n >> 24));
    f.push_back(static_cast<char>(n >> 16));
    f.push_back(static_cast<char>(n >> 8));
    f.push_back(static_cast<char>(n));
    f.append(body);
    return f;
}

int main() {
    Framer fr;
    std::string stream = frame("GET acct:1001") + frame("PUT acct:1001 500") + frame("PING");

    // Feed in awkward chunk sizes to prove partial handling works.
    std::size_t pos = 0;
    std::size_t sizes[] = {3, 5, 11, 1, 7, 100};
    for (std::size_t sz : sizes) {
        if (pos >= stream.size()) break;
        std::string_view chunk(stream.data() + pos, std::min(sz, stream.size() - pos));
        pos += chunk.size();
        auto msgs = fr.feed(chunk);
        std::printf("fed %zu bytes -> %zu message(s)", chunk.size(), msgs.size());
        for (auto& m : msgs) std::printf(" [%s]", m.c_str());
        std::printf("\n");
    }

    // Timeout while idle is harmless; mid-frame it fails.
    fr.timeout();
    std::printf("idle timeout -> error: %s\n", fr.error() ? fr.error() : "none");
    fr.feed(std::string_view("\x00\x00", 2));   // explicit length: the literal contains NULs
    fr.timeout();
    std::printf("mid-header timeout -> error: %s\n", fr.error() ? fr.error() : "none");
    fr.reset();

    // Bad length.
    std::string bad = "\xFF\xFF\xFF\xFF";
    fr.feed(bad);
    std::printf("bad length -> error: %s\n", fr.error() ? fr.error() : "none");

    std::printf("sizeof(State) = %zu\n", sizeof(State));
    return 0;
}
