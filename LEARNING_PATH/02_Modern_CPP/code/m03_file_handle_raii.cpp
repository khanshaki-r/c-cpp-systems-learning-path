// m03_file_handle_raii.cpp
// fopen/fclose rewritten as RAII. The destructor runs on normal return AND during exception unwinding.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_file_handle_raii.cpp
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

class FileHandle {
public:
    FileHandle(const char* path, const char* mode) : f_(std::fopen(path, mode)) {
        // Acquire in the constructor. If acquisition fails, the object never exists: throw.
        if (!f_) throw std::system_error(errno, std::generic_category(), std::string("fopen ") + path);
    }
    ~FileHandle() {
        // Release in the destructor. Runs exactly once, on every path out of the owning scope.
        if (f_) { std::fclose(f_); std::cout << "  fclose done\n"; }
    }
    FileHandle(const FileHandle&) = delete;              // two owners of one FILE* = double fclose
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) {
            if (f_) std::fclose(f_);
            f_ = o.f_;
            o.f_ = nullptr;
        }
        return *this;
    }
    std::FILE* get() const noexcept { return f_; }
private:
    std::FILE* f_;
};

void write_report(const char* path, bool fail_midway) {
    FileHandle out(path, "w");
    std::fputs("header\n", out.get());
    if (fail_midway) throw std::runtime_error("disk quota exceeded (simulated)");
    std::fputs("body\n", out.get());
    // In C you would need: fclose(f); return; on EVERY exit path, or a goto cleanup label.
}   // 'out' is destroyed here on both the normal path and the throwing path

int main() {
    const char* path = "/tmp/m03_report.txt";
    try {
        std::cout << "normal path:\n";
        write_report(path, false);
        std::cout << "failing path:\n";
        write_report(path, true);
        std::cout << "not reached\n";
    } catch (const std::exception& e) {
        std::cout << "caught: " << e.what() << '\n';
    }

    try {
        FileHandle missing("/nonexistent/dir/x.txt", "r");   // constructor throws: no object, no destructor
        std::cout << "not reached\n";
    } catch (const std::system_error& e) {
        std::cout << "caught system_error: " << e.what() << " (errno " << e.code().value() << ")\n";
    }
    std::remove(path);
    return 0;
}
