// src/protocol.cpp - parser and response formatting for the tcpkv text protocol.
#include "tcpkv/protocol.hpp"

#include <algorithm>
#include <cctype>

namespace tcpkv {
namespace {

// Strip a single trailing '\r' (clients on Windows, telnet, nc -C send CRLF).
std::string_view strip_cr(std::string_view s) noexcept {
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
}

// Return the next space-delimited token and advance 'rest' past it and past any following spaces.
std::string_view next_token(std::string_view& rest) noexcept {
    const std::size_t end = rest.find(' ');
    std::string_view tok = rest.substr(0, end);
    if (end == std::string_view::npos) {
        rest = std::string_view{};
    } else {
        rest.remove_prefix(end);
        // Collapse runs of spaces between tokens; values keep their internal spaces untouched.
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    }
    return tok;
}

bool valid_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaxKeyLength) return false;
    // Keys are printable ASCII without spaces so the wire format stays unambiguous.
    return std::all_of(key.begin(), key.end(), [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc > 0x20 && uc < 0x7f;
    });
}

ParseResult fail(std::string message) {
    ParseResult r;
    r.error = std::move(message);
    return r;
}

ParseResult succeed(Command cmd) {
    ParseResult r;
    r.command = std::move(cmd);
    return r;
}

}  // namespace

ParseResult parse_command(std::string_view line) {
    line = strip_cr(line);
    if (line.empty()) return fail("empty command");
    if (line.size() > kMaxLineLength) return fail("line too long");

    std::string_view rest = line;
    const std::string_view verb = next_token(rest);

    if (verb == "PING") {
        if (!rest.empty()) return fail("PING takes no arguments");
        return succeed(Command{CommandType::Ping, {}, {}});
    }
    if (verb == "QUIT") {
        if (!rest.empty()) return fail("QUIT takes no arguments");
        return succeed(Command{CommandType::Quit, {}, {}});
    }
    if (verb == "GET" || verb == "DEL") {
        const std::string_view key = next_token(rest);
        if (!valid_key(key)) return fail("invalid key");
        if (!rest.empty()) return fail(std::string(verb) + " takes exactly one argument");
        const CommandType t = (verb == "GET") ? CommandType::Get : CommandType::Del;
        return succeed(Command{t, std::string(key), {}});
    }
    if (verb == "SET") {
        const std::string_view key = next_token(rest);
        if (!valid_key(key)) return fail("invalid key");
        // Everything after the key, spaces included, is the value. Empty value is allowed.
        if (rest.size() > kMaxValueLength) return fail("value too long");
        return succeed(Command{CommandType::Set, std::string(key), std::string(rest)});
    }
    return fail("unknown command");
}

std::string format_pong()      { return "PONG\n"; }
std::string format_ok()        { return "OK\n"; }
std::string format_not_found() { return "NOT_FOUND\n"; }

std::string format_value(std::string_view value) {
    std::string out;
    out.reserve(6 + value.size() + 1);
    out.append("VALUE ").append(value).push_back('\n');
    return out;
}

std::string format_error(std::string_view message) {
    std::string out;
    out.reserve(6 + message.size() + 1);
    out.append("ERROR ").append(message).push_back('\n');
    return out;
}

const char* to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::Ping: return "PING";
        case CommandType::Get:  return "GET";
        case CommandType::Set:  return "SET";
        case CommandType::Del:  return "DEL";
        case CommandType::Quit: return "QUIT";
    }
    return "UNKNOWN";  // unreachable for valid enums; keeps -Wreturn-type quiet for bad casts
}

}  // namespace tcpkv
