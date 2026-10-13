// m07_actor.cpp
// ACTOR MODEL: each actor owns its state, runs on its own thread (or is
// scheduled onto a pool), and communicates ONLY by sending immutable messages
// to other actors' mailboxes. No shared mutable state => no data races by
// construction. Lifetime: a message never carries a pointer into another actor;
// it carries values and actor addresses (handles).
//
// Scenario: an Account actor per account and a Transfer coordinator. A transfer
// is a small saga: debit source, then credit destination, with a compensating
// credit if the destination rejects. Messages are a std::variant (Module 06).
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m07e m07_actor.cpp

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Messages: plain values. Replies carry the address (name) of who to reply to.
// ---------------------------------------------------------------------------
struct Debit   { std::string reply_to; std::uint64_t txn; std::int64_t amount; };
struct Credit  { std::string reply_to; std::uint64_t txn; std::int64_t amount; };
struct Result  { std::uint64_t txn; bool ok; std::string from; std::string note; };
struct GetBalance { std::string reply_to; };
struct BalanceIs  { std::string account; std::int64_t balance; };
struct Shutdown {};

using Message = std::variant<Debit, Credit, Result, GetBalance, BalanceIs, Shutdown>;

// ---------------------------------------------------------------------------
// Mailbox: the only synchronised object. One per actor.
// ---------------------------------------------------------------------------
class Mailbox {
public:
    void put(Message m) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(m));
        }
        cv_.notify_one();
    }
    Message take() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !q_.empty(); });
        Message m = std::move(q_.front());
        q_.pop_front();
        return m;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Message> q_;
};

// ---------------------------------------------------------------------------
// The system: a registry of mailboxes by name. Actors send by NAME, never by
// pointer to another actor. (In Erlang: a PID; in Akka: an ActorRef.)
// ---------------------------------------------------------------------------
class ActorSystem {
public:
    Mailbox& mailbox(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& box = boxes_[name];
        if (!box) box = std::make_unique<Mailbox>();
        return *box;
    }
    void send(const std::string& to, Message m) { mailbox(to).put(std::move(m)); }

private:
    std::mutex mu_;
    std::map<std::string, std::unique_ptr<Mailbox>> boxes_;   // system OWNS all mailboxes
};

// Overload helper for std::visit.
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// ---------------------------------------------------------------------------
// Account actor. Its balance is touched by exactly one thread: its own.
// ---------------------------------------------------------------------------
void account_actor(ActorSystem& sys, std::string name, std::int64_t balance) {
    Mailbox& inbox = sys.mailbox(name);
    for (;;) {
        Message m = inbox.take();
        bool stop = std::visit(overloaded{
            [&](const Debit& d) {
                bool ok = d.amount <= balance;
                if (ok) balance -= d.amount;
                sys.send(d.reply_to, Result{d.txn, ok, name, ok ? "debited" : "insufficient funds"});
                return false;
            },
            [&](const Credit& c) {
                bool ok = c.amount > 0 && c.amount < 10000000;   // a toy "credit limit" rule to force a rejection
                if (ok) balance += c.amount;
                sys.send(c.reply_to, Result{c.txn, ok, name, ok ? "credited" : "credit rejected"});
                return false;
            },
            [&](const GetBalance& g) { sys.send(g.reply_to, BalanceIs{name, balance}); return false; },
            [&](const Shutdown&) { return true; },
            [&](const auto&) { return false; },   // messages this actor does not handle: ignore
        }, m);
        if (stop) break;
    }
}

// ---------------------------------------------------------------------------
// Transfer coordinator actor: drives the saga. Its state is the table of
// in-flight transfers. Again, single-threaded by construction.
// ---------------------------------------------------------------------------
struct Transfer { std::string from, to; std::int64_t amount; int stage; };   // stage 0 = debiting, 1 = crediting, 2 = compensating

void coordinator_actor(ActorSystem& sys, std::vector<Transfer> transfers, std::string report_to) {
    const std::string me = "coordinator";
    Mailbox& inbox = sys.mailbox(me);
    std::map<std::uint64_t, Transfer> inflight;
    std::uint64_t next_txn = 1;
    for (auto& t : transfers) {
        std::uint64_t txn = next_txn++;
        inflight[txn] = t;
        sys.send(t.from, Debit{me, txn, t.amount});
    }
    while (!inflight.empty()) {
        Message m = inbox.take();
        if (auto* r = std::get_if<Result>(&m)) {
            auto it = inflight.find(r->txn);
            if (it == inflight.end()) continue;
            Transfer& t = it->second;
            if (t.stage == 0) {                       // debit result
                if (!r->ok) {
                    sys.send(report_to, Result{r->txn, false, me, "transfer failed: " + r->note});
                    inflight.erase(it);
                } else {
                    t.stage = 1;
                    sys.send(t.to, Credit{me, r->txn, t.amount});
                }
            } else if (t.stage == 1) {                // credit result
                if (r->ok) {
                    sys.send(report_to, Result{r->txn, true, me, "transfer complete"});
                    inflight.erase(it);
                } else {
                    t.stage = 2;                      // compensate: give the money back
                    sys.send(t.from, Credit{me, r->txn, t.amount});
                }
            } else {                                  // compensation result
                sys.send(report_to, Result{r->txn, false, me, "transfer rolled back"});
                inflight.erase(it);
            }
        }
    }
}

int main() {
    ActorSystem sys;
    std::thread a1(account_actor, std::ref(sys), "acct:A", 1000);
    std::thread a2(account_actor, std::ref(sys), "acct:B", 500);

    std::vector<Transfer> plan = {
        {"acct:A", "acct:B", 300, 0},          // ok
        {"acct:B", "acct:A", 5000, 0},         // insufficient funds at debit
        {"acct:A", "acct:B", 20000000, 0},     // would fail credit rule, but fails debit first (A has 700)
        {"acct:A", "acct:B", 200, 0},          // ok
    };
    std::thread coord(coordinator_actor, std::ref(sys), plan, "main");

    Mailbox& my_inbox = sys.mailbox("main");
    for (std::size_t i = 0; i < plan.size(); ++i) {
        Message m = my_inbox.take();
        if (auto* r = std::get_if<Result>(&m))
            std::printf("txn %llu: %s (%s)\n", static_cast<unsigned long long>(r->txn), r->ok ? "ok" : "failed", r->note.c_str());
    }
    coord.join();

    sys.send("acct:A", GetBalance{"main"});
    sys.send("acct:B", GetBalance{"main"});
    for (int i = 0; i < 2; ++i) {
        Message m = my_inbox.take();
        if (auto* b = std::get_if<BalanceIs>(&m))
            std::printf("%s balance = %lld\n", b->account.c_str(), static_cast<long long>(b->balance));
    }

    sys.send("acct:A", Shutdown{});
    sys.send("acct:B", Shutdown{});
    a1.join();
    a2.join();
    return 0;
}
