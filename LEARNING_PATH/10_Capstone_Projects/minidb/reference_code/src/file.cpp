#include "minidb/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>
#include <system_error>

namespace minidb {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

File::~File() { close(); }

File::File(File&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void File::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);  // errors on close of a file we fsynced are not actionable here
        fd_ = -1;
    }
}

File File::open(const std::string& path, bool create) {
    int flags = O_RDWR | O_CLOEXEC;
    if (create) {
        flags |= O_CREAT;
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        throw_errno("open(" + path + ")");
    }
    return File(fd);
}

std::size_t File::read_at(std::uint64_t offset, char* buf, std::size_t len) const {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pread(fd_, buf + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("pread");
        }
        if (n == 0) {
            break;  // end of file
        }
        done += static_cast<std::size_t>(n);
    }
    return done;
}

void File::write_at(std::uint64_t offset, const char* buf, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pwrite(fd_, buf + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("pwrite");
        }
        done += static_cast<std::size_t>(n);
    }
}

void File::sync() {
    if (::fdatasync(fd_) != 0) {
        throw_errno("fdatasync");
    }
}

void File::truncate(std::uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        throw_errno("ftruncate");
    }
}

std::uint64_t File::size() const {
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        throw_errno("fstat");
    }
    return static_cast<std::uint64_t>(st.st_size);
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

void rename_file(const std::string& from, const std::string& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throw_errno("rename(" + from + ", " + to + ")");
    }
}

void sync_parent_directory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        throw_errno("open(" + dir + ")");
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) {
        errno = saved;
        throw_errno("fsync(" + dir + ")");
    }
}

}  // namespace minidb
