#include "tcpkv/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace tcpkv {

namespace {

constexpr std::size_t kCompactThreshold = 64 * 1024;

std::vector<std::string_view> split_tokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

std::string to_upper_copy(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<CommandType> lookup_command(std::string_view upper) {
    if (upper == "PING") return CommandType::Ping;
    if (upper == "GET") return CommandType::Get;
    if (upper == "SET") return CommandType::Set;
    if (upper == "DEL") return CommandType::Del;
    if (upper == "EXISTS") return CommandType::Exists;
    if (upper == "KEYS") return CommandType::Keys;
    if (upper == "EXPIRE") return CommandType::Expire;
    if (upper == "TTL") return CommandType::Ttl;
    if (upper == "STATS") return CommandType::Stats;
    if (upper == "QUIT") return CommandType::Quit;
    return std::nullopt;
}

bool parse_unsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    // from_chars rejects leading '+', whitespace and (for unsigned types) '-': exactly the
    // strictness a wire protocol wants.
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool parse_signed(std::string_view text, long long& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool is_printable_ascii(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x21 && u <= 0x7E;
    });
}

}  // namespace

const char* to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::Ping: return "PING";
        case CommandType::Get: return "GET";
        case CommandType::Set: return "SET";
        case CommandType::Del: return "DEL";
        case CommandType::Exists: return "EXISTS";
        case CommandType::Keys: return "KEYS";
        case CommandType::Expire: return "EXPIRE";
        case CommandType::Ttl: return "TTL";
        case CommandType::Stats: return "STATS";
        case CommandType::Quit: return "QUIT";
    }
    return "?";
}

bool is_valid_key(std::string_view key) noexcept {
    return !key.empty() && key.size() <= limits::kMaxKeyLength && is_printable_ascii(key);
}

bool is_valid_key_prefix(std::string_view prefix) noexcept {
    return prefix.size() <= limits::kMaxKeyLength && is_printable_ascii(prefix);
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

void Parser::feed(const char* data, std::size_t len) {
    if (state_ == State::Dead || len == 0) {
        return;  // a dead parser drops input: the connection is about to be closed anyway
    }
    buffer_.append(data, len);
}

std::optional<ParseEvent> Parser::next() {
    for (;;) {
        switch (state_) {
            case State::Dead:
                return std::nullopt;

            case State::Line: {
                const std::size_t newline = buffer_.find('\n', pos_);
                if (newline == std::string::npos) {
                    if (buffered() > limits::kMaxLineLength) {
                        return fail("line too long", /*fatal=*/true);
                    }
                    compact();
                    return std::nullopt;
                }
                std::string_view line(buffer_.data() + pos_, newline - pos_);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                pos_ = newline + 1;
                if (line.size() > limits::kMaxLineLength) {
                    return fail("line too long", /*fatal=*/true);
                }
                if (auto event = parse_line(line)) {
                    return event;
                }
                break;  // blank line or SET header: loop and look at the next thing
            }

            case State::Value: {
                const std::size_t available = buffer_.size() - pos_;
                if (available < value_len_ + 1) {
                    compact();
                    return std::nullopt;
                }
                const char* data = buffer_.data() + pos_;
                std::size_t consumed = 0;
                if (data[value_len_] == '\n') {
                    consumed = value_len_ + 1;
                } else if (data[value_len_] == '\r') {
                    if (available < value_len_ + 2) {
                        compact();
                        return std::nullopt;
                    }
                    if (data[value_len_ + 1] != '\n') {
                        return fail("value not terminated by newline", /*fatal=*/true);
                    }
                    consumed = value_len_ + 2;
                } else {
                    return fail("value not terminated by newline", /*fatal=*/true);
                }
                pending_.value.assign(data, value_len_);
                pos_ += consumed;
                state_ = State::Line;
                Command command = std::move(pending_);
                pending_ = Command{};
                return ParseEvent(std::move(command));
            }
        }
    }
}

std::optional<ParseEvent> Parser::parse_line(std::string_view line) {
    const std::vector<std::string_view> tokens = split_tokens(line);
    if (tokens.empty()) {
        return std::nullopt;  // blank lines are ignored; makes `nc` sessions forgiving
    }
    if (tokens.size() > limits::kMaxArgs + 1) {
        return fail("too many arguments", /*fatal=*/false);
    }

    const std::string name = to_upper_copy(tokens[0]);
    const std::optional<CommandType> type = lookup_command(name);
    if (!type) {
        return fail("unknown command '" + name + "'", /*fatal=*/false);
    }

    Command cmd;
    cmd.type = *type;
    cmd.args.reserve(tokens.size() - 1);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        cmd.args.emplace_back(tokens[i]);
    }
    const std::size_t argc = cmd.args.size();

    switch (*type) {
        case CommandType::Ping:
        case CommandType::Stats:
        case CommandType::Quit:
            if (argc != 0) {
                return fail(name + " takes no arguments", false);
            }
            break;

        case CommandType::Get:
        case CommandType::Ttl:
            if (argc != 1) {
                return fail("wrong number of arguments for " + name, false);
            }
            if (!is_valid_key(cmd.args[0])) {
                return fail("invalid key", false);
            }
            break;

        case CommandType::Del:
        case CommandType::Exists:
            if (argc < 1) {
                return fail("wrong number of arguments for " + name, false);
            }
            for (const std::string& key : cmd.args) {
                if (!is_valid_key(key)) {
                    return fail("invalid key", false);
                }
            }
            break;

        case CommandType::Keys:
            if (argc > 1) {
                return fail("wrong number of arguments for KEYS", false);
            }
            if (argc == 1 && !is_valid_key_prefix(cmd.args[0])) {
                return fail("invalid prefix", false);
            }
            break;

        case CommandType::Expire: {
            if (argc != 2) {
                return fail("wrong number of arguments for EXPIRE (expected: EXPIRE key seconds)",
                            false);
            }
            if (!is_valid_key(cmd.args[0])) {
                return fail("invalid key", false);
            }
            long long seconds = 0;
            if (!parse_signed(cmd.args[1], seconds) || seconds > limits::kMaxExpireSeconds ||
                seconds < -limits::kMaxExpireSeconds) {
                return fail("invalid expire seconds", false);
            }
            break;
        }

        case CommandType::Set: {
            if (argc != 2) {
                return fail("wrong number of arguments for SET (expected: SET key length)", false);
            }
            if (!is_valid_key(cmd.args[0])) {
                return fail("invalid key", false);
            }
            std::uint64_t length = 0;
            if (!parse_unsigned(cmd.args[1], length)) {
                return fail("invalid value length", false);
            }
            if (length > limits::kMaxValueLength) {
                // Fatal: we refuse to read megabytes just to resynchronise on a bad client.
                return fail("value too large", /*fatal=*/true);
            }
            value_len_ = static_cast<std::size_t>(length);
            cmd.args.pop_back();  // the length is bookkeeping, not an argument the executor needs
            pending_ = std::move(cmd);
            state_ = State::Value;
            return std::nullopt;
        }
    }
    return ParseEvent(std::move(cmd));
}

