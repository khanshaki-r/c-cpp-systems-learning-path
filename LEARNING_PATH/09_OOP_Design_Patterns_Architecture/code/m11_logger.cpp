// m11_logger.cpp
// LLD WALKTHROUGH: a logger library.
//
// Requirements (as clarified):
//   - Levels Trace < Debug < Info < Warn < Error; a runtime minimum level.
//   - Disabled levels cost almost nothing: no formatting, no allocation.
//   - Several sinks at once (console, file, in-memory for tests). Sinks are
//     pluggable by the application.
//   - Each record carries time, level, thread id, file:line, message.
//   - Async mode: the calling thread only enqueues; one background thread
//     writes. Bounded queue; when full, DROP and count (never block a request
//     thread on logging). flush() waits until everything queued is written.
//   - Safe shutdown: the destructor drains the queue and joins the thread.
//
//   +-----------+   records    +-------------+  write()  +------------------+
//   | LOG macro |------------->|   Logger    |---------->| Sink (interface) |
//   | LogLine   |  (by value)  | level, sinks|           +------------------+
//   +-----------+              | queue, thread|            ^       ^       ^
//                              +-------------+       Console  Memory  File...
//
// Patterns used: strategy (Sink), RAII (LogLine submits in its destructor;
// Logger joins in its destructor), producer-consumer (Module 07).
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11d m11_logger.cpp

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

inline const char* to_string(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF";
    }
    return "?";
}

// A record is a VALUE: it crosses threads, so it must own all its data. A
// const char* to a caller's buffer here would be a use-after-return bug.
struct Record {
    std::chrono::system_clock::time_point time;
    Level level;
    std::thread::id thread;
    const char* file;      // OK as raw pointer: __FILE__ is a string literal (static storage)
    int line;
    std::string message;
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const Record& r) = 0;
    virtual void flush() {}
};

class ConsoleSink final : public Sink {
public:
    void write(const Record& r) override {
        std::fprintf(stderr, "[%s] %s:%d %s\n", to_string(r.level), r.file, r.line, r.message.c_str());
    }
    void flush() override { std::fflush(stderr); }
};

// Test sink. Only the logger thread writes, but the test thread reads, so lock.
class MemorySink final : public Sink {
public:
    void write(const Record& r) override {
        std::lock_guard<std::mutex> g(m_);
        lines_.push_back(std::string(to_string(r.level)) + " " + r.message);
    }
    std::vector<std::string> lines() const { std::lock_guard<std::mutex> g(m_); return lines_; }
private:
    mutable std::mutex m_;
    std::vector<std::string> lines_;
};

enum class Mode { Sync, Async };

class Logger {
public:
    explicit Logger(Mode mode, std::size_t queue_capacity = 4096)
        : mode_(mode), capacity_(queue_capacity) {
        if (mode_ == Mode::Async) worker_ = std::thread([this] { run(); });
    }
    ~Logger() {
        if (mode_ == Mode::Async) {
            {
                std::lock_guard<std::mutex> g(m_);
                stopping_ = true;
            }
            cv_.notify_all();
            worker_.join();        // run() drains the queue before returning
        }
        for (auto& s : sinks_) s->flush();
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Configure before logging starts. Sinks are owned by the logger.
    void add_sink(std::unique_ptr<Sink> s) { sinks_.push_back(std::move(s)); }

    // Relaxed atomic: the level is a hint read on every log call; a thread
    // seeing the new value a few nanoseconds late is harmless.
    void set_level(Level l) noexcept { level_.store(l, std::memory_order_relaxed); }
    bool enabled(Level l) const noexcept { return l >= level_.load(std::memory_order_relaxed); }

    void submit(Record r) {
        if (mode_ == Mode::Sync) {
            std::lock_guard<std::mutex> g(m_);     // serialise sink writes: no interleaved lines
            for (auto& s : sinks_) s->write(r);
            return;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            if (queue_.size() >= capacity_) { ++dropped_; return; }   // never block the caller
            queue_.push_back(std::move(r));
        }
        cv_.notify_one();
    }

    // Blocks until every record queued BEFORE this call has been written.
    void flush() {
        if (mode_ == Mode::Async) {
            std::unique_lock<std::mutex> lk(m_);
            const auto target = enqueued_total_locked();
            drained_cv_.wait(lk, [&] { return written_ >= target; });
        }
        for (auto& s : sinks_) s->flush();
    }

    std::uint64_t dropped() const { std::lock_guard<std::mutex> g(m_); return dropped_; }

private:
    std::uint64_t enqueued_total_locked() const { return written_ + in_flight_ + queue_.size(); }

    void run() {
        std::deque<Record> batch;
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) return;
            batch.swap(queue_);                // take everything: one lock per batch, not per record
            in_flight_ = batch.size();
            lk.unlock();                       // write OUTSIDE the lock: sinks may be slow (disk)
            for (auto& r : batch)
                for (auto& s : sinks_) s->write(r);
            batch.clear();
            lk.lock();
            written_ += in_flight_;
            in_flight_ = 0;
            drained_cv_.notify_all();
        }
    }

    const Mode mode_;
    const std::size_t capacity_;
    std::atomic<Level> level_{Level::Info};
    std::vector<std::unique_ptr<Sink>> sinks_;

    mutable std::mutex m_;
    std::condition_variable cv_, drained_cv_;
    std::deque<Record> queue_;
    std::uint64_t dropped_ = 0, written_ = 0, in_flight_ = 0;
    bool stopping_ = false;
    std::thread worker_;       // LAST member: started after everything it touches exists
};

