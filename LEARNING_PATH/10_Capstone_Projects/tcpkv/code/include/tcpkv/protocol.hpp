#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tcpkv {

// Wire format (see 00_spec.md):
//
//   request  := line '\n' [ value '\n' ]        a line is at most kMaxLineLength bytes
//   line     := COMMAND { ' ' arg }             tokens separated by spaces or tabs, optional '\r'
//   SET      := "SET" key length '\n' value '\n' value is exactly `length` raw bytes
//
//   reply    := '+' text '\n'                   status
//             | '-' "ERR " text '\n'            error
//             | ':' integer '\n'                integer
//             | '$' length '\n' bytes '\n'      bulk (binary safe)
//             | "$-1" '\n'                      null (key not found)
//             | '*' count '\n' { bulk }         array of bulks

namespace limits {
inline constexpr std::size_t kMaxKeyLength = 256;
inline constexpr std::size_t kMaxValueLength = 1024 * 1024;  // 1 MiB
inline constexpr std::size_t kMaxLineLength = 4096;
inline constexpr std::size_t kMaxArgs = 64;
// 100 years. Converted to steady_clock's nanoseconds this is ~3.2e18, below INT64_MAX (~9.2e18),
// and adding it to steady_clock::now() (time since boot) cannot overflow either. Without this
// bound, EXPIRE k 10000000000 is signed overflow (undefined behaviour) inside std::chrono.
inline constexpr long long kMaxExpireSeconds = 100LL * 365 * 24 * 3600;
}  // namespace limits

enum class CommandType { Ping, Get, Set, Del, Exists, Keys, Expire, Ttl, Stats, Quit };

const char* to_string(CommandType type) noexcept;

struct Command {
    CommandType type = CommandType::Ping;
    std::vector<std::string> args;  // everything after the command name, SET's length excluded
    std::string value;              // SET only
};

struct ParseError {
    std::string message;
    // fatal: the stream can no longer be trusted (e.g. oversized value). The server replies
    // with the error and closes. Non-fatal errors (unknown command, bad arity) only skip the line.
    bool fatal = false;
};

using ParseEvent = std::variant<Command, ParseError>;

// Keys: 1..kMaxKeyLength bytes of printable ASCII (0x21..0x7E), so no spaces or control chars.
bool is_valid_key(std::string_view key) noexcept;
// Same alphabet, but may be empty (used for KEYS prefix).
bool is_valid_key_prefix(std::string_view prefix) noexcept;

// Incremental request parser. Feed it whatever read() returned, then call next() until it
// returns nullopt. It never blocks and never looks at the socket; that keeps it unit-testable.
//
// State machine:
//   Line  --"SET k n" header-->  Value  --n bytes + '\n'-->  Line
//   Line  --any other line---->  Line   (emits a Command or a non-fatal ParseError)
//   any   --fatal error-------->  Dead   (next() returns nullopt forever)
class Parser {
public:
    Parser() = default;

    void feed(const char* data, std::size_t len);
    void feed(std::string_view data) { feed(data.data(), data.size()); }

    // Returns the next complete event, or nullopt when more bytes are needed.
    std::optional<ParseEvent> next();

    std::size_t buffered() const noexcept { return buffer_.size() - pos_; }
    bool dead() const noexcept { return state_ == State::Dead; }

private:
    enum class State { Line, Value, Dead };

    std::optional<ParseEvent> parse_line(std::string_view line);
    std::optional<ParseEvent> fail(std::string message, bool fatal);
    void compact();

    State state_ = State::Line;
    std::string buffer_;
    std::size_t pos_ = 0;  // start of unconsumed bytes in buffer_
    Command pending_;      // SET header waiting for its value
    std::size_t value_len_ = 0;
};

// Request encoders (used by the client library and by tests).
std::string encode_command(const std::vector<std::string>& tokens);  // "GET key\n"
std::string encode_set(std::string_view key, std::string_view value);  // "SET key 5\nhello\n"

// Reply encoders (used by the server and by tests).
namespace reply {
std::string status(std::string_view text);
std::string error(std::string_view text);
std::string integer(long long value);
std::string bulk(std::string_view bytes);
std::string null();
std::string array(const std::vector<std::string>& items);
}  // namespace reply

}  // namespace tcpkv
