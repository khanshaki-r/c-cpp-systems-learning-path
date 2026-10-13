// m10_optional_variant_error_code.cpp
// Error handling without exceptions: std::optional, std::error_code, std::variant + std::visit.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m10_optional_variant_error_code.cpp
#include <cerrno>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <variant>

// optional: "maybe a value". Replaces sentinels like -1, NULL, or an out-parameter plus a bool.
std::optional<int> parse_port(std::string_view s) {
    if (s.empty()) return std::nullopt;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
        if (v > 65535) return std::nullopt;
    }
    return v;
}

// error_code: a number plus a category. No allocation, no throw, carries a real errno or a library's
// own enum. The right tool at C API boundaries and on hot paths.
std::error_code open_device(std::string_view name) {
    if (name == "null") return {};                                                 // success: value 0
    if (name == "busy") return std::make_error_code(std::errc::device_or_resource_busy);
    errno = ENOENT;                                                                // pretend a syscall failed
    return std::error_code(errno, std::generic_category());                        // wrap the real errno
}

// variant: a type-safe union. Holds exactly one alternative; the compiler knows which types are possible.
struct Error {
    std::error_code code;
    std::string context;
};
using Result = std::variant<long long, Error>;

Result lookup_balance(const std::unordered_map<std::string, long long>& db, const std::string& acct) {
    if (auto it = db.find(acct); it != db.end()) return it->second;
    return Error{std::make_error_code(std::errc::no_such_file_or_directory), "account " + acct};
}

// The 'overloaded' helper turns a set of lambdas into one callable with several operator()s.
// std::visit then checks at COMPILE TIME that every alternative has a handler.
template <class... Fs> struct overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> overloaded(Fs...) -> overloaded<Fs...>;

// Protocol messages as a variant instead of a C tagged union with a hand-maintained 'type' field.
struct Get { std::string key; };
struct Put { std::string key; std::string value; };
struct Del { std::string key; };
using Message = std::variant<Get, Put, Del>;

int main() {
    for (std::string_view s : {"8080", "99999", "80a", ""}) {
        std::optional<int> p = parse_port(s);
        std::cout << '"' << s << "\" -> " << (p ? std::to_string(*p) : std::string("invalid"))
                  << "  value_or(0)=" << p.value_or(0) << '\n';
    }

    for (std::string_view n : {"null", "busy", "nope"}) {
        std::error_code ec = open_device(n);
        std::cout << n << ": " << (ec ? "FAILED " : "ok ") << ec.message()
                  << " [" << ec.category().name() << ':' << ec.value() << "]\n";
        if (ec == std::errc::device_or_resource_busy) std::cout << "  (retryable)\n";   // portable comparison
    }

    std::unordered_map<std::string, long long> db{{"A1", 5000}};
    for (const std::string acct : {"A1", "ZZ"}) {
        std::visit(overloaded{
                       [](long long bal) { std::cout << "balance=" << bal << '\n'; },
                       [](const Error& e) { std::cout << "error: " << e.code.message() << " (" << e.context << ")\n"; },
                   },
                   lookup_balance(db, acct));
    }

    Message msgs[] = {Get{"k1"}, Put{"k2", "v2"}, Del{"k3"}};
    for (const Message& m : msgs) {
        std::visit(overloaded{
                       [](const Get& g) { std::cout << "GET " << g.key << '\n'; },
                       [](const Put& p) { std::cout << "PUT " << p.key << '=' << p.value << '\n'; },
                       [](const Del& d) { std::cout << "DEL " << d.key << '\n'; },
                   },
                   m);
        std::cout << "  index=" << m.index() << " holds Put? " << std::holds_alternative<Put>(m) << '\n';
    }
    // std::get<Put>(msgs[0]);   // throws std::bad_variant_access at run time: msgs[0] holds a Get
    if (auto* g = std::get_if<Get>(&msgs[0])) std::cout << "get_if: " << g->key << '\n';   // null if wrong type
    return 0;
}