// RAII line builder: collects << pieces, submits once in the destructor.
class LogLine {
public:
    LogLine(Logger& lg, Level l, const char* file, int line) : lg_(lg), level_(l), file_(file), line_(line) {}
    ~LogLine() {
        lg_.submit(Record{std::chrono::system_clock::now(), level_, std::this_thread::get_id(),
                          file_, line_, os_.str()});
    }
    std::ostream& stream() { return os_; }
private:
    Logger& lg_;
    Level level_;
    const char* file_;
    int line_;
    std::ostringstream os_;
};

// The if/else shape makes the macro safe inside an unbraced if-else, and the
// stream expression to the right is NOT evaluated when the level is disabled.
#define LOG(lg, lvl) \
    if (!(lg).enabled(lvl)) {} else LogLine((lg), (lvl), __FILE__, __LINE__).stream()

static int g_side_effects = 0;
static int expensive() { ++g_side_effects; return 42; }

int main() {
    // 1. Sync mode, level filtering, no evaluation of disabled arguments.
    {
        auto mem = std::make_unique<MemorySink>();
        MemorySink* view = mem.get();          // non-owning observer for the test
        Logger lg(Mode::Sync);
        lg.add_sink(std::move(mem));
        lg.set_level(Level::Info);
        LOG(lg, Level::Debug) << "hidden " << expensive();
        LOG(lg, Level::Info) << "loan " << 1234 << " approved";
        if (true) LOG(lg, Level::Warn) << "branch"; else std::puts("never");
        assert(g_side_effects == 0);           // disabled line was never formatted
        auto lines = view->lines();
        assert(lines.size() == 2 && lines[0] == "INFO loan 1234 approved" && lines[1] == "WARN branch");
    }

    // 2. Async mode: ordering per producer, flush, drain on destruction.
    {
        auto mem = std::make_unique<MemorySink>();
        MemorySink* view = mem.get();
        {
            Logger lg(Mode::Async);
            lg.add_sink(std::move(mem));
            for (int i = 0; i < 100; ++i) LOG(lg, Level::Info) << i;
            lg.flush();
            auto lines = view->lines();
            assert(lines.size() == 100 && lines[0] == "INFO 0" && lines[99] == "INFO 99");

            std::vector<std::thread> ts;
            for (int t = 0; t < 4; ++t)
                ts.emplace_back([&lg] { for (int i = 0; i < 250; ++i) LOG(lg, Level::Error) << "x"; });
            for (auto& th : ts) th.join();
            // view must not be used after lg dies (lg owns the sink). Check now.
            lg.flush();
            assert(view->lines().size() == 1100);
            assert(lg.dropped() == 0);
        }
    }

    // 3. Bounded queue drops instead of blocking.
    {
        auto mem = std::make_unique<MemorySink>();
        Logger lg(Mode::Async, /*queue_capacity=*/4);
        lg.add_sink(std::move(mem));
        for (int i = 0; i < 10000; ++i) LOG(lg, Level::Info) << i;
        lg.flush();
        // Some were probably dropped; exactly how many depends on scheduling.
        std::printf("dropped under burst with capacity 4: %llu of 10000\n",
                    static_cast<unsigned long long>(lg.dropped()));
    }

    std::puts("m11_logger: all checks passed");
    return 0;
}
