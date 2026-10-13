// m08_http_server.cpp - minimal HTTP/1.1 server written by hand: request line, headers, body,
// keep-alive, JSON responses. Thread per connection, blocking I/O.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m08_http_server.cpp -o http_server
// Run:    ./http_server 8080
// Test:   curl -i http://127.0.0.1:8080/health
//         curl -i -X PUT --data-binary 'hello' http://127.0.0.1:8080/kv/greeting
//         curl -i http://127.0.0.1:8080/kv/greeting
//         curl -i -X DELETE http://127.0.0.1:8080/kv/greeting
//         curl -v http://127.0.0.1:8080/health http://127.0.0.1:8080/stats   # one connection, two requests
//         printf 'GET /health HTTP/1.1\r\nHost: x\r\n\r\n' | nc -q1 127.0.0.1 8080
//
// What it deliberately does NOT do: TLS, chunked request bodies (replies 411), pipelining beyond
// what falls out of the read loop, compression, virtual hosts. See the module for what a library
// (cpp-httplib, Boost.Beast) adds.
#include "m03_socket.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxBodyBytes = 1 * 1024 * 1024;
constexpr int kIdleTimeoutMs = 15000;

struct Request {
    std::string method, target, version;
    std::map<std::string, std::string> headers; // names lower-cased
    std::string body;
    bool keep_alive = true;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    bool close = false;
};

struct Stats {
    std::atomic<uint64_t> connections{0}, requests{0}, bad_requests{0};
};

struct Store {
    std::mutex mu;
    std::unordered_map<std::string, std::string> kv;
};

Stats g_stats;
Store g_store;

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 505: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
std::string http_date() {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

// Minimal JSON string escaping so a value with quotes or newlines produces valid JSON.
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "\\u%04x", static_cast<unsigned>(c));
                out += hex;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

// ----- parsing ---------------------------------------------------------------------------------
enum class ParseResult { Ok, Bad, VersionUnsupported };

// `head` is everything up to and excluding the blank line. Fills method/target/version/headers.
ParseResult parse_head(std::string_view head, Request& req) {
    size_t eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    // Request line: METHOD SP TARGET SP VERSION
    size_t s1 = line.find(' ');
    if (s1 == std::string_view::npos) return ParseResult::Bad;
    size_t s2 = line.find(' ', s1 + 1);
    if (s2 == std::string_view::npos) return ParseResult::Bad;
    req.method = std::string(line.substr(0, s1));
    req.target = std::string(line.substr(s1 + 1, s2 - s1 - 1));
    req.version = std::string(line.substr(s2 + 1));
    if (req.method.empty() || req.target.empty() || req.target[0] != '/') return ParseResult::Bad;
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return ParseResult::VersionUnsupported;

    // Header lines: Name ":" OWS Value OWS
    size_t pos = eol == std::string_view::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string_view h = head.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
        pos = e == std::string_view::npos ? head.size() : e + 2;
        size_t colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return ParseResult::Bad;
        std::string name = lower(std::string(h.substr(0, colon)));
        if (name.find(' ') != std::string::npos) return ParseResult::Bad; // no whitespace before colon
        std::string value(trim(h.substr(colon + 1)));
        auto it = req.headers.find(name);
        if (it == req.headers.end()) req.headers.emplace(std::move(name), std::move(value));
        else it->second += ", " + value; // repeated header: combine (RFC 7230 3.2.2)
    }

    // Persistence: HTTP/1.1 defaults to keep-alive; HTTP/1.0 defaults to close.
    auto conn = req.headers.find("connection");
    std::string conn_v = conn == req.headers.end() ? "" : lower(conn->second);
    if (req.version == "HTTP/1.1") req.keep_alive = conn_v.find("close") == std::string::npos;
    else req.keep_alive = conn_v.find("keep-alive") != std::string::npos;
    return ParseResult::Ok;
}

// ----- routing ---------------------------------------------------------------------------------
Response handle(const Request& req) {
    Response r;
    const std::string& t = req.target;

    if (t == "/" && req.method == "GET") {
        r.content_type = "text/plain; charset=utf-8";
        r.body = "tcpkv http gateway\n  GET /health\n  GET /stats\n  GET|PUT|DELETE /kv/<key>\n";
        return r;
    }
    if (t == "/health") {
        if (req.method != "GET" && req.method != "HEAD") { r.status = 405; r.body = R"({"error":"method not allowed"})"; return r; }
        r.body = R"({"status":"ok"})";
        return r;
    }
    if (t == "/stats" && req.method == "GET") {
        r.body = "{\"connections\":" + std::to_string(g_stats.connections.load()) +
                 ",\"requests\":" + std::to_string(g_stats.requests.load()) +
                 ",\"bad_requests\":" + std::to_string(g_stats.bad_requests.load()) + "}";
        return r;
    }
    if (t.rfind("/kv/", 0) == 0 && t.size() > 4) {
        std::string key = t.substr(4);
        std::lock_guard<std::mutex> lk(g_store.mu);
        if (req.method == "GET") {
            auto it = g_store.kv.find(key);
            if (it == g_store.kv.end()) { r.status = 404; r.body = R"({"error":"not found"})"; return r; }
            r.body = "{\"key\":" + json_escape(key) + ",\"value\":" + json_escape(it->second) + "}";
            return r;
        }
        if (req.method == "PUT") {
            bool existed = g_store.kv.count(key) > 0;
            g_store.kv[key] = req.body;
            r.status = existed ? 204 : 201;   // idempotent: same PUT twice => same end state
            if (r.status == 201) r.body = "{\"key\":" + json_escape(key) + "}";
            return r;
        }
        if (req.method == "DELETE") {
            g_store.kv.erase(key);
            r.status = 204;                   // idempotent: deleting a missing key is still "gone"
            return r;
        }
        r.status = 405;
        r.body = R"({"error":"method not allowed"})";
        return r;
    }
    r.status = 404;
    r.body = R"({"error":"no such resource"})";
    return r;
}