std::optional<ParseEvent> Parser::fail(std::string message, bool fatal) {
    if (fatal) {
        state_ = State::Dead;
        buffer_.clear();
        pos_ = 0;
        pending_ = Command{};
    }
    return ParseEvent(ParseError{std::move(message), fatal});
}

void Parser::compact() {
    if (pos_ == 0) {
        return;
    }
    if (pos_ == buffer_.size()) {
        buffer_.clear();
        pos_ = 0;
    } else if (pos_ >= kCompactThreshold) {
        // Erasing from the front is O(n); doing it rarely keeps the amortised cost negligible.
        buffer_.erase(0, pos_);
        pos_ = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------------------------

std::string encode_command(const std::vector<std::string>& tokens) {
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) {
            out += ' ';
        }
        out += tokens[i];
    }
    out += '\n';
    return out;
}

std::string encode_set(std::string_view key, std::string_view value) {
    std::string out;
    out.reserve(key.size() + value.size() + 32);
    out += "SET ";
    out += key;
    out += ' ';
    out += std::to_string(value.size());
    out += '\n';
    out.append(value);
    out += '\n';
    return out;
}

namespace reply {

std::string status(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '+';
    out += text;
    out += '\n';
    return out;
}

std::string error(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 6);
    out += "-ERR ";
    out += text;
    out += '\n';
    return out;
}

std::string integer(long long value) {
    std::string out = ":";
    out += std::to_string(value);
    out += '\n';
    return out;
}

std::string bulk(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() + 16);
    out += '$';
    out += std::to_string(bytes.size());
    out += '\n';
    out.append(bytes);
    out += '\n';
    return out;
}

std::string null() { return "$-1\n"; }

std::string array(const std::vector<std::string>& items) {
    std::string out;
    out += '*';
    out += std::to_string(items.size());
    out += '\n';
    for (const std::string& item : items) {
        out += bulk(item);
    }
    return out;
}

}  // namespace reply

}  // namespace tcpkv
