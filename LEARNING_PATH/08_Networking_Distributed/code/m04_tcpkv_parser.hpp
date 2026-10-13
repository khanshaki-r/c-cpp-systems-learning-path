// m04_tcpkv_parser.hpp - incremental parser for the tcpkv text protocol.
//
// Protocol (see 04_framing_and_protocol_design.md section 2.5):
//   Requests (each line ends in CRLF; key = 1..256 bytes, no spaces or control chars):
//     PING
//     GET <key>
//     SET <key> <len>\r\n<len raw bytes>\r\n        (value is binary-safe: length-prefixed)
//     DEL <key>
//     EXISTS <key>
//     QUIT
//   Responses:
//     +OK\r\n   +PONG\r\n                simple status
//     $<len>\r\n<bytes>\r\n              bulk value
//     $-1\r\n                            nil (key absent)
//     :<n>\r\n                           integer (DEL => 0/1, EXISTS => 0/1)
//     -<CODE> <message>\r\n              error; CODE in {ERR_SYNTAX, ERR_UNKNOWN_CMD, ERR_ARGS,
//                                                         ERR_TOO_LARGE, ERR_INTERNAL}
//
// The parser is a state machine fed with arbitrary byte chunks (1 byte or 64 KB, it does not
// matter) and emits complete commands. It never blocks and never touches a socket, so it can be
// unit-tested with plain asserts and reused by the thread-pool server (05) and an epoll server.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tcpkv {

enum class Op { Ping, Get, Set, Del, Exists, Quit };

struct Command {
    Op op = Op::Ping;
    std::string key;
    std::string value; // only for Set
};

struct ParseError {
    std::string code;    // e.g. "ERR_SYNTAX"
    std::string message; // human readable
    bool fatal = false;  // true => the stream is unrecoverable, caller should close the connection
};

enum class ParseStatus { NeedMore, Ok, Error };

class Parser {
public:
    explicit Parser(size_t max_line = 1024, size_t max_value = 1u << 20)
        : max_line_(max_line), max_value_(max_value) {}

    // Append bytes received from the socket. Cheap: just a buffer append.
    void feed(const char* data, size_t len) { buf_.append(data, len); }
    void feed(std::string_view s) { buf_.append(s.data(), s.size()); }

    // Try to extract the next complete command.
    //   Ok       -> `cmd` filled, bytes consumed; call again, there may be more.
    //   NeedMore -> nothing consumed beyond what is already parsed; feed more bytes.
    //   Error    -> `err` filled. If !err.fatal the offending request was skipped and the
    //               caller should send the error response and call next() again.
    ParseStatus next(Command& cmd, ParseError& err);

    size_t buffered() const { return buf_.size() - pos_; }

private:
    enum class State { Line, Payload, PayloadCrlf };

    ParseStatus parse_line(std::string_view line, Command& cmd, ParseError& err);
    static bool valid_key(std::string_view k);
    static bool parse_uint(std::string_view s, size_t& out);
    void compact();

    State state_ = State::Line;
    std::string buf_;
    size_t pos_ = 0;         // start of unconsumed data in buf_
    size_t payload_len_ = 0; // for State::Payload
    Command pending_;        // SET command waiting for its payload
    size_t max_line_;
    size_t max_value_;
};

// ----- response formatting -------------------------------------------------------------------
inline std::string fmt_ok() { return "+OK\r\n"; }
inline std::string fmt_pong() { return "+PONG\r\n"; }
inline std::string fmt_nil() { return "$-1\r\n"; }
inline std::string fmt_int(long long n) { return ":" + std::to_string(n) + "\r\n"; }
inline std::string fmt_bulk(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 16);
    out += '$';
    out += std::to_string(v.size());
    out += "\r\n";
    out.append(v.data(), v.size());
    out += "\r\n";
    return out;
}
inline std::string fmt_err(std::string_view code, std::string_view msg) {
    std::string out;
    out.reserve(code.size() + msg.size() + 4);
    out += '-';
    out.append(code.data(), code.size());
    out += ' ';
    out.append(msg.data(), msg.size());
    out += "\r\n";
    return out;
}

// ----- request formatting (for clients and tests) -------------------------------------------
inline std::string fmt_cmd(const Command& c) {
    switch (c.op) {
    case Op::Ping: return "PING\r\n";
    case Op::Quit: return "QUIT\r\n";
    case Op::Get: return "GET " + c.key + "\r\n";
    case Op::Del: return "DEL " + c.key + "\r\n";
    case Op::Exists: return "EXISTS " + c.key + "\r\n";
    case Op::Set:
        return "SET " + c.key + " " + std::to_string(c.value.size()) + "\r\n" + c.value + "\r\n";
    }
    return {};
}

// ----- implementation -----------------------------------------------------------------------
inline bool Parser::valid_key(std::string_view k) {
    if (k.empty() || k.size() > 256) return false;
    for (unsigned char ch : k)
        if (ch <= 0x20 || ch == 0x7f) return false; // no spaces, no control chars
    return true;
}

inline bool Parser::parse_uint(std::string_view s, size_t& out) {
    if (s.empty() || s.size() > 10) return false; // 10 digits covers 32-bit; rejects absurd lengths
    size_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<size_t>(ch - '0');
    }
    out = v;
    return true;
}

