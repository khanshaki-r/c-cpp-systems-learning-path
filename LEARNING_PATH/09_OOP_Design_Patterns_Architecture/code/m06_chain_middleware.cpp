// m06_chain_middleware.cpp
// CHAIN OF RESPONSIBILITY: pass a request along a sequence of handlers; each
// may handle it, modify it, or pass it on. The modern server form is
// "middleware": auth -> rate limit -> logging -> business handler, where each
// layer wraps the next as a callable.
//
// Two forms:
//   1. Classic linked handlers with a virtual handle() and a next pointer
//   2. Middleware as std::function composition (what web frameworks do)
//
// C equivalent: an array of handler fn pointers walked until one returns
// "handled", or nested callbacks with void* next.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06g m06_chain_middleware.cpp

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Request {
    std::string client;
    std::string token;
    std::string path;
};
struct Response {
    int status;
    std::string body;
};

// ---------------------------------------------------------------------------
// 1. Classic chain. Each handler owns the next (unique_ptr list).
// ---------------------------------------------------------------------------
namespace classic {

class Handler {
public:
    virtual ~Handler() = default;
    void set_next(std::unique_ptr<Handler> n) { next_ = std::move(n); }
    Response handle(const Request& r) {
        if (auto resp = try_handle(r)) return *resp;          // this link answered
        if (next_) return next_->handle(r);                     // pass along
        return Response{404, "no handler"};                     // end of chain
    }

private:
    virtual std::optional<Response> try_handle(const Request& r) = 0;   // NVI (Module 06 template method)
    std::unique_ptr<Handler> next_;
};

class AuthHandler final : public Handler {
    std::optional<Response> try_handle(const Request& r) override {
        if (r.token != "secret") return Response{401, "unauthorised"};   // stop the chain
        return std::nullopt;                                             // let it continue
    }
};

class StaticHandler final : public Handler {
    std::optional<Response> try_handle(const Request& r) override {
        if (r.path == "/health") return Response{200, "ok"};
        return std::nullopt;
    }
};

class KvHandler final : public Handler {
    std::optional<Response> try_handle(const Request& r) override {
        if (r.path.rfind("/kv/", 0) == 0) return Response{200, "value-of-" + r.path.substr(4)};
        return std::nullopt;
    }
};

}  // namespace classic

// ---------------------------------------------------------------------------
// 2. Middleware. A Handler is a function Request -> Response. A Middleware is a
//    function (Handler next) -> Handler: it receives the rest of the chain and
//    returns a new handler that does its work around calling next. Composition
//    is just function application, in order.
// ---------------------------------------------------------------------------
namespace middleware {

using Handler = std::function<Response(const Request&)>;
using Middleware = std::function<Handler(Handler next)>;

Middleware auth() {
    return [](Handler next) {
        return [next](const Request& r) -> Response {
            if (r.token != "secret") return {401, "unauthorised"};
            return next(r);
        };
    };
}

// Stateful middleware: the lambda captures a shared counter table. Note the
// shared_ptr: the table must outlive every copy of the handler closure, and
// std::function copies closures. This is one of the legitimate shared_ptr uses.
Middleware rate_limit(int max_per_client) {
    auto counts = std::make_shared<std::map<std::string, int>>();
    return [counts, max_per_client](Handler next) {
        return [counts, max_per_client, next](const Request& r) -> Response {
            if (++(*counts)[r.client] > max_per_client) return {429, "rate limited"};
            return next(r);
        };
    };
}

Middleware logging(std::vector<std::string>& log) {
    return [&log](Handler next) {
        return [&log, next](const Request& r) -> Response {
            Response resp = next(r);                    // AFTER the inner chain: can log the status
            log.push_back(r.client + " " + r.path + " -> " + std::to_string(resp.status));
            return resp;
        };
    };
}

// Build the chain: the first middleware in the list is the OUTERMOST.
Handler compose(std::vector<Middleware> chain, Handler terminal) {
    Handler h = std::move(terminal);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) h = (*it)(std::move(h));   // wrap inside-out
    return h;
}

}  // namespace middleware

int main() {
    Request reqs[] = {
        {"alice", "secret", "/health"},
        {"alice", "secret", "/kv/acct:1"},
        {"alice", "secret", "/kv/acct:2"},
        {"bob", "wrong", "/kv/acct:1"},
        {"alice", "secret", "/nothing"},
    };

    std::printf("classic chain:\n");
    auto chain = std::make_unique<classic::AuthHandler>();
    auto stat = std::make_unique<classic::StaticHandler>();
    auto kv = std::make_unique<classic::KvHandler>();
    stat->set_next(std::move(kv));
    chain->set_next(std::move(stat));
    for (const auto& r : reqs) {
        Response resp = chain->handle(r);
        std::printf("  %-6s %-12s -> %d %s\n", r.client.c_str(), r.path.c_str(), resp.status, resp.body.c_str());
    }

    std::printf("middleware:\n");
    std::vector<std::string> log;
    middleware::Handler app = middleware::compose(
        {middleware::logging(log), middleware::auth(), middleware::rate_limit(2)},
        [](const Request& r) -> Response {
            if (r.path == "/health") return {200, "ok"};
            if (r.path.rfind("/kv/", 0) == 0) return {200, "value-of-" + r.path.substr(4)};
            return {404, "not found"};
        });
    for (const auto& r : reqs) {
        Response resp = app(r);
        std::printf("  %-6s %-12s -> %d %s\n", r.client.c_str(), r.path.c_str(), resp.status, resp.body.c_str());
    }
    std::printf("  access log (%zu lines), last: %s\n", log.size(), log.back().c_str());
    return 0;
}
