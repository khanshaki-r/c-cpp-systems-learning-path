#include "tcpkv/fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace tcpkv {

void throw_system_error(int err, const char* what) {
    // generic_category() is the category for POSIX errno values; its messages come from strerror.
    throw std::system_error(err, std::generic_category(), what);
}

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
        // The return value is deliberately ignored. There is nothing useful to do with an
        // error from close() in a destructor, and on Linux the descriptor is gone either way.
        ::close(fd_);
    }
    fd_ = fd;
}

std::pair<FileDescriptor, FileDescriptor> make_pipe() {
    int fds[2] = {-1, -1};
    // pipe2 with O_CLOEXEC is atomic: no window in which a fork()ed child inherits the ends.
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw_system_error(errno, "pipe2");
    }
    return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

void set_nonblocking(int fd, bool enabled) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        throw_system_error(errno, "fcntl(F_GETFL)");
    }
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (wanted != flags && ::fcntl(fd, F_SETFL, wanted) < 0) {
        throw_system_error(errno, "fcntl(F_SETFL)");
    }
}

}  // namespace tcpkv
