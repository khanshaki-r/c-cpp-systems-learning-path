// m11_kv_transactions.cpp
// LLD WALKTHROUGH: an in-memory key-value store with NESTED transactions.
//
// Requirements (the classic interview version):
//   SET k v, GET k, UNSET k, COUNT v  (how many keys currently hold value v)
//   BEGIN, ROLLBACK, COMMIT
//   - Transactions nest. ROLLBACK undoes only the innermost open transaction.
//   - COMMIT makes all open transactions permanent (variant: commit innermost
//     into its parent; both are shown).
//   - ROLLBACK/COMMIT with no open transaction -> "NO TRANSACTION".
//   - GET and COUNT must be O(1) regardless of how many transactions are open.
//
// Design chosen: UNDO LOG. Writes go straight to the live map; each open
// transaction records, per key it touches, the value BEFORE its first write.
// ROLLBACK replays that log. Reads never look at the log, so they are O(1).
//
// Alternative (described in the module): OVERLAY layers, one map of pending
// writes per transaction, with reads searching from the top layer down. Reads
// become O(depth), but uncommitted writes are invisible to other readers,
// which is what you need for isolation between concurrent clients.
//
// This is the same idea as a database's undo segment (Oracle, InnoDB) versus
// a write set applied at commit (optimistic / MVCC engines).
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11c m11_kv_transactions.cpp

#include <cassert>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class TxKvStore {
public:
    std::optional<std::string> get(const std::string& k) const {
        auto it = data_.find(k);
        if (it == data_.end()) return std::nullopt;
        return it->second;
    }

    void set(const std::string& k, const std::string& v) { write(k, v); }
    void unset(const std::string& k) { write(k, std::nullopt); }

    std::size_t count(const std::string& v) const {
        auto it = counts_.find(v);
        return it == counts_.end() ? 0 : it->second;
    }

    void begin() { txs_.emplace_back(); }

    // [[nodiscard]]: a caller that ignores "there was nothing to roll back"
    // almost certainly has a bug in its own bookkeeping.
    [[nodiscard]] bool rollback() {
        if (txs_.empty()) return false;
        Tx tx = std::move(txs_.back());
        txs_.pop_back();
        // Order does not matter: the log holds ONE entry per key (the value
        // before the first write in this transaction), so replaying is a set
        // of independent restores. raw_write does not log (no tx is "open"
        // for this undo; we just popped it).
        for (auto& [key, before] : tx.undo) raw_write(key, before);
        return true;
    }

    // Flat commit: everything open becomes permanent.
    [[nodiscard]] bool commit_all() {
        if (txs_.empty()) return false;
        txs_.clear();
        return true;
    }

    // Nested commit: the innermost transaction's changes become part of its
    // parent, so a later ROLLBACK of the parent still undoes them. The parent
    // keeps ITS OWN before-image if it already had one for that key.
    [[nodiscard]] bool commit_innermost() {
        if (txs_.empty()) return false;
        Tx tx = std::move(txs_.back());
        txs_.pop_back();
        if (!txs_.empty()) {
            auto& parent = txs_.back().undo;
            for (auto& [key, before] : tx.undo) parent.emplace(key, std::move(before)); // emplace keeps existing
        }
        return true;
    }

    std::size_t depth() const noexcept { return txs_.size(); }

private:
    struct Tx {
        // key -> value before this transaction first touched it (nullopt = absent)
        std::unordered_map<std::string, std::optional<std::string>> undo;
    };

    void write(const std::string& k, std::optional<std::string> v) {
        if (!txs_.empty()) txs_.back().undo.emplace(k, get(k));  // first write wins
        raw_write(k, std::move(v));
    }

    // The ONLY function that changes data_; it keeps counts_ consistent. One
    // choke point for an invariant is the C lesson "one function owns the
    // struct", enforced by private access.
    void raw_write(const std::string& k, std::optional<std::string> v) {
        auto it = data_.find(k);
        if (it != data_.end()) {
            if (v && *v == it->second) return;            // no-op write
            if (--counts_[it->second] == 0) counts_.erase(it->second);
            if (!v) { data_.erase(it); return; }          // UNSET
            it->second = std::move(*v);                   // overwrite
            ++counts_[it->second];
        } else if (v) {                                   // insert
            auto ins = data_.emplace(k, std::move(*v)).first;
            ++counts_[ins->second];
        }                                                 // unset of absent key: nothing
    }

    std::unordered_map<std::string, std::string> data_;
    std::unordered_map<std::string, std::size_t> counts_;   // value -> number of keys
    std::vector<Tx> txs_;                                    // stack of open transactions
};

