#include "tcpkv/client.hpp"

#include <charconv>
#include <system_error>
#include <utility>

#include "tcpkv/protocol.hpp"

namespace tcpkv {

namespace {

long long parse_integer_or_throw(std::string_view text) {
    long long value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        throw ClientError("malformed integer in reply: '" + std::string(text) + "'");
    }
    return value;
}

}  // namespace

Client::Client(const std::string& host, std::uint16_t port)
    : socket_(Socket::connect_tcp(host, port)) {
    // Request/response protocol: every command is a small write that must leave immediately.
    socket_.set_nodelay(true);
}

std::string Client::ping() { return expect(execute({"PING"}), Reply::Kind::Status).text; }

std::optional<std::string> Client::get(const std::string& key) {
    Reply reply = execute({"GET", key});
    if (reply.kind == Reply::Kind::Null) {
        return std::nullopt;
    }
    return expect(std::move(reply), Reply::Kind::Bulk).text;
}

void Client::set(const std::string& key, std::string_view value) {
    expect(send_raw(encode_set(key, value)), Reply::Kind::Status);
}

long long Client::del(const std::vector<std::string>& keys) {
    std::vector<std::string> tokens{"DEL"};
    tokens.insert(tokens.end(), keys.begin(), keys.end());
    return expect(execute(tokens), Reply::Kind::Integer).integer;
}

long long Client::exists(const std::vector<std::string>& keys) {
    std::vector<std::string> tokens{"EXISTS"};
    tokens.insert(tokens.end(), keys.begin(), keys.end());
    return expect(execute(tokens), Reply::Kind::Integer).integer;
}

std::vector<std::string> Client::keys(std::string_view prefix) {
    std::vector<std::string> tokens{"KEYS"};
    if (!prefix.empty()) {
        tokens.emplace_back(prefix);
    }
    return expect(execute(tokens), Reply::Kind::Array).array;
}

bool Client::expire(const std::string& key, long long seconds) {
    return expect(execute({"EXPIRE", key, std::to_string(seconds)}), Reply::Kind::Integer).integer == 1;
}

long long Client::ttl(const std::string& key) {
    return expect(execute({"TTL", key}), Reply::Kind::Integer).integer;
}

std::string Client::stats() { return expect(execute({"STATS"}), Reply::Kind::Bulk).text; }

void Client::quit() {
    expect(execute({"QUIT"}), Reply::Kind::Status);
    socket_.shutdown_both();
}

Reply Client::execute(const std::vector<std::string>& tokens) {
    return send_raw(encode_command(tokens));
}

Reply Client::send_raw(std::string_view wire) {
    socket_.write_all(wire);
    return read_reply();
}

Reply Client::read_reply() {
    const std::string line = read_line();
    if (line.empty()) {
        throw ClientError("empty reply line");
    }
    const std::string_view payload(line.data() + 1, line.size() - 1);

    Reply reply;
    switch (line[0]) {
        case '+':
            reply.kind = Reply::Kind::Status;
            reply.text = payload;
            break;
        case '-':
            reply.kind = Reply::Kind::Error;
            // Strip the "ERR " prefix so callers see just the message.
            reply.text = payload.substr(0, 4) == "ERR " ? payload.substr(4) : payload;
            break;
        case ':':
            reply.kind = Reply::Kind::Integer;
            reply.integer = parse_integer_or_throw(payload);
            break;
        case '$': {
            const long long length = parse_integer_or_throw(payload);
            if (length < 0) {
                reply.kind = Reply::Kind::Null;
            } else {
                reply.kind = Reply::Kind::Bulk;
                reply.text = read_bulk(static_cast<std::size_t>(length));
            }
            break;
        }
        case '*': {
            const long long count = parse_integer_or_throw(payload);
            if (count < 0) {
                throw ClientError("negative array length in reply");
            }
            reply.kind = Reply::Kind::Array;
            reply.array.reserve(static_cast<std::size_t>(count));
            for (long long i = 0; i < count; ++i) {
                const std::string header = read_line();
                if (header.empty() || header[0] != '$') {
                    throw ClientError("array element is not a bulk string");
                }
                const long long length =
                    parse_integer_or_throw(std::string_view(header).substr(1));
                if (length < 0) {
                    throw ClientError("null element inside array reply");
                }
                reply.array.push_back(read_bulk(static_cast<std::size_t>(length)));
            }
            break;
        }
        default:
            throw ClientError("malformed reply: '" + line + "'");
    }
    return reply;
}

std::string Client::read_line() {
    for (;;) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        if (buffer_.size() > limits::kMaxLineLength) {
            throw ClientError("reply line too long");
        }
        fill(buffer_.size() + 1);
    }
}

std::string Client::read_bulk(std::size_t length) {
    if (length > limits::kMaxValueLength) {
        throw ClientError("bulk reply larger than protocol limit");
    }
    fill(length + 1);
    if (buffer_[length] != '\n') {
        throw ClientError("bulk reply not terminated by newline");
    }
    std::string value = buffer_.substr(0, length);
    buffer_.erase(0, length + 1);
    return value;
}

void Client::fill(std::size_t needed) {
    char chunk[16 * 1024];
    while (buffer_.size() < needed) {
        const std::size_t n = socket_.read_some(chunk, sizeof chunk);
        if (n == 0) {
            throw ClientError("connection closed by server");
        }
        buffer_.append(chunk, n);
    }
}

Reply Client::expect(Reply reply, Reply::Kind kind) {
    if (reply.kind == Reply::Kind::Error) {
        throw ClientError(reply.text);
    }
    if (reply.kind != kind) {
        throw ClientError("unexpected reply type from server");
    }
    return reply;
}

}  // namespace tcpkv
