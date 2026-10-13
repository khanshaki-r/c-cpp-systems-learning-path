#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace minidb {

// RAII wrapper over a POSIX file descriptor with positioned I/O. Move-only.
// Every failure throws std::system_error carrying errno.
class File {
public:
    File() noexcept = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    static File open(const std::string& path, bool create);

    // Reads up to len bytes at offset; returns fewer only at end of file. Retries EINTR and
    // short reads.
    std::size_t read_at(std::uint64_t offset, char* buf, std::size_t len) const;
    // Writes all len bytes at offset or throws.
    void write_at(std::uint64_t offset, const char* buf, std::size_t len);
    void sync();  // fdatasync: data plus the metadata needed to read it back (size)
    void truncate(std::uint64_t size);
    std::uint64_t size() const;
    bool valid() const noexcept { return fd_ >= 0; }

private:
    explicit File(int fd) noexcept : fd_(fd) {}
    void close() noexcept;
    int fd_ = -1;
};

bool file_exists(const std::string& path);
void rename_file(const std::string& from, const std::string& to);
// fsync on the directory containing `path`, so a create or rename in it is durable.
void sync_parent_directory(const std::string& path);

}  // namespace minidb
