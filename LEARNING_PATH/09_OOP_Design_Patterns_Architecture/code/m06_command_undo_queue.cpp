// m06_command_undo_queue.cpp
// COMMAND: turn an operation into an object so it can be queued, logged,
// undone, replayed or sent across a thread. Two uses shown:
//   1. Undo/redo stack over a key-value document
//   2. A request queue where commands carry everything needed to run later
//
// C equivalent: struct { int type; union { ... } args; } pushed on a queue, with
// a switch to execute and another to undo. The C++ version keeps execute and
// undo next to each other per command, so they cannot drift apart.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06c m06_command_undo_queue.cpp

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The receiver: the thing commands act upon.
class Document {
public:
    void set(const std::string& k, const std::string& v) { data_[k] = v; }
    bool erase(const std::string& k) { return data_.erase(k) > 0; }
    std::optional<std::string> get(const std::string& k) const {
        auto it = data_.find(k);
        return it == data_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::string dump() const {
        std::string s = "{";
        for (auto& [k, v] : data_) s += k + "=" + v + " ";
        return s + "}";
    }

private:
    std::map<std::string, std::string> data_;
};

// ---------------------------------------------------------------------------
// 1. Undoable commands. Each command captures what it needs to reverse itself
//    AT EXECUTE TIME (the previous value), not at construction.
// ---------------------------------------------------------------------------
class Command {
public:
    virtual ~Command() = default;
    virtual void execute(Document& d) = 0;
    virtual void undo(Document& d) = 0;
    virtual std::string describe() const = 0;
};

class SetCommand final : public Command {
public:
    SetCommand(std::string k, std::string v) : key_(std::move(k)), value_(std::move(v)) {}
    void execute(Document& d) override {
        previous_ = d.get(key_);     // remember state for undo
        d.set(key_, value_);
    }
    void undo(Document& d) override {
        if (previous_) d.set(key_, *previous_);
        else d.erase(key_);
    }
    std::string describe() const override { return "set " + key_ + "=" + value_; }

private:
    std::string key_, value_;
    std::optional<std::string> previous_;
};

class EraseCommand final : public Command {
public:
    explicit EraseCommand(std::string k) : key_(std::move(k)) {}
    void execute(Document& d) override {
        previous_ = d.get(key_);
        d.erase(key_);
    }
    void undo(Document& d) override {
        if (previous_) d.set(key_, *previous_);
    }
    std::string describe() const override { return "erase " + key_; }

private:
    std::string key_;
    std::optional<std::string> previous_;
};

// A macro command: a transaction of several commands, undone in reverse.
class CompositeCommand final : public Command {
public:
    void add(std::unique_ptr<Command> c) { parts_.push_back(std::move(c)); }
    void execute(Document& d) override {
        for (auto& c : parts_) c->execute(d);
    }
    void undo(Document& d) override {
        for (auto it = parts_.rbegin(); it != parts_.rend(); ++it) (*it)->undo(d);
    }
    std::string describe() const override { return "batch(" + std::to_string(parts_.size()) + ")"; }

private:
    std::vector<std::unique_ptr<Command>> parts_;
};

class History {
public:
    explicit History(Document& d) : doc_(d) {}
    void run(std::unique_ptr<Command> c) {
        c->execute(doc_);
        done_.push_back(std::move(c));
        undone_.clear();   // a new action invalidates the redo stack
    }
    bool undo() {
        if (done_.empty()) return false;
        auto c = std::move(done_.back());
        done_.pop_back();
        c->undo(doc_);
        undone_.push_back(std::move(c));
        return true;
    }
    bool redo() {
        if (undone_.empty()) return false;
        auto c = std::move(undone_.back());
        undone_.pop_back();
        c->execute(doc_);
        done_.push_back(std::move(c));
        return true;
    }

private:
    Document& doc_;
    std::vector<std::unique_ptr<Command>> done_;    // history OWNS executed commands
    std::vector<std::unique_ptr<Command>> undone_;
};

// ---------------------------------------------------------------------------
// 2. Request queue. When commands do not need undo, a std::function is the
//    whole pattern: the closure IS the command object. This is how a thread
//    pool or an event loop queues work (Module 07).
// ---------------------------------------------------------------------------
class RequestQueue {
public:
    using Task = std::function<void(Document&)>;
    void post(Task t) { q_.push_back(std::move(t)); }
    std::size_t drain(Document& d) {
        std::size_t n = 0;
        while (!q_.empty()) {
            Task t = std::move(q_.front());
            q_.pop_front();
            t(d);
            ++n;
        }
        return n;
    }

private:
    std::deque<Task> q_;
};

int main() {
    Document doc;
    History hist(doc);

    hist.run(std::make_unique<SetCommand>("acct:1", "100"));
    hist.run(std::make_unique<SetCommand>("acct:2", "200"));
    hist.run(std::make_unique<SetCommand>("acct:1", "150"));
    std::printf("after 3 sets:  %s\n", doc.dump().c_str());

    hist.undo();
    std::printf("undo 1:        %s\n", doc.dump().c_str());
    hist.undo();
    std::printf("undo 2:        %s\n", doc.dump().c_str());
    hist.redo();
    std::printf("redo:          %s\n", doc.dump().c_str());

    auto batch = std::make_unique<CompositeCommand>();
    batch->add(std::make_unique<EraseCommand>("acct:1"));
    batch->add(std::make_unique<SetCommand>("acct:3", "300"));
    hist.run(std::move(batch));
    std::printf("batch:         %s\n", doc.dump().c_str());
    hist.undo();
    std::printf("undo batch:    %s\n", doc.dump().c_str());

    // Queue: closures carrying their arguments, executed later.
    RequestQueue queue;
    for (int i = 0; i < 3; ++i) {
        std::string key = "q:" + std::to_string(i);
        queue.post([key, i](Document& d) { d.set(key, std::to_string(i * 10)); });
    }
    std::size_t ran = queue.drain(doc);
    std::printf("queue ran %zu: %s\n", ran, doc.dump().c_str());
    return 0;
}