// ---------------------------------------------------------------------------
// A thin command interpreter, because the interview often gives the problem as
// "read commands from stdin". Keep parsing OUT of the store: the store is the
// model, this is the protocol layer (Module 09's layering in miniature).
// ---------------------------------------------------------------------------
std::string execute(TxKvStore& db, const std::string& line) {
    std::istringstream in(line);
    std::string cmd, a, b;
    in >> cmd >> a >> b;
    if (cmd == "SET")      { db.set(a, b); return ""; }
    if (cmd == "GET")      { auto v = db.get(a); return v ? *v : "NULL"; }
    if (cmd == "UNSET")    { db.unset(a); return ""; }
    if (cmd == "COUNT")    { return std::to_string(db.count(a)); }
    if (cmd == "BEGIN")    { db.begin(); return ""; }
    if (cmd == "ROLLBACK") { return db.rollback() ? "" : "NO TRANSACTION"; }
    if (cmd == "COMMIT")   { return db.commit_all() ? "" : "NO TRANSACTION"; }
    return "ERR unknown command";
}

int main() {
    // 1. Basic operations and COUNT.
    {
        TxKvStore db;
        db.set("a", "10"); db.set("b", "10");
        assert(db.count("10") == 2);
        db.set("b", "20");
        assert(db.count("10") == 1 && db.count("20") == 1);
        db.unset("a");
        assert(!db.get("a") && db.count("10") == 0);
        db.set("b", "20");                               // no-op write keeps count
        assert(db.count("20") == 1);
    }

    // 2. Nested rollback restores exactly the inner changes.
    {
        TxKvStore db;
        db.set("a", "10");
        db.begin();
        db.set("a", "20");
        db.begin();
        db.set("a", "30");
        db.set("new", "x");
        assert(db.get("a") == std::string("30"));
        assert(db.rollback());
        assert(db.get("a") == std::string("20") && !db.get("new"));
        assert(db.rollback());
        assert(db.get("a") == std::string("10"));
        assert(!db.rollback());                          // nothing open
        assert(db.count("10") == 1 && db.count("20") == 0 && db.count("30") == 0);
    }

    // 3. Unset inside a transaction is undone.
    {
        TxKvStore db;
        db.set("k", "v");
        db.begin();
        db.unset("k");
        db.set("k", "w");                                // second write: log keeps "v"
        assert(db.rollback());
        assert(db.get("k") == std::string("v") && db.count("v") == 1 && db.count("w") == 0);
    }

    // 4. commit_innermost merges into parent; parent rollback undoes both.
    {
        TxKvStore db;
        db.set("a", "1");
        db.begin();
        db.set("a", "2");
        db.begin();
        db.set("a", "3");
        db.set("b", "9");
        assert(db.commit_innermost());
        assert(db.depth() == 1 && db.get("a") == std::string("3"));
        assert(db.rollback());
        assert(db.get("a") == std::string("1") && !db.get("b"));
    }

    // 5. The textual protocol, the way the interviewer will test it.
    {
        TxKvStore db;
        const char* script[][2] = {
            {"SET a 10", ""},       {"BEGIN", ""},          {"SET a 20", ""},
            {"GET a", "20"},        {"BEGIN", ""},          {"UNSET a", ""},
            {"GET a", "NULL"},      {"ROLLBACK", ""},       {"GET a", "20"},
            {"COMMIT", ""},         {"GET a", "20"},        {"ROLLBACK", "NO TRANSACTION"},
            {"COUNT 20", "1"},      {"FOO", "ERR unknown command"},
        };
        for (auto& step : script) assert(execute(db, step[0]) == step[1]);
    }

    std::puts("m11_kv_transactions: all checks passed");
    return 0;
}
