// tcpkv/protocol.hpp - line-oriented text protocol.
//
// Request (one line, '\n' terminated, optional '\r' before it):
//   PING                 -> PONG
//   GET <key>            -> VALUE <value>   | NOT_FOUND
//   SET <key> <value...> -> OK              (value is the rest of the line, spaces allowed)
//   DEL <key>            -> OK              | NOT_FOUND
//   QUIT                 -> server closes the connection
// Any malformed line     -> ERROR <message>
//
// Pure functions: no I/O, no global state. That is what makes them unit-testable (module 05).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tcpkv {

enum class CommandType { Ping, Get, Set, Del, Quit };

struct Command {
    CommandType type{CommandType::Ping};
    std::string key;
    std::string value;
};

// A tiny result type: C++17 has no std::expected.
struct ParseResult {
    std::optional<Command> command;  // engaged on success
    std::string error;               // non-empty on failure

    bool ok() const noexcept { return command.has_value(); }
};

// Limits enforced by the parser and the server's line reader.
constexpr std::size_t kMaxKeyLength   = 256;
constexpr std::size_t kMaxValueLength = 64 * 1024;
constexpr std::size_t kMaxLineLength  = kMaxKeyLength + kMaxValueLength + 16;

// Parse one request line WITHOUT the trailing '\n'. A trailing '\r' is tolerated and stripped.
ParseResult parse_command(std::string_view line);

// Response builders. Each returns a complete line including the trailing '\n'.
std::string format_pong();
std::string format_ok();
std::string format_value(std::string_view value);
std::string format_not_found();
std::string format_error(std::string_view message);

// For logging and tests.
const char* to_string(CommandType type) noexcept;

}  // namespace tcpkv
