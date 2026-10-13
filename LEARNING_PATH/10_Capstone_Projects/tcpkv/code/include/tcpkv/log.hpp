#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace tcpkv::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level) noexcept;
Level level() noexcept;
const char* to_string(Level level) noexcept;
bool parse_level(std::string_view text, Level& out) noexcept;  // "debug" | "info" | "warn" | "error"

// Writes one line to stderr: "2026-10-06T10:00:00.123 INFO  [tid 4242] message".
// Thread-safe: the whole line is formatted first and written with one call under a mutex.
void write(Level level, std::string_view message);

// Builds a message by streaming every argument. No format strings, nothing to get wrong.
template <class... Args>
std::string format(const Args&... args) {
    std::ostringstream os;
    (os << ... << args);
    return os.str();
}

template <class... Args>
void debug(const Args&... args) {
    if (level() <= Level::Debug) write(Level::Debug, format(args...));
}
template <class... Args>
void info(const Args&... args) {
    if (level() <= Level::Info) write(Level::Info, format(args...));
}
template <class... Args>
void warn(const Args&... args) {
    if (level() <= Level::Warn) write(Level::Warn, format(args...));
}
template <class... Args>
void error(const Args&... args) {
    if (level() <= Level::Error) write(Level::Error, format(args...));
}

}  // namespace tcpkv::log