inline void Parser::compact() {
    // Drop consumed bytes once they dominate the buffer. Amortised O(1) per byte.
    if (pos_ > 4096 && pos_ > buf_.size() / 2) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
}

inline ParseStatus Parser::parse_line(std::string_view line, Command& cmd, ParseError& err) {
    // Split on single spaces. Tolerating multiple spaces would be friendlier; being strict keeps
    // the grammar unambiguous and the parser small.
    size_t sp1 = line.find(' ');
    std::string_view verb = line.substr(0, sp1);
    std::string_view rest = sp1 == std::string_view::npos ? std::string_view{} : line.substr(sp1 + 1);

    auto one_arg = [&](Op op) -> ParseStatus {
        if (rest.empty() || rest.find(' ') != std::string_view::npos) {
            err = {"ERR_ARGS", std::string(verb) + " takes exactly one argument", false};
            return ParseStatus::Error;
        }
        if (!valid_key(rest)) {
            err = {"ERR_SYNTAX", "invalid key", false};
            return ParseStatus::Error;
        }
        cmd.op = op;
        cmd.key.assign(rest.data(), rest.size());
        cmd.value.clear();
        return ParseStatus::Ok;
    };

    if (verb == "PING" || verb == "QUIT") {
        if (!rest.empty()) {
            err = {"ERR_ARGS", std::string(verb) + " takes no arguments", false};
            return ParseStatus::Error;
        }
        cmd.op = verb == "PING" ? Op::Ping : Op::Quit;
        cmd.key.clear();
        cmd.value.clear();
        return ParseStatus::Ok;
    }
    if (verb == "GET") return one_arg(Op::Get);
    if (verb == "DEL") return one_arg(Op::Del);
    if (verb == "EXISTS") return one_arg(Op::Exists);
    if (verb == "SET") {
        size_t sp2 = rest.find(' ');
        if (sp2 == std::string_view::npos) {
            err = {"ERR_ARGS", "SET takes <key> <len>", false};
            return ParseStatus::Error;
        }
        std::string_view key = rest.substr(0, sp2);
        std::string_view len_s = rest.substr(sp2 + 1);
        if (!valid_key(key)) {
            err = {"ERR_SYNTAX", "invalid key", false};
            return ParseStatus::Error;
        }
        size_t len = 0;
        if (!parse_uint(len_s, len)) {
            err = {"ERR_SYNTAX", "invalid length", false};
            return ParseStatus::Error;
        }
        if (len > max_value_) {
            // We cannot skip a payload we refuse to buffer, and we do not know whether the
            // client will send it. The only safe choice is to drop the connection.
            err = {"ERR_TOO_LARGE", "value exceeds limit", true};
            return ParseStatus::Error;
        }
        pending_.op = Op::Set;
        pending_.key.assign(key.data(), key.size());
        pending_.value.clear();
        payload_len_ = len;
        state_ = State::Payload;
        return ParseStatus::NeedMore; // caller loops; payload may already be buffered
    }
    err = {"ERR_UNKNOWN_CMD", "unknown command '" + std::string(verb) + "'", false};
    return ParseStatus::Error;
}

inline ParseStatus Parser::next(Command& cmd, ParseError& err) {
    for (;;) {
        std::string_view avail(buf_.data() + pos_, buf_.size() - pos_);

        switch (state_) {
        case State::Line: {
            size_t nl = avail.find("\r\n");
            if (nl == std::string_view::npos) {
                if (avail.size() > max_line_) {
                    // No line terminator within the limit: either a hostile client or a
                    // client speaking a different protocol (e.g. HTTP). Unrecoverable.
                    err = {"ERR_TOO_LARGE", "line too long", true};
                    return ParseStatus::Error;
                }
                return ParseStatus::NeedMore;
            }
            std::string_view line = avail.substr(0, nl);
            pos_ += nl + 2; // consume the line including CRLF
            // parse_line copies what it needs out of `line` (which points into buf_), so the
            // compact() that may erase from buf_ must come AFTER it, never before.
            ParseStatus st = parse_line(line, cmd, err);
            compact();
            if (st == ParseStatus::NeedMore) continue; // SET: go read the payload
            return st;
        }
        case State::Payload: {
            if (avail.size() < payload_len_) return ParseStatus::NeedMore;
            pending_.value.assign(avail.data(), payload_len_);
            pos_ += payload_len_;
            state_ = State::PayloadCrlf;
            continue;
        }
        case State::PayloadCrlf: {
            if (avail.size() < 2) return ParseStatus::NeedMore;
            bool ok = avail[0] == '\r' && avail[1] == '\n';
            pos_ += 2;
            compact();
            state_ = State::Line;
            if (!ok) {
                // The declared length and the actual payload disagree. We are no longer sure
                // where the next command starts, so the stream is poisoned.
                err = {"ERR_SYNTAX", "payload not terminated by CRLF", true};
                return ParseStatus::Error;
            }
            cmd = std::move(pending_);
            pending_ = Command{};
            return ParseStatus::Ok;
        }
        }
    }
}

} // namespace tcpkv
