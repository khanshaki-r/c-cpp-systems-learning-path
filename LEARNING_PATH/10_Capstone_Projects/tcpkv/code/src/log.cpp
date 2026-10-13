#include "tcpkv/log.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace tcpkv::log {

namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_write_mutex;

std::string timestamp_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
    ::localtime_r(&seconds, &tm);  // localtime() is not thread-safe; localtime_r is
    char buf[32];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);

    std::string out(buf, n);
    out += '.';
    out += static_cast<char>('0' + millis / 100);
    out += static_cast<char>('0' + (millis / 10) % 10);
    out += static_cast<char>('0' + millis % 10);
    return out;
}

}  // namespace

void set_level(Level level) noexcept { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

Level level() noexcept { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

const char* to_string(Level level) noexcept {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

bool parse_level(std::string_view text, Level& out) noexcept {
    if (text == "debug") {
        out = Level::Debug;
    } else if (text == "info") {
        out = Level::Info;
    } else if (text == "warn" || text == "warning") {
        out = Level::Warn;
    } else if (text == "error") {
        out = Level::Error;
    } else {
        return false;
    }
    return true;
}

void write(Level level, std::string_view message) {
    std::string line;
    line.reserve(message.size() + 64);
    line += timestamp_now();
    line += ' ';
    line += to_string(level);
    line += " [tid ";
    line += std::to_string(static_cast<long>(::gettid()));  // kernel thread id, matches top/gdb
    line += "] ";
    line.append(message);
    line += '\n';

    std::lock_guard<std::mutex> lock(g_write_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace tcpkv::log
