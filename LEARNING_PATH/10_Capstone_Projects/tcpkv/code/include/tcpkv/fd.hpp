#pragma once

#include <utility>

namespace tcpkv {

// Throws std::system_error built from an errno value.
// Every wrapper that talks to the kernel uses this so error reporting is uniform.
[[noreturn]] void throw_system_error(int err, const char* what);

// Owning handle for one POSIX file descriptor.
//
// Rules:
//   * exactly one owner: copying is deleted, moving transfers ownership
//   * the destructor closes the descriptor exactly once; a moved-from handle holds -1
//   * close() is NOT retried on EINTR: on Linux the descriptor is already released
//     when close() returns EINTR, and retrying could close a descriptor that another
//     thread has just been handed by the kernel
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // Gives up ownership without closing. The caller now owns the raw descriptor.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // Closes the current descriptor (if any) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

    friend void swap(FileDescriptor& a, FileDescriptor& b) noexcept { std::swap(a.fd_, b.fd_); }

private:
    int fd_ = -1;
};

// Creates a pipe with O_CLOEXEC on both ends. Returns {read_end, write_end}.
// Throws std::system_error on failure.
std::pair<FileDescriptor, FileDescriptor> make_pipe();

// Sets or clears O_NONBLOCK on a descriptor. Throws std::system_error on failure.
void set_nonblocking(int fd, bool enabled);

}  // namespace tcpkv
