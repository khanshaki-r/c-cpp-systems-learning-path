// m06_observer_lifetime.cpp
// OBSERVER: a subject notifies registered listeners when something happens.
// The classic implementation has a lifetime bug: the subject holds raw
// pointers to observers, an observer is destroyed without unsubscribing, the
// next notify() dereferences freed memory.
//
// Three implementations:
//   1. Naive (raw pointers)       -- shows the bug (commented, not executed)
//   2. Subscription token (RAII)  -- unsubscribes in the observer's destructor
//   3. weak_ptr                   -- subject detects dead observers
// plus the std::function variant.
//
// C equivalent: a callback list of {fn, void* ctx}. Same bug, no fix available
// in the type system; you must document "unregister before free".
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06b m06_observer_lifetime.cpp

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AccountEvent {
    std::string account;
    std::int64_t delta;
};

// ---------------------------------------------------------------------------
// 1. Naive observer. DO NOT USE. Kept to show the shape of the bug.
// ---------------------------------------------------------------------------
namespace naive {
class Observer {
public:
    virtual ~Observer() = default;
    virtual void on_event(const AccountEvent&) = 0;
};
class Subject {
public:
    void subscribe(Observer* o) { obs_.push_back(o); }
    void unsubscribe(Observer* o) { obs_.erase(std::remove(obs_.begin(), obs_.end(), o), obs_.end()); }
    void notify(const AccountEvent& e) {
        for (auto* o : obs_) o->on_event(e);   // if any o was destroyed: use-after-free
    }

private:
    std::vector<Observer*> obs_;
};
}  // namespace naive

// ---------------------------------------------------------------------------
// 2. Subscription token. subscribe() returns an RAII object; destroying it
//    unsubscribes. An observer that holds its token as a member can never
//    outlive its registration. Subject must outlive tokens (ownership tree:
//    the subject is owned higher up than its observers).
// ---------------------------------------------------------------------------
namespace token {

class Subject {
public:
    using Callback = std::function<void(const AccountEvent&)>;

    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subject* s, std::uint64_t id) : subject_(s), id_(id) {}
        ~Subscription() { reset(); }
        Subscription(Subscription&& o) noexcept : subject_(o.subject_), id_(o.id_) { o.subject_ = nullptr; }
        Subscription& operator=(Subscription&& o) noexcept {
            if (this != &o) { reset(); subject_ = o.subject_; id_ = o.id_; o.subject_ = nullptr; }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        void reset() {
            if (subject_) { subject_->unsubscribe(id_); subject_ = nullptr; }
        }

    private:
        Subject* subject_ = nullptr;
        std::uint64_t id_ = 0;
    };

    [[nodiscard]] Subscription subscribe(Callback cb) {   // nodiscard: dropping the token = instant unsubscribe
        std::uint64_t id = next_id_++;
        subs_.push_back({id, std::move(cb)});
        return Subscription(this, id);
    }

    void notify(const AccountEvent& e) {
        // Iterate over a COPY of the ids: a callback may subscribe/unsubscribe
        // during notify, which would invalidate iterators into subs_.
        std::vector<std::uint64_t> ids;
        ids.reserve(subs_.size());
        for (auto& s : subs_) ids.push_back(s.id);
        for (auto id : ids) {
            auto it = std::find_if(subs_.begin(), subs_.end(), [id](const Entry& s) { return s.id == id; });
            if (it != subs_.end()) it->cb(e);   // skip if unsubscribed meanwhile
        }
    }

    std::size_t count() const { return subs_.size(); }

private:
    struct Entry {
        std::uint64_t id;
        Callback cb;
    };
    void unsubscribe(std::uint64_t id) {
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [id](const Entry& s) { return s.id == id; }),
                    subs_.end());
    }
    std::vector<Entry> subs_;
    std::uint64_t next_id_ = 1;
};

// An observer that is SAFE BY CONSTRUCTION: it owns its subscription, so when
// it dies, the subscription dies, so the subject forgets it.
class AuditLog {
public:
    explicit AuditLog(Subject& s) : sub_(s.subscribe([this](const AccountEvent& e) { record(e); })) {}
    std::size_t entries() const { return lines_.size(); }

private:
    void record(const AccountEvent& e) { lines_.push_back(e.account + ":" + std::to_string(e.delta)); }
    std::vector<std::string> lines_;
    Subject::Subscription sub_;   // declared LAST: destroyed FIRST, before lines_ -> callback can never see dead members
};

}  // namespace token

// ---------------------------------------------------------------------------
// 3. weak_ptr observers. The subject cannot keep an observer alive, and it can
//    tell whether one is gone. Use when observers are already shared_ptr-managed
//    for other reasons. Otherwise prefer tokens (no forced heap, no atomics).
// ---------------------------------------------------------------------------
namespace weak {
class Observer {
public:
    virtual ~Observer() = default;
    virtual void on_event(const AccountEvent&) = 0;
};
class Subject {
public:
    void subscribe(const std::shared_ptr<Observer>& o) { obs_.push_back(o); }
    void notify(const AccountEvent& e) {
        for (auto it = obs_.begin(); it != obs_.end();) {
            if (auto sp = it->lock()) {   // lock(): promote to shared_ptr if still alive
                sp->on_event(e);
                ++it;
            } else {
                it = obs_.erase(it);      // dead: drop it
            }
        }
    }
    std::size_t count() const { return obs_.size(); }

private:
    std::vector<std::weak_ptr<Observer>> obs_;
};
class Printer final : public Observer {
public:
    void on_event(const AccountEvent& e) override { std::printf("  [weak printer] %s %lld\n", e.account.c_str(), static_cast<long long>(e.delta)); }
};
}  // namespace weak

int main() {
    // --- token-based ---
    std::printf("token subject:\n");
    token::Subject subject;
    {
        token::AuditLog audit(subject);
        auto sub2 = subject.subscribe([](const AccountEvent& e) {
            std::printf("  [lambda] %s %lld\n", e.account.c_str(), static_cast<long long>(e.delta));
        });
        subject.notify({"acct:1001", 500});
        std::printf("  subscribers = %zu, audit entries = %zu\n", subject.count(), audit.entries());
    }   // audit and sub2 destroyed here -> both unsubscribed automatically
    subject.notify({"acct:1001", -200});   // nobody listening; no crash
    std::printf("  subscribers after scope = %zu\n", subject.count());

    // --- weak_ptr-based ---
    std::printf("weak subject:\n");
    weak::Subject ws;
    {
        auto p = std::make_shared<weak::Printer>();
        ws.subscribe(p);
        ws.notify({"acct:2002", 100});
        std::printf("  observers = %zu\n", ws.count());
    }   // p destroyed; subject still holds an expired weak_ptr
    ws.notify({"acct:2002", 300});          // detects expiry, prunes
    std::printf("  observers after prune = %zu\n", ws.count());
    return 0;
}
