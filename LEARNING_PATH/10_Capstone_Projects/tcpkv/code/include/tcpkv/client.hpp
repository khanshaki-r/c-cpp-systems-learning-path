#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tcpkv/socket.hpp"

namespace tcpkv {

// Thrown for "-ERR ..." replies and for malformed or unexpected replies.
// Transport failures surface as std::system_error from the Socket layer.
class ClientError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Reply {
    enum class Kind { Status, Error, Integer, Bulk, Null, Array };
    Kind kind = Kind::Null;
    std::string text;                // Status, Error, Bulk
    long long integer = 0;           // Integer
    std::vector<std::string> array;  // Array
};

// Blocking, single-connection client. Not thread-safe: one Client per thread.
class Client {
public:
    Client(const std::string& host, std::uint16_t port);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    ~Client() = default;

    std::string ping();
    std::optional<std::string> get(const std::string& key);
    void set(const std::string& key, std::string_view value);
    long long del(const std::vector<std::string>& keys);
    long long exists(const std::vector<std::string>& keys);
    std::vector<std::string> keys(std::string_view prefix = {});
    bool expire(const std::string& key, long long seconds);
    long long ttl(const std::string& key);
    std::string stats();
    void quit();

    // Sends an inline command ("GET key") and returns whatever the server replied,
    // including error replies. Used by the CLI.
    Reply execute(const std::vector<std::string>& tokens);

    // Sends pre-encoded bytes and reads one reply.
    Reply send_raw(std::string_view wire);

private:
    Reply read_reply();
    std::string read_line();
    std::string read_bulk(std::size_t length);
    void fill(std::size_t needed);  // read from the socket until buffer_ holds >= needed bytes
    static Reply expect(Reply reply, Reply::Kind kind);

    Socket socket_;
    std::string buffer_;  // bytes received but not yet consumed
};

}  // namespace tcpkv