std::string serialize(const Response& r, bool head_only) {
    std::string out;
    out += "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    out += "Date: " + http_date() + "\r\n";
    out += "Server: tcpkv-http/0.1\r\n";
    if (r.status != 204) {
        out += "Content-Type: " + r.content_type + "\r\n";
        out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    }
    out += r.close ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    out += "\r\n";
    if (!head_only && r.status != 204) out += r.body;
    return out;
}

// ----- connection loop ---------------------------------------------------------------------------
void serve(net::Fd conn) {
    ++g_stats.connections;
    const int fd = conn.get();
    std::string buf; // unconsumed bytes; may already hold the next request (pipelining)
    char tmp[16 * 1024];
    try {
        net::set_nodelay(fd);
        net::set_recv_timeout(fd, kIdleTimeoutMs);
        for (;;) {
            // 1. Read until the blank line that ends the head.
            size_t head_end;
            while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
                if (buf.size() > kMaxHeaderBytes) {
                    net::write_all(fd, serialize(Response{400, "application/json", R"({"error":"headers too large"})", true}, false));
                    return;
                }
                size_t n = net::read_some(fd, tmp, sizeof tmp);
                if (n == 0) return; // client closed between requests (or mid-head: nothing to answer)
                buf.append(tmp, n);
            }

            Request req;
            ParseResult pr = parse_head(std::string_view(buf).substr(0, head_end), req);
            if (pr != ParseResult::Ok) {
                ++g_stats.bad_requests;
                int st = pr == ParseResult::Bad ? 400 : 505;
                net::write_all(fd, serialize(Response{st, "application/json", R"({"error":"bad request"})", true}, false));
                return; // we cannot trust where the next request begins
            }
            buf.erase(0, head_end + 4);

            // 2. Body: only Content-Length is supported. Chunked request bodies => 411.
            if (req.headers.count("transfer-encoding")) {
                net::write_all(fd, serialize(Response{411, "application/json", R"({"error":"chunked requests not supported"})", true}, false));
                return;
            }
            size_t content_length = 0;
            if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
                const std::string& v = it->second;
                if (v.empty() || v.size() > 9 || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    net::write_all(fd, serialize(Response{400, "application/json", R"({"error":"bad content-length"})", true}, false));
                    return;
                }
                content_length = std::stoul(v);
                if (content_length > kMaxBodyBytes) {
                    net::write_all(fd, serialize(Response{413, "application/json", R"({"error":"body too large"})", true}, false));
                    return;
                }
            }
            while (buf.size() < content_length) {
                size_t n = net::read_some(fd, tmp, sizeof tmp);
                if (n == 0) return; // truncated body: nothing sensible to reply
                buf.append(tmp, n);
            }
            req.body = buf.substr(0, content_length);
            buf.erase(0, content_length);

            // 3. Dispatch and reply.
            ++g_stats.requests;
            Response resp = handle(req);
            resp.close = !req.keep_alive;
            net::write_all(fd, serialize(resp, req.method == "HEAD"));
            if (resp.close) return;
            // keep-alive: loop; `buf` may already contain the next request's bytes.
        }
    } catch (const std::system_error& e) {
        int ec = e.code().value();
        if (ec != EAGAIN && ec != EWOULDBLOCK && ec != ECONNRESET && ec != EPIPE)
            std::fprintf(stderr, "[http] %s\n", e.what());
        // EAGAIN here means the idle timeout fired: closing is the correct response.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[http] %s\n", e.what());
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* port = argc > 1 ? argv[1] : "8080";
    net::ignore_sigpipe();
    net::Fd listener;
    try {
        listener = net::listen_tcp(nullptr, port, 256);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "listen: %s\n", e.what());
        return EXIT_FAILURE;
    }
    std::printf("http server on port %s\n", port);
    for (;;) {
        net::Fd c = net::accept_tcp(listener.get());
        if (!c) break;
        std::thread(serve, std::move(c)).detach();
    }
    return EXIT_SUCCESS;
}
