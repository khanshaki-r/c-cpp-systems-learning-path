// m05_pimpl.cpp
// PIMPL ("pointer to implementation"), the C++ form of the BRIDGE pattern.
// The public class holds one pointer to a private struct defined in the .cpp.
// Effects:
//   - Compile firewall: changing private members does not recompile clients.
//   - ABI stability: sizeof(Connection) never changes, so a shared library can
//     change internals without clients rebuilding.
//   - Hides heavy or platform headers (<winsock2.h>, <libpq-fe.h>) from the public header.
// Cost: one heap allocation per object, one indirection per call, boilerplate.
//
// C equivalent: an opaque struct ("struct conn;" in the header, defined in
// conn.c). That IS pimpl; C programmers invented it. C++ adds RAII and type-safe
// move so the opaque pointer cannot leak.
//
// This single file is laid out as it would be split: header, then source, then
// client. Only the header section would be visible to clients.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m05e m05_pimpl.cpp

// =============================== connection.h ==============================
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class Connection {
public:
    explicit Connection(std::string host, std::uint16_t port);
    ~Connection();                                   // MUST be defined in the .cpp (see below)

    // Move-only. Declared here, defined in the .cpp, because the compiler needs
    // the complete Impl type to generate them.
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool send(std::string_view bytes);
    std::string describe() const;
    std::uint64_t bytes_sent() const;

private:
    struct Impl;                                     // declared, not defined: clients see only the name
    std::unique_ptr<Impl> impl_;                     // sizeof(Connection) == sizeof(void*), forever
};

// =============================== connection.cpp ============================
#include <cstdio>
#include <vector>

// Everything in here can change freely. Clients never recompile.
struct Connection::Impl {
    std::string host;
    std::uint16_t port;
    int fake_fd;
    std::uint64_t bytes_sent = 0;
    std::vector<std::string> send_log;   // added later for debugging: no client recompile

    Impl(std::string h, std::uint16_t p) : host(std::move(h)), port(p), fake_fd(42) {}
};

Connection::Connection(std::string host, std::uint16_t port)
    : impl_(std::make_unique<Impl>(std::move(host), port)) {}

// The destructor is defined here, where Impl is complete. If it were
// defaulted in the header, unique_ptr<Impl>'s deleter would be instantiated
// in client code with an incomplete type and fail to compile.
Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

bool Connection::send(std::string_view bytes) {
    impl_->bytes_sent += bytes.size();
    impl_->send_log.emplace_back(bytes);
    return true;
}

std::string Connection::describe() const {
    return impl_->host + ":" + std::to_string(impl_->port) + " (fd " + std::to_string(impl_->fake_fd) + ")";
}

std::uint64_t Connection::bytes_sent() const { return impl_->bytes_sent; }

// =============================== client.cpp ================================
// A client that includes only connection.h. Note: it never names Impl, and it
// would not need to be recompiled if Impl gained a field.

static Connection make_and_use() {
    Connection c("10.0.0.5", 5432);
    c.send("SELECT 1");
    c.send("SELECT balance FROM accounts WHERE id = 1001");
    return c;   // moves (or elides); works because move ops are declared
}

int main() {
    Connection c = make_and_use();
    std::printf("%s sent %llu bytes\n", c.describe().c_str(), static_cast<unsigned long long>(c.bytes_sent()));
    std::printf("sizeof(Connection) = %zu (one pointer, regardless of Impl)\n", sizeof(Connection));

    Connection d = std::move(c);   // c now holds a null impl_; using c would be a bug
    std::printf("moved: %s\n", d.describe().c_str());
    return 0;
}
