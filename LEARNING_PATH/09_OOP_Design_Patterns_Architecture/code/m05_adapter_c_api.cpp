// m05_adapter_c_api.cpp
// ADAPTER: wrap a C API (modelled on libpq) so that it fits a C++ interface
// (Store) and so that resource management is RAII instead of "remember to call
// PQclear and PQfinish on every path".
//
// The fake C library below has the SHAPE of libpq: opaque handles, manual
// free functions, status codes, char* results. Replace fakepq_* with PQ* and
// this is real code.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m05a m05_adapter_c_api.cpp

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// ===========================================================================
// Fake C library (what you cannot change). Everything is C-style on purpose.
// ===========================================================================
extern "C" {

struct fakepq_conn;            // opaque
struct fakepq_result;          // opaque

enum fakepq_status { FAKEPQ_OK = 0, FAKEPQ_BAD = 1 };

fakepq_conn* fakepq_connect(const char* conninfo);
void fakepq_finish(fakepq_conn* c);
fakepq_status fakepq_conn_status(const fakepq_conn* c);

fakepq_result* fakepq_exec(fakepq_conn* c, const char* sql);
fakepq_status fakepq_result_status(const fakepq_result* r);
int fakepq_ntuples(const fakepq_result* r);
const char* fakepq_getvalue(const fakepq_result* r, int row, int col);
void fakepq_clear(fakepq_result* r);

}  // extern "C"

// --- fake implementation (in a real program this is libpq.so) ---
struct fakepq_conn {
    std::map<std::string, std::string> table;   // our "database"
    bool ok;
};
struct fakepq_result {
    fakepq_status status;
    std::string value;   // at most one row, one column in this fake
    int rows;
};

fakepq_conn* fakepq_connect(const char* conninfo) {
    auto* c = new fakepq_conn{};
    c->ok = std::strstr(conninfo, "host=") != nullptr;
    return c;
}
void fakepq_finish(fakepq_conn* c) { delete c; }
fakepq_status fakepq_conn_status(const fakepq_conn* c) { return c->ok ? FAKEPQ_OK : FAKEPQ_BAD; }

fakepq_result* fakepq_exec(fakepq_conn* c, const char* sql) {
    auto* r = new fakepq_result{FAKEPQ_OK, "", 0};
    // "toy SQL": "PUT k v" or "GET k"
    const char* sp = std::strchr(sql, ' ');
    if (!sp) { r->status = FAKEPQ_BAD; return r; }
    std::string op(sql, static_cast<std::size_t>(sp - sql));
    std::string rest(sp + 1);
    if (op == "PUT") {
        auto sp2 = rest.find(' ');
        if (sp2 == std::string::npos) { r->status = FAKEPQ_BAD; return r; }
        c->table[rest.substr(0, sp2)] = rest.substr(sp2 + 1);
    } else if (op == "GET") {
        auto it = c->table.find(rest);
        if (it != c->table.end()) { r->value = it->second; r->rows = 1; }
    } else {
        r->status = FAKEPQ_BAD;
    }
    return r;
}
fakepq_status fakepq_result_status(const fakepq_result* r) { return r->status; }
int fakepq_ntuples(const fakepq_result* r) { return r->rows; }
const char* fakepq_getvalue(const fakepq_result* r, int, int) { return r->value.c_str(); }
void fakepq_clear(fakepq_result* r) { delete r; }

// ===========================================================================
// The C++ side: the interface our application wants.
// ===========================================================================
class Store {
public:
    virtual ~Store() = default;
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
};

// ===========================================================================
// Step 1 of adapting a C API: RAII wrappers for every handle type. These are
// tiny, and they make every later line exception-safe and leak-free.
// ===========================================================================
struct ConnDeleter   { void operator()(fakepq_conn* c) const { fakepq_finish(c); } };
struct ResultDeleter { void operator()(fakepq_result* r) const { fakepq_clear(r); } };

using ConnPtr   = std::unique_ptr<fakepq_conn, ConnDeleter>;
using ResultPtr = std::unique_ptr<fakepq_result, ResultDeleter>;

// ===========================================================================
// Step 2: the adapter. Implements the C++ interface by translating each call
// into the C API, converting status codes to exceptions or optionals, and
// char* to std::string. The rest of the program never sees fakepq_*.
// ===========================================================================
class PqStoreAdapter final : public Store {
public:
    explicit PqStoreAdapter(const std::string& conninfo) : conn_(fakepq_connect(conninfo.c_str())) {
        // Constructor either yields a usable object or throws. No half-open state.
        if (!conn_ || fakepq_conn_status(conn_.get()) != FAKEPQ_OK)
            throw std::runtime_error("connect failed: " + conninfo);
    }

    void put(const std::string& key, const std::string& value) override {
        ResultPtr r(fakepq_exec(conn_.get(), ("PUT " + key + " " + value).c_str()));
        if (fakepq_result_status(r.get()) != FAKEPQ_OK) throw std::runtime_error("put failed for " + key);
        // r is cleared here automatically, on every path.
    }

    std::optional<std::string> get(const std::string& key) const override {
        ResultPtr r(fakepq_exec(conn_.get(), ("GET " + key).c_str()));
        if (fakepq_result_status(r.get()) != FAKEPQ_OK) throw std::runtime_error("get failed for " + key);
        if (fakepq_ntuples(r.get()) == 0) return std::nullopt;
        return std::string(fakepq_getvalue(r.get(), 0, 0));   // copy before r is cleared
    }

private:
    ConnPtr conn_;   // one member, one owner; ~PqStoreAdapter calls fakepq_finish via deleter
};

// Application code: depends on Store only.
static void run(Store& s) {
    s.put("acct:1001", "balance=500");
    auto v = s.get("acct:1001");
    auto missing = s.get("acct:4242");
    std::printf("acct:1001 -> %s\n", v ? v->c_str() : "(none)");
    std::printf("acct:4242 -> %s\n", missing ? missing->c_str() : "(none)");
}

int main() {
    try {
        PqStoreAdapter store("host=127.0.0.1 dbname=bank");
        run(store);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
    }

    try {
        PqStoreAdapter bad("dbname=bank");   // no host= -> fake lib reports BAD
        run(bad);
    } catch (const std::exception& e) {
        std::printf("expected failure: %s\n", e.what());
    }
    return 0;
}
