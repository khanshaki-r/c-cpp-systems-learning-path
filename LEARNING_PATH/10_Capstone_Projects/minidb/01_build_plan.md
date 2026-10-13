# minidb build plan: eight steps

**Time:** ~50 hours over 4 weeks (Mon 2026-11-10 to Sun 2026-12-07), two steps a week.
**Optional:** start only after tcpkv v1.0.0 is tagged, public and pinned (README of this course).
**Read first, for the whole project:** 07_Databases_Internals: `02_storage_engines_pages_and_heap_files.md`,
`03_btree_indexes_in_depth.md`, `04_wal_and_crash_recovery.md`. 05_Memory_Debugging_RCA:
`08_valgrind_and_sanitizers.md`. 06_Linux_Toolchain_DevOps: `04_make_and_cmake_from_scratch.md`,
`05_testing_with_googletest_and_ctest.md`. tcpkv steps 01 and 08 (the CMake presets, the CI
matrix and the sanitizer setup are reused unchanged). The specification is `00_spec.md`;
read it fully before step 1.

## How to use this plan

Same rules as tcpkv (README of this course, section 5): read the step, type the code yourself,
build, run the tests, commit with the given message, and write the interview paragraph in your
own words before moving on.

**State of the code in this plan.** The five key classes, `Pager`, `PageCache`, `BPlusTree`,
`Wal` and `Recovery`, are given in full, together with the small headers they need
(`common.hpp`, `crc32`, `page.hpp`) and the `Db` class that wires them together. This exact
code was compiled with `-std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
-Woverloaded-virtual -Werror` on GCC 13.3 in WSL Ubuntu 24.04, and it passes all 21 tests
shown here, the SIGKILL crash test included, both at `-O2` and under AddressSanitizer plus
UndefinedBehaviorSanitizer. `src/file.cpp`, the CLI and the benchmark are outlined: you have
written their tcpkv equivalents already.

The first draft of this code had two recovery bugs, and its own tests caught both. They are
kept in the text as stories (steps 6 and 7), because "my test caught my bug, here is the test"
is the most convincing thing you can say in an interview about testing storage code.

**Repository.** `github.com/khanshaki-r/minidb`, laid out as in `00_spec.md` section 10.
Copy `CMakePresets.json`, `.clang-format`, `.gitignore`, `LICENSE` and the CI workflow from
tcpkv, and rename `TCPKV_SANITIZER` to `MINIDB_SANITIZER` in the presets. Work under
`~/dev/minidb` in WSL, not under `/mnt/d`: `fdatasync` on the Windows file system through WSL
is both slow and not a fair test of Linux durability.

| step | title | week | hours |
|-----:|-------|------|------:|
| 1 | Skeleton, `File`, CRC-32, the page format | 6 (Nov 10–16) | 5 |
| 2 | `Pager`: pages on disk, checksums, atomic creation | 6 | 5 |
| 3 | `PageCache`: pinning, LRU, dirty pages, no-steal | 7 (Nov 17–23) | 7 |
| 4 | `BPlusTree` I: node format, search, insert, splits, invariant checker | 7 | 8 |
| 5 | `BPlusTree` II: erase, range scan, model-based test | 8 (Nov 24–30) | 5 |
| 6 | `Wal`: records, LSNs, sync, replay, truncate, reset | 8 | 6 |
| 7 | `Recovery` and `Db`: the commit protocol and checkpoints | 9 (Dec 01–07) | 8 |
| 8 | The crash test, CLI, benchmark, CI, README, tag v0.1.0 | 9 | 6 |

---

## Step 1. Skeleton, `File`, CRC-32, the page format

**Time:** ~5 hours   **Week:** 6
**Read first:** 07_Databases_Internals/02_storage_engines_pages_and_heap_files.md (pages, why
fixed-size blocks). 06_Linux_Toolchain_DevOps/04_make_and_cmake_from_scratch.md.
02_Modern_CPP: `03_raii_the_core_idea.md`, `11_const_constexpr_and_compile_time.md` (the
`constexpr` CRC table). tcpkv step 02 (`FileDescriptor`, the same RAII idea).

### Goal

A repository that builds and tests in CI, an RAII file class with positioned I/O, a checksum,
and the definition of a page. Nothing is stored yet; everything later stands on this.

In plain words: before you can build a library you need the shelves (pages), a way to put a
shelf into a numbered slot of the warehouse and take it back out (`pwrite`/`pread` at
`id * 4096`), and a seal on every shelf that breaks if the contents change behind your back
(the CRC).

### Design

* **4 KiB pages.** It is the x86 memory page size, the usual ext4 block size, and the unit the
  kernel page cache works in. One page is one I/O. Most engines use 4, 8 or 16 KiB.
* **A checksum on every page.** Disks, controllers and filesystems occasionally return wrong
  bytes, and a crash can leave a page half old and half new (a *torn write*: the drive
  persists 512-byte or 4 KiB sectors, and a power cut can land between them). Without a
  checksum you would read garbage as data. With one you detect it, and the log repairs it
  (step 7).
* **The page LSN in the header.** The log sequence number of the log record that last carried
  this page. Recovery uses it to skip work. Defined now so the format never changes.
* **`memcpy`, never pointer casts**, for fixed-size fields: `*(uint32_t*)(p + 4)` violates
  strict aliasing and may be misaligned, which is undefined behaviour that UBSan reports.
  A 4-byte `memcpy` compiles to one `mov` anyway.
* **`pread`/`pwrite`**, not `lseek` plus `read`: no shared file offset (no race if two threads
  ever share the descriptor), and one syscall instead of two.
* **`fdatasync`, not `fsync`**: it skips metadata that is not needed to read the data back
  (the modification time) but still flushes the file size. Cheaper, same durability here.

### Files

`CMakeLists.txt`, `CMakePresets.json` (copied), `.github/workflows/ci.yml` (copied: keep the
`build-and-test` matrix; drop the tcpkv `valgrind` job), `include/minidb/common.hpp`,
`include/minidb/crc32.hpp`, `src/crc32.cpp`, `include/minidb/file.hpp`, `src/file.cpp`,
`include/minidb/page.hpp`, `tests/test_util.hpp`, `tests/storage_test.cpp` (the `Crc32` test
only, for now).

### Code

`CMakeLists.txt`, final version. In step 1 list only `src/crc32.cpp` and `src/file.cpp` in
the library and only `storage` in the test loop, and leave out the two executables; add each
piece in the step that introduces it.

```cmake
cmake_minimum_required(VERSION 3.22)

project(minidb
    VERSION 0.1.0
    DESCRIPTION "Page-based key-value storage engine with a B+tree and a write-ahead log"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(MINIDB_BUILD_TESTS "Build the GoogleTest suite" ON)
set(MINIDB_SANITIZER "" CACHE STRING "Sanitizer to enable: address | thread | undefined | (empty)")

find_package(Threads REQUIRED)

add_library(minidb_warnings INTERFACE)
target_compile_options(minidb_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Werror)

if(MINIDB_SANITIZER)
    message(STATUS "minidb: building with -fsanitize=${MINIDB_SANITIZER}")
    add_compile_options(-fsanitize=${MINIDB_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${MINIDB_SANITIZER})
endif()

add_library(minidb STATIC
    src/crc32.cpp
    src/file.cpp
    src/pager.cpp
    src/page_cache.cpp
    src/wal.cpp
    src/recovery.cpp
    src/btree.cpp
    src/db.cpp)
target_include_directories(minidb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minidb PUBLIC Threads::Threads PRIVATE minidb_warnings)

add_executable(minidb_cli apps/minidb_cli.cpp)
target_link_libraries(minidb_cli PRIVATE minidb minidb_warnings)

add_executable(minidb_bench tools/bench.cpp)
target_link_libraries(minidb_bench PRIVATE minidb minidb_warnings)

if(MINIDB_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)
    foreach(name IN ITEMS storage btree db crash)
        add_executable(${name}_test tests/${name}_test.cpp)
        target_include_directories(${name}_test PRIVATE tests)
        target_link_libraries(${name}_test PRIVATE minidb GTest::gtest_main minidb_warnings)
        # fsync-bound tests are slow, more so under sanitizers: generous timeouts.
        gtest_discover_tests(${name}_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 900)
    endforeach()
endif()
```

`include/minidb/common.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace minidb {

using PageId = std::uint32_t;
using Lsn = std::uint64_t;  // log sequence number: byte position in the logical WAL stream

inline constexpr std::size_t kPageSize = 4096;
inline constexpr PageId kMetaPageId = 0;
inline constexpr PageId kNoPage = 0;  // page 0 is the meta page, so 0 never names a tree node

// Chosen so that at least four maximum-size leaf cells fit in one page; that guarantees a
// byte-balanced split always produces two halves that fit (see btree.cpp).
inline constexpr std::size_t kMaxKeySize = 128;
inline constexpr std::size_t kMaxValueSize = 880;

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The on-disk bytes are not what we wrote: bad checksum, bad magic, impossible field values.
class CorruptionError : public DbError {
public:
    using DbError::DbError;
};

}  // namespace minidb
```

`include/minidb/crc32.hpp` and `src/crc32.cpp`. The table is computed by a `constexpr`
function, so the compiler builds it and it lives in read-only data: no start-up cost and no
static-initialisation-order question.

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), the one zlib and Ethernet use.
// Chainable: crc32(b, nb, crc32(a, na)) == crc32(a followed by b).
std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t previous = 0) noexcept;

}  // namespace minidb
```

```cpp
#include "minidb/crc32.hpp"

#include <array>

namespace minidb {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();  // built at compile time

}  // namespace

std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t previous) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = previous ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace minidb
```

`include/minidb/file.hpp`:

```cpp
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
```

`src/file.cpp`, outlined. You wrote its twin in tcpkv step 02; the differences:

* `open`: `::open(path, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0644)`; on failure throw
  `std::system_error(errno, std::generic_category(), "open(" + path + ")")`.
* `read_at`: loop `::pread(fd_, buf + done, len - done, offset + done)`; retry on `EINTR`; stop
  at a return of 0 (end of file) and return the count, so the caller can see a short read.
* `write_at`: loop `::pwrite` until every byte is written; retry on `EINTR`.
* `sync`: `::fdatasync(fd_)`. `truncate`: `::ftruncate`. `size`: `::fstat(...).st_size`.
* Move operations take the descriptor and set the source's to -1; the destructor closes it.
* `rename_file`: `std::rename`, throwing on failure.
* `sync_parent_directory(path)`: open the directory that contains `path` with
  `O_RDONLY | O_DIRECTORY | O_CLOEXEC`, `fsync` it, close it. This is what makes a newly
  created or renamed file's *name* durable. Without it a power cut can lose the directory
  entry even though the file's data was synced.

`include/minidb/page.hpp`, the format every later step depends on:

```cpp
#pragma once

#include <array>
#include <cstring>

#include "minidb/common.hpp"
#include "minidb/crc32.hpp"

namespace minidb {

// Fixed-width fields are stored in host byte order with memcpy (never a pointer cast, which
// would break strict aliasing and alignment rules). x86-64 and ARM64 Linux are both
// little-endian; a portable format would convert explicitly.
template <class T>
T read_as(const char* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

template <class T>
void write_as(char* p, T value) noexcept {
    std::memcpy(p, &value, sizeof value);
}

enum class PageType : std::uint16_t { Free = 0, Meta = 1, Leaf = 2, Internal = 3 };

// Every page starts with a 16-byte header:
//   [0, 4)   CRC-32 of bytes [4, kPageSize)
//   [4, 6)   PageType
//   [6, 8)   reserved, zero
//   [8, 16)  page LSN: the LSN of the WAL record that carried this page image
inline constexpr std::size_t kPageHeaderSize = 16;
inline constexpr std::size_t kPagePayloadSize = kPageSize - kPageHeaderSize;

struct Page {
    alignas(64) std::array<char, kPageSize> bytes{};

    PageType type() const noexcept { return static_cast<PageType>(read_as<std::uint16_t>(bytes.data() + 4)); }
    void set_type(PageType type) noexcept { write_as(bytes.data() + 4, static_cast<std::uint16_t>(type)); }
    Lsn lsn() const noexcept { return read_as<Lsn>(bytes.data() + 8); }
    void set_lsn(Lsn lsn) noexcept { write_as(bytes.data() + 8, lsn); }

    char* payload() noexcept { return bytes.data() + kPageHeaderSize; }
    const char* payload() const noexcept { return bytes.data() + kPageHeaderSize; }
    void clear_payload() noexcept { std::memset(payload(), 0, kPagePayloadSize); }

    // Computes and stores the checksum. Call after the last modification, before any write.
    void seal() noexcept { write_as(bytes.data(), crc32(bytes.data() + 4, kPageSize - 4)); }
    // A page of all zeros (never written, or a hole) fails this check: crc32 of zeros is not 0.
    bool verify() const noexcept {
        return read_as<std::uint32_t>(bytes.data()) == crc32(bytes.data() + 4, kPageSize - 4);
    }
};

// Page 0. Logged and recovered like every other page.
//   payload: u64 magic | u32 version | u32 root | u32 page_count
struct Meta {
    static constexpr std::uint64_t kMagic = 0x3142444E494D2121ULL;  // arbitrary, fixed forever
    static constexpr std::uint32_t kVersion = 1;

    PageId root = kNoPage;
    std::uint32_t page_count = 0;  // pages allocated so far, including page 0

    static Meta decode(const Page& page) {
        const char* p = page.payload();
        if (page.type() != PageType::Meta || read_as<std::uint64_t>(p) != kMagic) {
            throw CorruptionError("meta page: bad type or magic (not a minidb file?)");
        }
        if (read_as<std::uint32_t>(p + 8) != kVersion) {
            throw CorruptionError("meta page: unsupported format version");
        }
        Meta meta;
        meta.root = read_as<std::uint32_t>(p + 12);
        meta.page_count = read_as<std::uint32_t>(p + 16);
        if (meta.root == kNoPage || meta.root >= meta.page_count) {
            throw CorruptionError("meta page: root page out of range");
        }
        return meta;
    }

    void encode(Page& page) const noexcept {
        page.set_type(PageType::Meta);
        page.clear_payload();
        char* p = page.payload();
        write_as(p, kMagic);
        write_as(p + 8, kVersion);
        write_as(p + 12, root);
        write_as(p + 16, page_count);
    }
};

}  // namespace minidb
```

`static_cast<PageType>` of an arbitrary `uint16_t` read from disk is well defined because the
enum has a fixed underlying type; an unknown value matches no enumerator and the tree code
rejects it. `alignas(64)` starts each page on a cache-line boundary; it is a nicety, not a
correctness requirement.

`tests/test_util.hpp` (a fresh temporary directory per test, removed afterwards):

```cpp
#pragma once

#include <stdlib.h>

#include <filesystem>
#include <string>
#include <system_error>

// A fresh directory under /tmp, removed (with its contents) when the object dies.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "minidb_test_XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path_ = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};
```

The start of `tests/storage_test.cpp`; the file grows in steps 2, 3 and 6:

```cpp
// Pager, PageCache and Wal unit tests.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "minidb/btree.hpp"
#include "minidb/page_cache.hpp"
#include "minidb/pager.hpp"
#include "minidb/wal.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

Page page_with_byte(char c) {
    Page page;
    page.set_type(PageType::Leaf);
    std::memset(page.payload(), c, kPagePayloadSize);
    return page;
}

}  // namespace

TEST(Crc32, KnownVector) {
    // The standard check value for CRC-32/ISO-HDLC.
    EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
    // Chaining equals one pass.
    EXPECT_EQ(crc32("6789", 4, crc32("12345", 5)), 0xCBF43926u);
}
```

`0xCBF43926` is the published check value of this CRC variant (CRC-32/ISO-HDLC, the one zlib
uses) for the input `"123456789"`. Testing against a published vector is how you know you
implemented *the* CRC-32 and not *a* CRC.

### Build and run (WSL Ubuntu 24.04)

```bash
mkdir -p ~/dev && cd ~/dev && git init minidb && cd minidb
# create the files above; copy presets, .clang-format, .gitignore, LICENSE and ci.yml from tcpkv
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
```

### Tests

`Crc32.KnownVector` passes under `debug` and `asan`. CI is green on the first push.

### Commit

```bash
git add -A
git commit -m "Project skeleton: CMake presets, CI, RAII File, CRC-32, page format

- File: pread/pwrite loops with EINTR, fdatasync, ftruncate, atomic rename, directory fsync
- crc32: table built at compile time; tested against the published check value
- Page: 4 KiB with a 16-byte header (CRC, type, page LSN); seal/verify; Meta page codec"
git push -u origin main
```

### What to say in an interview

"Every page is 4 KiB with a 16-byte header: a CRC-32 over the rest of the page, the page type,
and the page LSN, the log sequence number of the last log record that carried this page. The
checksum is how the engine notices a torn or corrupted page instead of returning garbage; the
LSN lets recovery skip pages that are already up to date. Fields go through `memcpy`, which
avoids strict-aliasing and alignment undefined behaviour and costs nothing after optimisation."

---

## Step 2. `Pager`: pages on disk, checksums, atomic creation

**Time:** ~5 hours   **Week:** 6
**Read first:** 07_Databases_Internals/02_storage_engines_pages_and_heap_files.md (page
addressing, torn pages). 06_Linux_Toolchain_DevOps/01_linux_for_a_windows_developer.md (the
page cache, `fsync`). 05_Memory_Debugging_RCA/04_the_catalogue_of_memory_bugs.md (why every
length read from disk must be bounds-checked).

### Goal

Read and write page N of the data file, checksummed, and create a new database file so that a
crash during creation can never leave a half-initialised database.

### Design

* **Page N at offset N * 4096.** No indirection. The meta page (page 0) holds the root page id
  and the page count.
* **Seal on write, verify on read.** `write()` computes the CRC; `read()` throws
  `CorruptionError` on a mismatch or a short read.
* **`read_if_valid()` for recovery.** During recovery a missing or torn page is an expected
  state, not an error: it is about to be overwritten. A separate function says so in its name.
* **Atomic creation:** write `<path>.tmp`, `fdatasync` it, `rename` it over `<path>`, `fsync`
  the directory. `rename(2)` replaces the name atomically, so after a crash there is either no
  database or a complete one.
* **No caching, no policy.** The Pager does raw I/O; deciding *when* to write is the cache's
  job. One class, one responsibility, testable alone.

### Files

`include/minidb/pager.hpp`, `src/pager.cpp`; add `src/pager.cpp` to the library. The tests
call `BPlusTree::initial_pages()`, which arrives in step 4; until then use a local helper in
the test that returns two pages: a meta page (`Meta{1, 2}.encode(page0)`) and an empty leaf
(`page1.set_type(PageType::Leaf)`).

### Code

```cpp
#pragma once

#include <string>
#include <vector>

#include "minidb/file.hpp"
#include "minidb/page.hpp"

namespace minidb {

// Raw page I/O on the single data file: page N lives at byte offset N * kPageSize.
// Checksums are applied on write and verified on read. No caching, no policy: that is the
// PageCache's job. Not thread-safe; the Db serialises access.
class Pager {
public:
    // Opens an existing data file. Throws std::system_error if it does not exist.
    explicit Pager(const std::string& path);

    // Creates a new data file atomically: writes `pages` (page i at index i) to path + ".tmp",
    // fsyncs it, renames it over `path`, fsyncs the directory. A crash leaves either no file
    // or a complete one, never a half-initialised database.
    static void create(const std::string& path, std::vector<Page> pages);

    // Reads and verifies page `id`. Throws CorruptionError on a short read or bad checksum.
    void read(PageId id, Page& out) const;

    // Recovery helper: like read(), but reports a missing or torn page as `false` instead
    // of throwing, because during redo that is an expected state, not corruption.
    bool read_if_valid(PageId id, Page& out) const;

    // Seals (checksums) the page and writes it in place. Not durable until sync().
    void write(PageId id, Page& page);

    void sync() { file_.sync(); }

    // Pages physically present in the file (may be fewer than Meta::page_count after a
    // crash: allocated pages are only written at eviction or checkpoint).
    PageId file_page_count() const;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    File file_;
};

}  // namespace minidb
```

```cpp
#include "minidb/pager.hpp"

#include <string>
#include <utility>

namespace minidb {

namespace {

std::uint64_t offset_of(PageId id) noexcept { return static_cast<std::uint64_t>(id) * kPageSize; }

}  // namespace

// The file size need not be a multiple of kPageSize: a crash while extending the file can
// leave a partial last page. Such a page was written at eviction or checkpoint, so its
// committed image is in the WAL; read_if_valid() reports it as missing and redo rewrites it.
Pager::Pager(const std::string& path) : path_(path), file_(File::open(path, /*create=*/false)) {}

void Pager::create(const std::string& path, std::vector<Page> pages) {
    const std::string tmp = path + ".tmp";
    {
        File file = File::open(tmp, /*create=*/true);
        file.truncate(0);  // a leftover .tmp from an earlier crashed create
        for (std::size_t i = 0; i < pages.size(); ++i) {
            pages[i].seal();
            file.write_at(offset_of(static_cast<PageId>(i)), pages[i].bytes.data(), kPageSize);
        }
        file.sync();
    }
    rename_file(tmp, path);
    sync_parent_directory(path);
}

void Pager::read(PageId id, Page& out) const {
    const std::size_t n = file_.read_at(offset_of(id), out.bytes.data(), kPageSize);
    if (n != kPageSize) {
        throw CorruptionError("page " + std::to_string(id) + ": short read (beyond end of file)");
    }
    if (!out.verify()) {
        throw CorruptionError("page " + std::to_string(id) + ": checksum mismatch");
    }
}

bool Pager::read_if_valid(PageId id, Page& out) const {
    const std::size_t n = file_.read_at(offset_of(id), out.bytes.data(), kPageSize);
    return n == kPageSize && out.verify();
}

void Pager::write(PageId id, Page& page) {
    page.seal();
    file_.write_at(offset_of(id), page.bytes.data(), kPageSize);
}

PageId Pager::file_page_count() const { return static_cast<PageId>(file_.size() / kPageSize); }

}  // namespace minidb
```

Why `file.truncate(0)` in `create`: a `.tmp` left by an earlier crashed creation may be longer
than the new one; without the truncate, stale pages would survive past the new content.

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug -R 'Crc32|Pager'
```

### Tests

Add to `tests/storage_test.cpp`:

```cpp
TEST(Pager, CreateWriteReadRoundTrip) {
    TempDir dir;
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    Pager pager(path);
    EXPECT_EQ(pager.file_page_count(), 2u);

    Page out = page_with_byte('x');
    pager.write(5, out);  // beyond the end: the file grows, pages 2..4 are holes
    pager.sync();
    Page in;
    pager.read(5, in);
    EXPECT_EQ(std::memcmp(in.bytes.data(), out.bytes.data(), kPageSize), 0);
    EXPECT_EQ(pager.file_page_count(), 6u);
    EXPECT_THROW(pager.read(3, in), CorruptionError);  // a hole is all zeros: checksum fails
    EXPECT_FALSE(pager.read_if_valid(3, in));
    EXPECT_FALSE(pager.read_if_valid(100, in));  // beyond EOF
}

TEST(Pager, DetectsCorruptedPage) {
    TempDir dir;
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    {
        File f = File::open(path, false);
        const char junk = 0x5A;
        f.write_at(kPageSize + 100, &junk, 1);  // flip a byte inside page 1
    }
    Pager pager(path);
    Page page;
    EXPECT_THROW(pager.read(1, page), CorruptionError);
    EXPECT_NO_THROW(pager.read(0, page));
}
```

`CreateWriteReadRoundTrip` also documents a Linux fact worth knowing: writing page 5 of a
two-page file creates a *hole* (pages 2 to 4) that reads back as zeros and takes no disk
space. Zeros fail the checksum, so a hole can never be mistaken for a valid page.

### Commit

```bash
git add -A
git commit -m "Add Pager: checksummed page I/O and atomic database creation

- page N at offset N*4096; seal on write, verify on read (CorruptionError)
- read_if_valid() for recovery, where a torn or missing page is expected
- create(): write .tmp, fdatasync, rename, fsync the directory
- tests: round trip, holes, corrupted byte detection"
git push
```

### What to say in an interview

"Creating the database file is atomic: write a temporary file, `fdatasync` it, `rename` it into
place, `fsync` the directory. Rename is atomic on POSIX filesystems, and the directory sync
makes the rename itself durable, which people forget: without it a power cut can lose the
directory entry of a file whose data was synced. Every page read is verified against its CRC,
so corruption becomes an exception, never wrong data."

---

## Step 3. `PageCache`: pinning, LRU, dirty pages, no-steal

**Time:** ~7 hours   **Week:** 7
**Read first:** 07_Databases_Internals/02_storage_engines_pages_and_heap_files.md (buffer
pool) and `04_wal_and_crash_recovery.md` (steal/no-steal, force/no-force).
02_Modern_CPP: `05_smart_pointers_and_ownership.md` (RAII handles), `06_stl_containers_deep_dive.md`
(`std::list` iterator stability, `unordered_map`). 04_Concurrency is not needed: the cache is
single-threaded by design; the `Db` mutex protects it.

### Goal

A fixed pool of page frames in front of the Pager, with the four properties the rest of the
engine relies on: a pinned page never moves; a dirty page is written back before its frame is
reused; the write-ahead rule is enforced at that moment; pages changed by an uncommitted
transaction never reach the disk.

In plain words: the cache is a desk with room for N open books. You may only put a book back
on the shelf if nobody is reading it (pin count zero), you put back the one used least
recently first (LRU), and if you wrote in it you must first copy your notes into the safe (the
log, the WAL rule). Books you are in the middle of editing stay on the desk until the change
is signed off (no-steal).

### Design

* **Frames and a page table.** A `std::vector<Frame>` sized once, so references into it stay
  valid, plus `unordered_map<PageId, frame index>`.
* **`PageRef`, an RAII pin.** `fetch()` pins; the `PageRef` destructor unpins. Move-only. An
  exception anywhere in the tree code cannot leak a pin, so it cannot slowly make every frame
  unevictable. tcpkv's `FileDescriptor` idea, applied to a buffer-pool pin.
* **`read()` versus `write()` on the handle.** `write()` must be called *before* modifying the
  page: it saves the before-image (the first time in this transaction), marks the frame dirty
  and joins it to the transaction. Making it the only way to get a mutable reference means you
  cannot forget.
* **LRU over unpinned frames** with a `std::list` and stored iterators: O(1) touch, O(1) evict.
  Real engines use CLOCK (a cheaper approximation) or LRU-K / 2Q (resistant to one big scan
  flushing the hot set); plain LRU is the clear version.
* **The WAL-rule hook.** The cache knows nothing about the log. It receives a
  `std::function<void(Lsn)>` that must make the log durable up to an LSN, and calls it before
  every write-back. Dependency inversion in one line, and it lets this step's tests run with a
  no-op hook.
* **No-steal and abort.** Frames in the open transaction are skipped by eviction.
  `abort_txn()` restores before-images and drops pages created in the transaction (the meta
  page's before-image undoes the allocation). In-memory undo is all a no-steal engine needs.

### Files

`include/minidb/page_cache.hpp`, `src/page_cache.cpp`.

### Code

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minidb/page.hpp"
#include "minidb/pager.hpp"

namespace minidb {

// Fixed-size buffer pool in front of the Pager.
//
//  * Pinning: fetch()/create() return a PageRef that pins the frame; a pinned frame is never
//    evicted. The PageRef unpins in its destructor (RAII), so an exception cannot leak a pin.
//  * Replacement: least recently unpinned frame first (LRU over unpinned frames).
//  * WAL rule: before a dirty page is written to the data file, wal_flush(page.lsn()) is
//    called so the log record carrying that image is durable first.
//  * No-steal: a page modified by the current transaction is never written to the data file
//    before commit_txn(). On abort_txn() its before-image is restored from memory. This is
//    what makes a redo-only WAL sufficient: the data file never contains uncommitted bytes.
//
// Not thread-safe; the Db holds one mutex around every operation.
class PageCache {
public:
    using WalFlush = std::function<void(Lsn)>;

    PageCache(Pager& pager, std::size_t capacity, WalFlush wal_flush);
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    class PageRef {
    public:
        PageRef() noexcept = default;
        PageRef(PageRef&& other) noexcept : cache_(other.cache_), frame_(other.frame_) { other.cache_ = nullptr; }
        PageRef& operator=(PageRef&& other) noexcept {
            if (this != &other) {
                release();
                cache_ = other.cache_;
                frame_ = other.frame_;
                other.cache_ = nullptr;
            }
            return *this;
        }
        PageRef(const PageRef&) = delete;
        PageRef& operator=(const PageRef&) = delete;
        ~PageRef() { release(); }

        PageId id() const noexcept { return cache_->frames_[frame_].id; }
        const Page& read() const noexcept { return cache_->frames_[frame_].page; }
        // Joins the page to the current transaction (saving its before-image the first time)
        // and returns it for modification. Always call write() *before* changing bytes.
        Page& write() { return cache_->begin_write(frame_); }

    private:
        friend class PageCache;
        PageRef(PageCache* cache, std::size_t frame) noexcept : cache_(cache), frame_(frame) {}
        void release() noexcept {
            if (cache_ != nullptr) {
                cache_->unpin(frame_);
                cache_ = nullptr;
            }
        }
        PageCache* cache_ = nullptr;
        std::size_t frame_ = 0;
    };

    PageRef fetch(PageId id);   // pins; reads (and verifies) from disk on a miss
    PageRef create(PageId id);  // pins a zeroed frame for a newly allocated page id

    // Transaction support. A transaction is implicit: it starts with the first write() and
    // ends with commit_txn() or abort_txn().
    void for_each_txn_page(const std::function<void(PageId, Page&)>& fn);
    void commit_txn() noexcept;
    void abort_txn() noexcept;
    bool in_txn() const noexcept { return !txn_frames_.empty(); }

    // Writes every dirty, committed page. The caller then calls Pager::sync().
    void flush_all();

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t writebacks = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    std::size_t capacity() const noexcept { return frames_.size(); }

private:
    struct Frame {
        PageId id = 0;
        Page page;
        int pins = 0;
        bool used = false;
        bool dirty = false;
        bool in_txn = false;
        bool created_in_txn = false;
        bool in_lru = false;
        std::list<std::size_t>::iterator lru_pos;
    };
    struct BeforeImage {
        Page page;
        bool was_dirty = false;
    };

    std::size_t obtain_frame();  // a free frame, or an evicted one; throws if none
    void write_back(Frame& frame);
    void pin(std::size_t frame);
    void unpin(std::size_t frame) noexcept;
    Page& begin_write(std::size_t frame);
    void drop_frame(std::size_t frame) noexcept;

    Pager& pager_;
    WalFlush wal_flush_;
    std::vector<Frame> frames_;                      // never resized: references stay valid
    std::unordered_map<PageId, std::size_t> table_;  // page id -> frame index
    std::list<std::size_t> lru_;                     // unpinned frames, least recent at front
    std::vector<std::size_t> free_;                  // frames holding no page
    std::vector<std::size_t> txn_frames_;            // frames written by the current transaction
    std::unordered_map<PageId, BeforeImage> before_images_;
    Stats stats_;
};

}  // namespace minidb
```

```cpp
#include "minidb/page_cache.hpp"

#include <string>
#include <utility>

namespace minidb {

PageCache::PageCache(Pager& pager, std::size_t capacity, WalFlush wal_flush)
    : pager_(pager), wal_flush_(std::move(wal_flush)), frames_(capacity) {
    if (capacity < 16) {
        // A put pins one page per tree level and a split creates up to one new page per level
        // plus a new root; 16 frames covers trees far deeper than 4 KiB pages ever produce.
        throw DbError("page cache needs at least 16 frames");
    }
    free_.reserve(capacity);
    for (std::size_t i = capacity; i > 0; --i) {
        free_.push_back(i - 1);
    }
}

PageCache::PageRef PageCache::fetch(PageId id) {
    const auto it = table_.find(id);
    if (it != table_.end()) {
        ++stats_.hits;
        pin(it->second);
        return PageRef(this, it->second);
    }
    ++stats_.misses;
    const std::size_t index = obtain_frame();
    Frame& frame = frames_[index];
    try {
        pager_.read(id, frame.page);
    } catch (...) {
        free_.push_back(index);  // frame was not registered yet; just give it back
        throw;
    }
    frame.id = id;
    frame.used = true;
    frame.dirty = false;
    table_.emplace(id, index);
    pin(index);
    return PageRef(this, index);
}

PageCache::PageRef PageCache::create(PageId id) {
    if (table_.count(id) != 0) {
        throw DbError("page " + std::to_string(id) + " already cached; allocated twice?");
    }
    const std::size_t index = obtain_frame();
    Frame& frame = frames_[index];
    frame.page = Page{};
    frame.id = id;
    frame.used = true;
    frame.dirty = true;
    frame.in_txn = true;
    frame.created_in_txn = true;  // abort removes it; there is no before-image to restore
    txn_frames_.push_back(index);
    table_.emplace(id, index);
    pin(index);
    return PageRef(this, index);
}

Page& PageCache::begin_write(std::size_t index) {
    Frame& frame = frames_[index];
    if (!frame.in_txn) {
        before_images_.emplace(frame.id, BeforeImage{frame.page, frame.dirty});
        frame.in_txn = true;
        txn_frames_.push_back(index);
    }
    frame.dirty = true;
    return frame.page;
}

void PageCache::for_each_txn_page(const std::function<void(PageId, Page&)>& fn) {
    for (const std::size_t index : txn_frames_) {
        fn(frames_[index].id, frames_[index].page);
    }
}

void PageCache::commit_txn() noexcept {
    for (const std::size_t index : txn_frames_) {
        frames_[index].in_txn = false;
        frames_[index].created_in_txn = false;  // now an ordinary dirty, committed page
    }
    txn_frames_.clear();
    before_images_.clear();
}

void PageCache::abort_txn() noexcept {
    for (const std::size_t index : txn_frames_) {
        Frame& frame = frames_[index];
        if (frame.created_in_txn) {
            drop_frame(index);  // the allocation is undone by the meta page's before-image
            continue;
        }
        const auto it = before_images_.find(frame.id);
        if (it != before_images_.end()) {
            frame.page = it->second.page;
            frame.dirty = it->second.was_dirty;
        }
        frame.in_txn = false;
    }
    txn_frames_.clear();
    before_images_.clear();
}

void PageCache::flush_all() {
    for (Frame& frame : frames_) {
        if (frame.used && frame.dirty && !frame.in_txn) {
            write_back(frame);
        }
    }
}

std::size_t PageCache::obtain_frame() {
    if (!free_.empty()) {
        const std::size_t index = free_.back();
        free_.pop_back();
        return index;
    }
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        Frame& frame = frames_[*it];
        if (frame.in_txn) {
            continue;  // no-steal: uncommitted bytes never reach the data file
        }
        if (frame.dirty) {
            write_back(frame);  // may throw; the frame stays cached and consistent
        }
        const std::size_t index = *it;
        lru_.erase(it);
        frame.in_lru = false;
        table_.erase(frame.id);
        frame.used = false;
        ++stats_.evictions;
        return index;
    }
    throw DbError("page cache exhausted: every frame is pinned or part of the open transaction");
}

void PageCache::write_back(Frame& frame) {
    wal_flush_(frame.page.lsn());  // the WAL rule: log record durable before the page
    pager_.write(frame.id, frame.page);
    frame.dirty = false;
    ++stats_.writebacks;
}

void PageCache::pin(std::size_t index) {
    Frame& frame = frames_[index];
    if (frame.in_lru) {
        lru_.erase(frame.lru_pos);
        frame.in_lru = false;
    }
    ++frame.pins;
}

void PageCache::unpin(std::size_t index) noexcept {
    Frame& frame = frames_[index];
    if (--frame.pins == 0 && frame.used) {
        frame.lru_pos = lru_.insert(lru_.end(), index);  // most recently used at the back
        frame.in_lru = true;
    }
}

void PageCache::drop_frame(std::size_t index) noexcept {
    Frame& frame = frames_[index];
    if (frame.in_lru) {
        lru_.erase(frame.lru_pos);
        frame.in_lru = false;
    }
    table_.erase(frame.id);
    frame.used = false;
    frame.dirty = false;
    frame.in_txn = false;
    frame.created_in_txn = false;
    if (frame.pins == 0) {
        free_.push_back(index);
    }
    // A dropped frame that is still pinned would be a bug in the caller (abort while a
    // PageRef is alive); it then leaks one frame rather than corrupting another page.
}

}  // namespace minidb
```

Two details to be able to explain:

* In `fetch()`, if the disk read throws, the frame goes back to the free list *before* it was
  entered in the page table, so a failed read leaves the cache exactly as it was.
* In `obtain_frame()`, a dirty victim is written back *before* it leaves the table. If the
  write throws, the page is still cached and still dirty: nothing is lost.

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug -R PageCache
cmake --build --preset asan  && ctest --preset asan  -R PageCache
```

### Tests

```cpp
TEST(PageCache, EvictsLeastRecentlyUsedAndWritesBackDirtyPages) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        pages[i] = page_with_byte(static_cast<char>('a' + i));
    }
    Pager::create(path, pages);
    Pager pager(path);
    std::vector<Lsn> flushed;
    PageCache cache(pager, 16, [&](Lsn lsn) { flushed.push_back(lsn); });

    {
        PageCache::PageRef ref = cache.fetch(3);
        ref.write().payload()[0] = 'Z';  // dirty, inside a "transaction"
    }
    cache.commit_txn();  // now an ordinary dirty page: evictable
    for (PageId id = 4; id < 20; ++id) {
        cache.fetch(id);  // 16 more pages through a 16-frame cache: page 3 must be evicted
    }
    EXPECT_GE(cache.stats().evictions, 1u);
    EXPECT_EQ(flushed.size(), 1u);  // the WAL rule hook ran before the write-back
    Page on_disk;
    pager.read(3, on_disk);
    EXPECT_EQ(on_disk.payload()[0], 'Z');
}

TEST(PageCache, NoStealAndAbortRestoresBeforeImage) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        pages[i] = page_with_byte('a');
    }
    Pager::create(path, pages);
    Pager pager(path);
    PageCache cache(pager, 16, [](Lsn) {});

    {
        PageCache::PageRef ref = cache.fetch(2);
        ref.write().payload()[0] = 'U';  // uncommitted
    }
    for (PageId id = 3; id < 20; ++id) {
        cache.fetch(id);  // pressure: 17 other pages through the 15 remaining frames
    }
    Page on_disk;
    pager.read(2, on_disk);
    EXPECT_EQ(on_disk.payload()[0], 'a');  // no-steal: the uncommitted byte never hit the file

    cache.abort_txn();
    EXPECT_EQ(cache.fetch(2).read().payload()[0], 'a');  // before-image restored
}

TEST(PageCache, ThrowsWhenEveryFrameIsPinned) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20, page_with_byte('a'));
    Pager::create(path, pages);
    Pager pager(path);
    PageCache cache(pager, 16, [](Lsn) {});
    std::vector<PageCache::PageRef> pins;
    for (PageId id = 0; id < 16; ++id) {
        pins.push_back(cache.fetch(id));
    }
    EXPECT_THROW(cache.fetch(16), DbError);
    pins.pop_back();  // unpin one
    EXPECT_NO_THROW(cache.fetch(16));
}
```

### Commit

```bash
git add -A
git commit -m "Add PageCache: pinned frames, LRU replacement, WAL-rule hook, no-steal

- PageRef: move-only RAII pin; write() saves the before-image and joins the transaction
- LRU over unpinned frames (list + stored iterators); dirty victims written back first
- write-back calls the WAL hook with the page LSN before touching the data file
- frames in the open transaction are never evicted; abort_txn() restores before-images"
git push
```

### What to say in an interview

"The buffer pool pins pages through a move-only RAII handle, so an exception cannot leak a pin.
Before a dirty page is written back, the cache calls a hook that makes the log durable up to
that page's LSN: the write-ahead rule, enforced in exactly one place. Pages modified by the open
transaction are never evicted, which is the no-steal policy; the data file therefore never
holds uncommitted bytes, so recovery needs redo but never undo. I did not use `mmap`, because
the engine has to control *when* a page reaches the disk to keep the WAL rule, and with `mmap`
the kernel writes dirty pages back whenever it likes. The CIDR 2022 paper 'Are You Sure You
Want to Use MMAP in Your Database Management System?' makes that argument in detail."

---

## Step 4. `BPlusTree` I: node format, search, insert, splits, invariant checker

**Time:** ~8 hours   **Week:** 7
**Read first:** 07_Databases_Internals/03_btree_indexes_in_depth.md, all of it.
03_Data_Structures_Algorithms: binary search, and exactly what `std::lower_bound` and
`std::upper_bound` return. 02_Modern_CPP: `07_stl_algorithms_lambdas_iterators.md`.

### Goal

A B+tree stored in pages: get, insert, overwrite, leaf splits, internal splits, root splits,
every leaf at the same depth, and a checker that proves it after any sequence of operations.

In plain words: a library card catalogue. The top drawer says "A to F: drawer 2, G to M:
drawer 3". Drawer 3 says "G to H: card box 17, ...". The card box holds the cards in order.
When a card box overflows you split it into two and add one line to the drawer above it. When
a drawer overflows you split it and add a line to the one above that. When the top drawer
overflows you build a new top drawer above it: the only way the catalogue gets taller, which
is why every card box is always the same number of drawers from the top.

### Design

* **Decode, modify, encode.** Each operation decodes a page into a small struct of vectors,
  changes it, and encodes it back. Real engines edit slotted pages in place; this is slower but
  far easier to get right, and the on-disk format is identical.
* **Separators.** In an internal node, `key_i` is the smallest key that may appear under
  `child_i`. The child for a search key is the number of separators `<=` the key, which is
  `std::upper_bound`.
* **Split by bytes, not by count.** Cells range from 5 to 1,012 bytes. Splitting at the middle
  *cell* can leave one half overfull; splitting at the middle *byte* cannot. Because a cell is
  at most a quarter of a page, the left half is at most about 5/8 of a page and the right at
  most about 7/8. Redo that arithmetic on a whiteboard: total after the insert is at most one
  page plus one cell; the left half stops at or before half of that; the right half is the
  rest, at most half plus one cell.
* **A leaf split copies the separator up** (the first key of the right leaf stays in the leaf);
  **an internal split moves it up** (the middle separator leaves the node). That difference is
  the B+tree in one sentence.
* **Leaves are chained** through `next_leaf` for range scans (step 5).
* **Allocation** is "increment `page_count` in the meta page", done through the cache, so it is
  part of the transaction: an aborted insert also un-allocates.
* **The checker** (`check()`) walks the whole tree and verifies every invariant in
  `00_spec.md` section 4.4. Write it in this step, not later: it is the test oracle for every
  split you are about to debug.
* **Fan-out, for interviews.** With 16-byte keys an internal entry is 22 bytes, so an internal
  page holds about 185 children. With 100-byte values a leaf cell is 120 bytes, so a leaf holds
  about 34. One million keys need roughly 30,000 leaves (more, since split leaves are partly
  empty) and two internal levels above them: height 3. Three page reads per lookup, and the
  top two levels stay in the cache.

### Files

`include/minidb/btree.hpp`; `src/btree.cpp` (everything except `erase` and `scan`, which are
step 5: leave them declared and unimplemented until then, and do not call them);
`tests/btree_test.cpp` (the fixture and one test, below).

### Code

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minidb/page_cache.hpp"

namespace minidb {

// B+tree over pages from the PageCache. Keys and values are byte strings; keys are compared
// with std::string_view::compare (bytewise, like memcmp).
//
//   internal page: child0 | key1 child1 | key2 child2 | ...   key_i = smallest key in child_i
//   leaf page:     next_leaf | key1 value1 | key2 value2 | ...  sorted, unique keys
//
// Every leaf is at the same depth. Leaves are chained left to right through next_leaf, so a
// range scan descends once and then walks sideways.
//
// Simplification (documented, deliberate): erase() never merges or rebalances. A leaf may
// become empty; it stays in the chain and is skipped by scans. Space is reclaimed only by
// rebuilding the file. Many production B-trees also delete lazily for the same reason:
// merges are complex and rarely pay for themselves.
//
// Mutating calls must run inside a PageCache transaction (the Db does this). Not thread-safe.
class BPlusTree {
public:
    explicit BPlusTree(PageCache& cache) : cache_(cache) {}

    // Pages for a brand-new database: page 0 = meta (root = 1, page_count = 2), page 1 = empty leaf.
    static std::vector<Page> initial_pages();

    std::optional<std::string> get(std::string_view key);
    void put(std::string_view key, std::string_view value);
    bool erase(std::string_view key);

    // Keys in [start, end), ascending, at most `limit`. An empty `end` means "no upper bound".
    std::vector<std::pair<std::string, std::string>> scan(std::string_view start, std::string_view end,
                                                          std::size_t limit);

    // Walks the whole tree and checks every structural invariant (key order, separator
    // bounds, equal leaf depth, leaf chain matches in-order leaves). Throws CorruptionError on
    // the first violation; returns the number of keys. Used by tests and after recovery.
    std::size_t check();

private:
    struct Split {
        std::string separator;  // smallest key of the new right sibling
        PageId right = kNoPage;
    };

    std::optional<Split> insert(PageId id, std::string_view key, std::string_view value, int depth);
    PageId find_leaf(std::string_view key);
    PageId allocate_page();
    PageId root();
    void set_root(PageId root);

    PageCache& cache_;
};

}  // namespace minidb
```

`src/btree.cpp`, part 1: node codecs, search, insert and splits.

```cpp
#include "minidb/btree.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace minidb {

namespace {

constexpr int kMaxDepth = 32;  // a 4 KiB-page tree of any realistic size is under 6 levels

// Decoded node forms. We decode a page, change the vectors, and encode it back. Real engines
// edit slotted pages in place; decode/encode costs some CPU but is far easier to get right,
// and the on-disk format is identical either way.
struct LeafNode {
    PageId next = kNoPage;
    std::vector<std::pair<std::string, std::string>> cells;  // sorted by key, unique
};

struct InternalNode {
    std::vector<std::string> keys;  // separators, strictly increasing
    std::vector<PageId> children;   // keys.size() + 1 entries
};

// Leaf payload:     u16 count | u32 next | count x (u16 klen | u16 vlen | key | value)
// Internal payload: u16 count | u32 child0 | count x (u16 klen | key | u32 child)
constexpr std::size_t kNodeHeader = 2 + 4;

std::size_t leaf_cell_size(const std::pair<std::string, std::string>& cell) noexcept {
    return 4 + cell.first.size() + cell.second.size();
}

std::size_t internal_entry_size(const std::string& key) noexcept { return 2 + key.size() + 4; }

std::size_t leaf_size(const LeafNode& node) noexcept {
    std::size_t size = kNodeHeader;
    for (const auto& cell : node.cells) {
        size += leaf_cell_size(cell);
    }
    return size;
}

std::size_t internal_size(const InternalNode& node) noexcept {
    std::size_t size = kNodeHeader;
    for (const auto& key : node.keys) {
        size += internal_entry_size(key);
    }
    return size;
}

// Bounds-checked decoding: a corrupt length can never read past the page.
class Reader {
public:
    explicit Reader(const char* p) noexcept : p_(p), left_(kPagePayloadSize) {}
    template <class T>
    T get() {
        need(sizeof(T));
        const T value = read_as<T>(p_);
        p_ += sizeof(T);
        left_ -= sizeof(T);
        return value;
    }
    std::string bytes(std::size_t n) {
        need(n);
        std::string out(p_, n);
        p_ += n;
        left_ -= n;
        return out;
    }

private:
    void need(std::size_t n) const {
        if (n > left_) {
            throw CorruptionError("b+tree node overruns its page");
        }
    }
    const char* p_;
    std::size_t left_;
};

class Writer {
public:
    explicit Writer(char* p) noexcept : p_(p) {}
    template <class T>
    void put(T value) noexcept {
        write_as(p_, value);
        p_ += sizeof(T);
    }
    void bytes(const std::string& s) noexcept {
        std::memcpy(p_, s.data(), s.size());
        p_ += s.size();
    }

private:
    char* p_;
};

LeafNode decode_leaf(const Page& page) {
    if (page.type() != PageType::Leaf) {
        throw CorruptionError("expected a leaf page");
    }
    Reader r(page.payload());
    LeafNode node;
    const auto count = r.get<std::uint16_t>();
    node.next = r.get<PageId>();
    node.cells.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto klen = r.get<std::uint16_t>();
        const auto vlen = r.get<std::uint16_t>();
        std::string key = r.bytes(klen);
        std::string value = r.bytes(vlen);
        node.cells.emplace_back(std::move(key), std::move(value));
    }
    return node;
}

// Precondition: leaf_size(node) <= kPagePayloadSize (callers check before encoding).
void encode_leaf(const LeafNode& node, Page& page) {
    page.set_type(PageType::Leaf);
    page.clear_payload();
    Writer w(page.payload());
    w.put(static_cast<std::uint16_t>(node.cells.size()));
    w.put(node.next);
    for (const auto& [key, value] : node.cells) {
        w.put(static_cast<std::uint16_t>(key.size()));
        w.put(static_cast<std::uint16_t>(value.size()));
        w.bytes(key);
        w.bytes(value);
    }
}

InternalNode decode_internal(const Page& page) {
    if (page.type() != PageType::Internal) {
        throw CorruptionError("expected an internal page");
    }
    Reader r(page.payload());
    InternalNode node;
    const auto count = r.get<std::uint16_t>();
    node.keys.reserve(count);
    node.children.reserve(count + 1u);
    node.children.push_back(r.get<PageId>());
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto klen = r.get<std::uint16_t>();
        node.keys.push_back(r.bytes(klen));
        node.children.push_back(r.get<PageId>());
    }
    return node;
}

void encode_internal(const InternalNode& node, Page& page) {
    page.set_type(PageType::Internal);
    page.clear_payload();
    Writer w(page.payload());
    w.put(static_cast<std::uint16_t>(node.keys.size()));
    w.put(node.children.front());
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
        w.put(static_cast<std::uint16_t>(node.keys[i].size()));
        w.bytes(node.keys[i]);
        w.put(node.children[i + 1]);
    }
}

// Index of the child whose subtree may contain `key`: the number of separators <= key.
std::size_t child_index(const InternalNode& node, std::string_view key) {
    const auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key,
                                     [](std::string_view k, const std::string& sep) { return k.compare(sep) < 0; });
    return static_cast<std::size_t>(it - node.keys.begin());
}

auto leaf_lower_bound(LeafNode& node, std::string_view key) {
    return std::lower_bound(node.cells.begin(), node.cells.end(), key,
                            [](const auto& cell, std::string_view k) { return std::string_view(cell.first).compare(k) < 0; });
}

}  // namespace

std::vector<Page> BPlusTree::initial_pages() {
    std::vector<Page> pages(2);
    Meta meta;
    meta.root = 1;
    meta.page_count = 2;
    meta.encode(pages[0]);
    encode_leaf(LeafNode{}, pages[1]);
    return pages;
}

PageId BPlusTree::root() {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    return Meta::decode(meta_ref.read()).root;
}

void BPlusTree::set_root(PageId new_root) {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    Meta meta = Meta::decode(meta_ref.read());
    meta.root = new_root;
    meta.encode(meta_ref.write());
}

PageId BPlusTree::allocate_page() {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    Meta meta = Meta::decode(meta_ref.read());
    if (meta.page_count == UINT32_MAX) {
        throw DbError("database full: page id space exhausted");
    }
    const PageId id = meta.page_count++;
    meta.encode(meta_ref.write());  // logged with the transaction; undone by abort
    return id;
}

PageId BPlusTree::find_leaf(std::string_view key) {
    PageId id = root();
    for (int depth = 0; depth <= kMaxDepth; ++depth) {
        PageCache::PageRef ref = cache_.fetch(id);
        const Page& page = ref.read();
        if (page.type() == PageType::Leaf) {
            return id;
        }
        const InternalNode node = decode_internal(page);
        id = node.children[child_index(node, key)];
    }
    throw CorruptionError("b+tree deeper than possible (cycle?)");
}

std::optional<std::string> BPlusTree::get(std::string_view key) {
    PageCache::PageRef ref = cache_.fetch(find_leaf(key));
    LeafNode leaf = decode_leaf(ref.read());
    const auto it = leaf_lower_bound(leaf, key);
    if (it == leaf.cells.end() || it->first != key) {
        return std::nullopt;
    }
    return std::move(it->second);
}

void BPlusTree::put(std::string_view key, std::string_view value) {
    const PageId old_root = root();
    std::optional<Split> split = insert(old_root, key, value, 0);
    if (!split) {
        return;
    }
    // The root split: the tree grows by one level, at the top. This is the only way a
    // B+tree gets taller, which is why all leaves stay at the same depth.
    InternalNode node;
    node.keys.push_back(std::move(split->separator));
    node.children = {old_root, split->right};
    const PageId new_root = allocate_page();
    PageCache::PageRef ref = cache_.create(new_root);
    encode_internal(node, ref.write());
    set_root(new_root);
}

std::optional<BPlusTree::Split> BPlusTree::insert(PageId id, std::string_view key, std::string_view value,
                                                  int depth) {
    if (depth > kMaxDepth) {
        throw CorruptionError("b+tree deeper than possible (cycle?)");
    }
    PageCache::PageRef ref = cache_.fetch(id);

    if (ref.read().type() == PageType::Leaf) {
        LeafNode leaf = decode_leaf(ref.read());
        const auto it = leaf_lower_bound(leaf, key);
        if (it != leaf.cells.end() && it->first == key) {
            it->second.assign(value.data(), value.size());
        } else {
            leaf.cells.emplace(it, std::string(key), std::string(value));
        }
        if (leaf_size(leaf) <= kPagePayloadSize) {
            encode_leaf(leaf, ref.write());
            return std::nullopt;
        }

        // Leaf split, balanced by bytes rather than by count, so that a few large values
        // cannot leave one half overfull. Because one cell is at most a quarter of a page
        // (kMaxKeySize, kMaxValueSize), both halves are guaranteed to fit.
        std::size_t total = 0;
        for (const auto& cell : leaf.cells) {
            total += leaf_cell_size(cell);
        }
        std::size_t mid = 0;
        std::size_t left_bytes = 0;
        while (mid + 1 < leaf.cells.size() && left_bytes + leaf_cell_size(leaf.cells[mid]) <= total / 2) {
            left_bytes += leaf_cell_size(leaf.cells[mid]);
            ++mid;
        }
        mid = std::max<std::size_t>(mid, 1);

        LeafNode right;
        right.cells.assign(std::make_move_iterator(leaf.cells.begin() + static_cast<std::ptrdiff_t>(mid)),
                           std::make_move_iterator(leaf.cells.end()));
        leaf.cells.resize(mid);

        const PageId right_id = allocate_page();
        right.next = leaf.next;  // splice the new leaf into the sibling chain
        leaf.next = right_id;
        PageCache::PageRef right_ref = cache_.create(right_id);
        encode_leaf(right, right_ref.write());
        encode_leaf(leaf, ref.write());
        return Split{right.cells.front().first, right_id};
    }

    InternalNode node = decode_internal(ref.read());
    const std::size_t index = child_index(node, key);
    std::optional<Split> child_split = insert(node.children[index], key, value, depth + 1);
    if (!child_split) {
        return std::nullopt;
    }
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(index), std::move(child_split->separator));
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(index) + 1, child_split->right);
    if (internal_size(node) <= kPagePayloadSize) {
        encode_internal(node, ref.write());
        return std::nullopt;
    }

    // Internal split: keys[mid] moves *up* to the parent (unlike a leaf split, which copies
    // the first right key up and keeps it in the leaf).
    std::size_t total = 0;
    for (const auto& k : node.keys) {
        total += internal_entry_size(k);
    }
    std::size_t mid = 0;
    std::size_t left_bytes = 0;
    while (mid + 2 < node.keys.size() && left_bytes + internal_entry_size(node.keys[mid]) <= total / 2) {
        left_bytes += internal_entry_size(node.keys[mid]);
        ++mid;
    }
    mid = std::max<std::size_t>(mid, 1);

    InternalNode right;
    right.keys.assign(std::make_move_iterator(node.keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                      std::make_move_iterator(node.keys.end()));
    right.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(mid) + 1, node.children.end());
    std::string promoted = std::move(node.keys[mid]);
    node.keys.resize(mid);
    node.children.resize(mid + 1);

    const PageId right_id = allocate_page();
    PageCache::PageRef right_ref = cache_.create(right_id);
    encode_internal(right, right_ref.write());
    encode_internal(node, ref.write());
    return Split{std::move(promoted), right_id};
}

```

`src/btree.cpp`, part 2 (end of file): the invariant checker.

```cpp
std::size_t BPlusTree::check() {
    struct Checker {
        explicit Checker(PageCache& c) : cache(c) {}
        PageCache& cache;
        std::uint32_t page_count = 0;
        int leaf_depth = -1;
        std::size_t keys = 0;
        std::vector<PageId> leaves;  // in key order

        void visit(PageId id, const std::string* lo, const std::string* hi, int depth) {
            if (depth > kMaxDepth || id == kNoPage || id >= page_count) {
                throw CorruptionError("check: bad child pointer or depth");
            }
            PageCache::PageRef ref = cache.fetch(id);
            if (ref.read().type() == PageType::Leaf) {
                const LeafNode leaf = decode_leaf(ref.read());
                if (leaf_depth == -1) {
                    leaf_depth = depth;
                } else if (leaf_depth != depth) {
                    throw CorruptionError("check: leaves at different depths");
                }
                for (std::size_t i = 0; i < leaf.cells.size(); ++i) {
                    const std::string& key = leaf.cells[i].first;
                    if (i > 0 && !(leaf.cells[i - 1].first < key)) {
                        throw CorruptionError("check: leaf keys not strictly increasing");
                    }
                    if ((lo != nullptr && key < *lo) || (hi != nullptr && !(key < *hi))) {
                        throw CorruptionError("check: leaf key outside its separator bounds");
                    }
                }
                keys += leaf.cells.size();
                leaves.push_back(id);
                return;
            }
            const InternalNode node = decode_internal(ref.read());
            ref = PageCache::PageRef();  // unpin before descending: pins stay O(1), not O(depth)
            if (node.children.size() != node.keys.size() + 1) {
                throw CorruptionError("check: internal node child count");
            }
            for (std::size_t i = 0; i < node.keys.size(); ++i) {
                if ((i > 0 && !(node.keys[i - 1] < node.keys[i])) || (lo != nullptr && node.keys[i] < *lo) ||
                    (hi != nullptr && !(node.keys[i] < *hi))) {
                    throw CorruptionError("check: separators out of order or out of bounds");
                }
            }
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                const std::string* child_lo = i == 0 ? lo : &node.keys[i - 1];
                const std::string* child_hi = i == node.keys.size() ? hi : &node.keys[i];
                visit(node.children[i], child_lo, child_hi, depth + 1);
            }
        }
    };

    Checker checker(cache_);
    {
        PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
        checker.page_count = Meta::decode(meta_ref.read()).page_count;
    }
    checker.visit(root(), nullptr, nullptr, 0);

    // The sibling chain must visit exactly the leaves the tree walk found, in the same order.
    for (std::size_t i = 0; i < checker.leaves.size(); ++i) {
        PageCache::PageRef ref = cache_.fetch(checker.leaves[i]);
        const PageId expected_next = i + 1 < checker.leaves.size() ? checker.leaves[i + 1] : kNoPage;
        if (decode_leaf(ref.read()).next != expected_next) {
            throw CorruptionError("check: leaf chain does not match tree order");
        }
    }
    return checker.keys;
}

}  // namespace minidb
```

Points that come up in review:

* `insert` keeps the parent pinned while it recurses into the child, so pins grow with the
  tree height (3 or 4), far below the 16-frame minimum. `check` unpins a node before visiting
  its children, so it holds O(1) pins however large the tree.
* `find_leaf` and `insert` stop after 32 levels and throw `CorruptionError`: a corrupted child
  pointer that forms a cycle must not hang the process.
* `Reader` bounds-checks every length read from a page, so a corrupt `count` or `klen` can
  never read past the 4 KiB buffer. This is exactly the class of bug ASan finds in parsers.

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug -R TreeFixture
cmake --build --preset asan  && ctest --preset asan  -R TreeFixture
```

### Tests

`tests/btree_test.cpp`: the tree on a Pager and a PageCache with a no-op WAL hook, committing
each operation in the cache directly, so tree bugs show up here and not in recovery. In step 4,
leave out the `erase` helper of the fixture.

```cpp
// BPlusTree on a Pager + PageCache with no log: each operation is committed in the cache
// straight away. Isolates the tree from the WAL so tree bugs show up here, not in recovery.

#include <cstdio>
#include <map>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "minidb/btree.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%08d", i);
    return buf;
}

std::string create_database(const TempDir& dir) {
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    return path;
}

class TreeFixture : public ::testing::Test {
protected:
    TreeFixture()
        : path_(create_database(dir_)),
          pager_(path_),
          cache_(pager_, 32, [](Lsn) {}),  // no WAL in this test: nothing to make durable
          tree_(cache_) {}

    void put(const std::string& k, const std::string& v) {
        tree_.put(k, v);
        cache_.commit_txn();  // what Db::commit does after logging
    }
    bool erase(const std::string& k) {
        const bool removed = tree_.erase(k);
        cache_.commit_txn();
        return removed;
    }

    TempDir dir_;
    std::string path_;
    Pager pager_;
    PageCache cache_;
    BPlusTree tree_;
};

}  // namespace

```

```cpp
TEST_F(TreeFixture, MaximumSizeCellsAlwaysSplitIntoFittingHalves) {
    // Worst case for the split rule: every cell at the maximum size, inserted in reverse.
    for (int i = 300; i > 0; --i) {
        std::string key = key_of(i);
        key.resize(kMaxKeySize, 'k');
        put(key, std::string(kMaxValueSize, 'v'));
    }
    EXPECT_EQ(tree_.check(), 300u);
}
```

This is the adversarial case for the split rule: every cell at the maximum size, inserted in
descending order so every insert goes to the leftmost leaf. If the byte-balanced split or the
size limits were wrong, `encode_leaf` would be asked to write more than a page, which the
checker or ASan would catch.

### Commit

```bash
git add -A
git commit -m "Add BPlusTree: node codecs, search, insert with leaf/internal/root splits, checker

- leaf and internal page formats; bounds-checked decoding (CorruptionError on overrun)
- byte-balanced splits; leaf split copies the separator up, internal split moves it up
- root split is the only way the tree grows, so all leaves stay at one depth
- page allocation through the meta page inside the transaction
- check(): key order, separator bounds, equal leaf depth, leaf chain = in-order leaves"
git push
```

### What to say in an interview

"It is a B+tree: values only in leaves, leaves chained for range scans, internal nodes holding
separators. Splits are balanced by bytes rather than by count, because cells vary in size, and
the key and value limits guarantee that a cell is at most a quarter of a page, so both halves
of a split always fit. A leaf split copies the first key of the new leaf up; an internal split
moves its middle key up. The tree only grows by splitting the root, which is why every leaf is
at the same depth. With 4 KiB pages and 16-byte keys the fan-out is about 185, so a million
keys is three levels. And there is a `check()` that verifies every invariant; every test calls
it."

---

## Step 5. `BPlusTree` II: erase, range scan, model-based test

**Time:** ~5 hours   **Week:** 8
**Read first:** 07_Databases_Internals/03_btree_indexes_in_depth.md (deletion, merges, range
scans). 03_Data_Structures_Algorithms: `std::map` as an ordered reference model.

### Goal

Delete keys, scan ranges through the leaf chain, and prove the whole tree against `std::map`
under tens of thousands of random operations.

### Design

* **Erase without merge.** Remove the cell from its leaf; never merge or redistribute. Leaves
  may become empty; they stay in the chain and scans skip them. Separators in internal nodes
  may then name keys that no longer exist, which is harmless: they are still valid bounds.
  Merging is where most B-tree bugs live, and many production engines also delete lazily and
  reclaim space by rebuilding. Say it as a decision, with the cost: the file never shrinks.
* **Range scan.** Descend once to the leaf that would contain `start`, then follow
  `next_leaf`, stopping at `end` or `limit`. One descent plus sequential leaf reads, which is
  why B+trees (not plain B-trees) are the standard index structure.
* **Model-based testing.** Run the same random operations against the tree and against a
  `std::map`, compare results as you go, compare full contents at the end, and call `check()`
  periodically. It finds the bugs you did not think to write a test for.

### Files

The rest of `src/btree.cpp` (below); the remaining tests in `tests/btree_test.cpp`, and the
fixture's `erase` helper.

### Code

`src/btree.cpp`, the two functions between `insert` and `check`:

```cpp
bool BPlusTree::erase(std::string_view key) {
    PageCache::PageRef ref = cache_.fetch(find_leaf(key));
    LeafNode leaf = decode_leaf(ref.read());
    const auto it = leaf_lower_bound(leaf, key);
    if (it == leaf.cells.end() || it->first != key) {
        return false;
    }
    leaf.cells.erase(it);
    encode_leaf(leaf, ref.write());  // no merge: see the class comment
    return true;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::scan(std::string_view start, std::string_view end,
                                                                 std::size_t limit) {
    std::vector<std::pair<std::string, std::string>> out;
    if (limit == 0) {
        return out;
    }
    std::uint32_t page_count = 0;
    {
        PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
        page_count = Meta::decode(meta_ref.read()).page_count;
    }
    PageId id = find_leaf(start);
    for (std::uint32_t hops = 0; id != kNoPage; ++hops) {
        if (hops > page_count) {
            throw CorruptionError("leaf chain longer than the file (cycle?)");
        }
        PageCache::PageRef ref = cache_.fetch(id);
        LeafNode leaf = decode_leaf(ref.read());
        for (auto& [key, value] : leaf.cells) {
            if (std::string_view(key).compare(start) < 0) {
                continue;
            }
            if (!end.empty() && std::string_view(key).compare(end) >= 0) {
                return out;
            }
            out.emplace_back(std::move(key), std::move(value));
            if (out.size() >= limit) {
                return out;
            }
        }
        id = leaf.next;
    }
    return out;
}

```

`scan` refuses to follow more leaf links than there are pages in the file: a corrupted
`next_leaf` that points backwards would otherwise loop forever.

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug -R TreeFixture
cmake --build --preset asan  && ctest --preset asan  -R TreeFixture
```

### Tests

The remaining three tests of `tests/btree_test.cpp`:

```cpp
TEST_F(TreeFixture, EmptyTree) {
    EXPECT_FALSE(tree_.get("x").has_value());
    EXPECT_TRUE(tree_.scan("", "", 10).empty());
    EXPECT_EQ(tree_.check(), 0u);
}

TEST_F(TreeFixture, SequentialInsertsSplitLeavesAndInternals) {
    for (int i = 0; i < 5000; ++i) {
        put(key_of(i), std::string(400, 'v'));  // ~9 cells per leaf: hundreds of leaves
    }
    EXPECT_EQ(tree_.check(), 5000u);
    EXPECT_EQ(tree_.get(key_of(4321)).value_or("").size(), 400u);
    const auto range = tree_.scan(key_of(10), key_of(20), 100);
    ASSERT_EQ(range.size(), 10u);
    EXPECT_EQ(range.front().first, key_of(10));
}

TEST_F(TreeFixture, MatchesStdMapUnderRandomOperations) {
    std::map<std::string, std::string> model;
    std::mt19937 rng(7);
    for (int step = 0; step < 40000; ++step) {
        const std::string key = key_of(static_cast<int>(rng() % 4000));
        if (rng() % 3 != 0) {
            const std::string value(rng() % (kMaxValueSize + 1), static_cast<char>('a' + rng() % 26));
            put(key, value);
            model[key] = value;
        } else {
            EXPECT_EQ(erase(key), model.erase(key) == 1);
        }
        if (step % 5000 == 0) {
            ASSERT_EQ(tree_.check(), model.size());
        }
    }
    ASSERT_EQ(tree_.check(), model.size());
    const auto all = tree_.scan("", "", SIZE_MAX);
    ASSERT_EQ(all.size(), model.size());
    auto it = model.begin();
    for (const auto& [k, v] : all) {
        ASSERT_EQ(k, it->first);
        ASSERT_EQ(v, it->second);
        ++it;
    }
}

```

Forty thousand operations over 4,000 keys with values up to the maximum size: hundreds of
splits, thousands of deletes, empty leaves, and a check every 5,000 steps. The fixed seed (7)
makes a failure reproducible; when one fails, print `step` and shrink the loop until the
smallest failing sequence remains.

### Commit

```bash
git add -A
git commit -m "Add BPlusTree erase and range scan; model-based test against std::map

- erase removes the cell without merging (documented: lazy deletion, file never shrinks)
- scan: one descent, then the leaf chain; [start, end) with a limit; cycle guard
- 40k random puts/erases checked against std::map with periodic full invariant checks"
git push
```

### What to say in an interview

"Deletes are lazy: the key is removed from its leaf, nothing is merged. That keeps the
delete path trivially correct, at the cost that space is only reclaimed by rebuilding the file,
which is a trade-off production engines also make. Range scans descend once and then walk the
leaf chain. The tree is tested model-based: forty thousand random operations against
`std::map`, comparing every result, with the full invariant checker running every few
thousand steps."

---

## Step 6. `Wal`: records, LSNs, sync, replay, truncate, reset

**Time:** ~6 hours   **Week:** 8
**Read first:** 07_Databases_Internals/04_wal_and_crash_recovery.md (all of it).
06_Linux_Toolchain_DevOps/07_debugging_and_profiling_tools_on_linux.md (`strace -e
trace=fdatasync,pwrite64` to see the commit protocol happen).

### Goal

The write-ahead log: append page-image and commit records, make them durable with one
`fdatasync`, read them back after a crash stopping cleanly at a torn tail, cut the log at a
chosen point, and replace it with an empty one at a checkpoint without ever losing the LSN
sequence.

In plain words: the receipt pad from `00_spec.md` section 1. Every receipt is numbered by its
position on an endless roll of paper (the LSN), sealed (CRC), and the roll is only considered
"in the safe" after `fdatasync` returns.

### Design

* **LSN = byte position** in a logical log that never restarts: `start_lsn + (offset - 24)`.
  The record stores its own LSN, so replay can tell a record written here from leftover bytes
  of an earlier, longer log.
* **Append buffers, `sync()` writes and syncs.** One `pwrite` and one `fdatasync` per commit,
  however many records the transaction has. `ensure_durable(lsn)` is what the cache's WAL hook
  calls: it syncs only if that LSN is not durable yet.
* **Replay stops at the first invalid record**: incomplete, wrong length for its type, LSN not
  equal to its position, or CRC mismatch. All of those mean "the crash happened here".
* **`truncate_at` is separate from `replay`.** Recovery decides where the log ends: just after
  the last *commit*, not just after the last valid record. The first draft of this code
  truncated after the last valid record; `Recovery.UncommittedTailIsDiscardedAndNotReplayedLater`
  (step 7) failed, because the page images of a crashed transaction stayed in the log, in front
  of the next transaction's records, and the *next* recovery applied them as part of that
  transaction's commit. Keep that story: it is a real ordering bug, and a test you wrote found it.
* **`reset()` never rewrites the header in place.** It builds `<path>.tmp` with a header whose
  `start_lsn` is the current end, syncs it, renames it over the log, syncs the directory. A
  crash leaves the old log (replaying it is harmless after a checkpoint) or the new empty one.
* **LSN 0 is reserved**, a new log starts at `kFirstLsn = 1`. Why is in step 7.

### Files

`include/minidb/wal.hpp`, `src/wal.cpp`; add `src/wal.cpp` to the library.

### Code

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "minidb/file.hpp"
#include "minidb/page.hpp"

namespace minidb {

enum class RecordType : std::uint8_t { PageImage = 1, Commit = 2 };

struct WalRecord {
    Lsn lsn = 0;
    RecordType type = RecordType::Commit;
    PageId page_id = 0;        // PageImage
    std::uint64_t txn_id = 0;  // Commit
    Page image;                // PageImage: the full page after the change, sealed
};

// Write-ahead log: an append-only file next to the data file ("<db>-wal").
//
// File layout:
//   header (24 bytes): u64 magic | u64 start_lsn | u32 crc32(first 16 bytes) | u32 zero
//   records, back to back:
//     u32 crc32(bytes [4, 24) + payload) | u32 payload_len | u64 lsn | u8 type | 3 x u8 zero |
//     u32 page_id | payload (4096-byte page image, or u64 txn id for Commit)
//
// LSN = start_lsn + (file offset - header size). LSNs only grow: reset() starts the next log
// at the current end LSN, so a page LSN can be compared with any record LSN.
//
// The header is never rewritten in place. A new log (creation or reset) is built as
// "<path>.tmp", fsynced, and renamed over the old one, so the file always has a valid header
// and either the old records or none.
//
// Durability: append_*() only buffer in memory; sync() writes the buffer and fdatasyncs.
// A record is durable once durable_lsn() is greater than its LSN.
class Wal {
public:
    // LSN 0 is reserved: it is the LSN of pages that were never logged (written by
    // Pager::create). If the first record also had LSN 0, recovery would see a page with
    // LSN 0 >= record LSN 0 and wrongly skip the redo.
    static constexpr Lsn kFirstLsn = 1;

    // Opens the log at `path`, creating an empty one (start LSN kFirstLsn) if it does not exist.
    // Throws CorruptionError if an existing file has an invalid header.
    explicit Wal(const std::string& path);
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Recovery: calls `fn` for every valid record in order, stopping at the first record that
    // is incomplete, has a bad checksum or an unexpected LSN: the torn tail of a crash.
    // Returns the number of valid records. Does not modify the file.
    std::size_t replay(const std::function<void(const WalRecord&)>& fn);

    // Recovery: discard everything from `end` onwards (the records of a transaction that never
    // committed, and any torn tail). Must be called before the first append after replay(),
    // or those orphaned page images would be mistaken for part of the next transaction.
    void truncate_at(Lsn end);

    // Stamps the page with the record's LSN, seals it, and buffers the record.
    Lsn append_page(PageId id, Page& page);
    Lsn append_commit(std::uint64_t txn_id);

    void sync();                     // write buffered records, then fdatasync
    void ensure_durable(Lsn lsn);    // sync() unless the record at `lsn` is already durable
    void discard_buffer() noexcept;  // drop unsynced records (a transaction that failed)

    // Checkpoint: the data file has been synced and contains every logged change. Replace
    // the log with an empty one that starts at end_lsn().
    void reset();

    Lsn start_lsn() const noexcept { return start_lsn_; }
    Lsn end_lsn() const noexcept { return end_lsn_; }          // LSN the next record gets
    Lsn durable_lsn() const noexcept { return durable_lsn_; }  // everything below is on disk
    std::uint64_t size_bytes() const noexcept { return kHeaderSize + (end_lsn_ - start_lsn_); }

    static constexpr std::size_t kHeaderSize = 24;
    static constexpr std::size_t kRecordHeaderSize = 24;

private:
    static void create_empty(const std::string& path, Lsn start_lsn);
    std::uint64_t offset_of(Lsn lsn) const noexcept { return kHeaderSize + (lsn - start_lsn_); }

    std::string path_;
    File file_;
    Lsn start_lsn_ = 0;
    Lsn end_lsn_ = 0;
    Lsn durable_lsn_ = 0;
    std::string buffer_;  // encoded records in [durable_lsn_, end_lsn_)
};

}  // namespace minidb
```

```cpp
#include "minidb/wal.hpp"

#include <array>

#include "minidb/crc32.hpp"

namespace minidb {

namespace {

constexpr std::uint64_t kWalMagic = 0x314C41574244494DULL;  // arbitrary, fixed forever

std::uint32_t payload_size(RecordType type) noexcept {
    return type == RecordType::PageImage ? static_cast<std::uint32_t>(kPageSize) : 8u;
}

void encode_record(std::string& out, Lsn lsn, RecordType type, PageId page_id, const char* payload,
                   std::uint32_t payload_len) {
    std::array<char, Wal::kRecordHeaderSize> header{};
    write_as(header.data() + 4, payload_len);
    write_as(header.data() + 8, lsn);
    write_as(header.data() + 16, static_cast<std::uint8_t>(type));
    write_as(header.data() + 20, page_id);
    std::uint32_t crc = crc32(header.data() + 4, header.size() - 4);
    crc = crc32(payload, payload_len, crc);
    write_as(header.data(), crc);
    out.append(header.data(), header.size());
    out.append(payload, payload_len);
}

}  // namespace

void Wal::create_empty(const std::string& path, Lsn start_lsn) {
    std::array<char, kHeaderSize> header{};
    write_as(header.data(), kWalMagic);
    write_as(header.data() + 8, start_lsn);
    write_as(header.data() + 16, crc32(header.data(), 16));

    const std::string tmp = path + ".tmp";
    {
        File file = File::open(tmp, /*create=*/true);
        file.truncate(0);
        file.write_at(0, header.data(), header.size());
        file.sync();
    }
    rename_file(tmp, path);         // atomic: readers see the old log or the new one
    sync_parent_directory(path);    // and the rename survives a power cut
}

Wal::Wal(const std::string& path) : path_(path) {
    if (!file_exists(path)) {
        create_empty(path, kFirstLsn);
    }
    file_ = File::open(path, /*create=*/false);

    std::array<char, kHeaderSize> header{};
    const std::size_t n = file_.read_at(0, header.data(), header.size());
    if (n != kHeaderSize || read_as<std::uint64_t>(header.data()) != kWalMagic ||
        read_as<std::uint32_t>(header.data() + 16) != crc32(header.data(), 16)) {
        throw CorruptionError("WAL " + path + ": invalid header");
    }
    start_lsn_ = read_as<Lsn>(header.data() + 8);
    end_lsn_ = start_lsn_;
    durable_lsn_ = start_lsn_;
}

std::size_t Wal::replay(const std::function<void(const WalRecord&)>& fn) {
    std::size_t count = 0;
    Lsn lsn = start_lsn_;
    WalRecord record;
    std::array<char, kRecordHeaderSize> header{};
    std::array<char, 8> commit_payload{};
    for (;;) {
        const std::uint64_t offset = offset_of(lsn);
        if (file_.read_at(offset, header.data(), header.size()) != header.size()) {
            break;  // clean end of log, or a torn record header
        }
        const auto len = read_as<std::uint32_t>(header.data() + 4);
        const auto type = static_cast<RecordType>(read_as<std::uint8_t>(header.data() + 16));
        if ((type != RecordType::PageImage && type != RecordType::Commit) || len != payload_size(type) ||
            read_as<Lsn>(header.data() + 8) != lsn) {
            break;  // garbage: not a record we wrote at this position
        }
        char* payload = type == RecordType::PageImage ? record.image.bytes.data() : commit_payload.data();
        if (file_.read_at(offset + kRecordHeaderSize, payload, len) != len) {
            break;  // torn payload
        }
        std::uint32_t crc = crc32(header.data() + 4, header.size() - 4);
        crc = crc32(payload, len, crc);
        if (crc != read_as<std::uint32_t>(header.data())) {
            break;  // torn or corrupted record
        }
        record.lsn = lsn;
        record.type = type;
        record.page_id = read_as<PageId>(header.data() + 20);
        record.txn_id = type == RecordType::Commit ? read_as<std::uint64_t>(commit_payload.data()) : 0;
        fn(record);
        ++count;
        lsn += kRecordHeaderSize + len;
    }
    return count;
}

void Wal::truncate_at(Lsn end) {
    if (end < start_lsn_) {
        throw DbError("WAL truncate_at: LSN before start of log");
    }
    // Cut the uncommitted or torn tail so new records follow the last commit directly.
    // Otherwise a later replay would see the orphaned images, or stop at the old garbage
    // before reaching newer committed records.
    file_.truncate(offset_of(end));
    file_.sync();
    buffer_.clear();
    end_lsn_ = end;
    durable_lsn_ = end;
}

Lsn Wal::append_page(PageId id, Page& page) {
    const Lsn lsn = end_lsn_;
    page.set_lsn(lsn);
    page.seal();
    encode_record(buffer_, lsn, RecordType::PageImage, id, page.bytes.data(),
                  static_cast<std::uint32_t>(kPageSize));
    end_lsn_ += kRecordHeaderSize + kPageSize;
    return lsn;
}

Lsn Wal::append_commit(std::uint64_t txn_id) {
    const Lsn lsn = end_lsn_;
    std::array<char, 8> payload{};
    write_as(payload.data(), txn_id);
    encode_record(buffer_, lsn, RecordType::Commit, 0, payload.data(), 8);
    end_lsn_ += kRecordHeaderSize + 8;
    return lsn;
}

void Wal::sync() {
    if (!buffer_.empty()) {
        file_.write_at(offset_of(durable_lsn_), buffer_.data(), buffer_.size());
    }
    file_.sync();
    durable_lsn_ = end_lsn_;
    buffer_.clear();
}

void Wal::ensure_durable(Lsn lsn) {
    if (lsn >= durable_lsn_) {
        sync();
    }
}

void Wal::discard_buffer() noexcept {
    buffer_.clear();
    end_lsn_ = durable_lsn_;
}

void Wal::reset() {
    if (!buffer_.empty()) {
        sync();
    }
    file_ = File();  // close before replacing the file
    create_empty(path_, end_lsn_);
    file_ = File::open(path_, /*create=*/false);
    start_lsn_ = end_lsn_;
    durable_lsn_ = end_lsn_;
}

}  // namespace minidb
```

Notes:

* The CRC covers the record header *after* the CRC field plus the payload, chained with the
  `previous` parameter of `crc32`: one checksum over two buffers without concatenating them.
* `discard_buffer()` is for a transaction that failed before its records reached the file.
  The next `sync()` writes at the durable offset, overwriting whatever a failed attempt may
  have left there.
* `size_bytes()` is what the `Db` compares with `checkpoint_wal_bytes`.

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug -R Wal
strace -f -e trace=pwrite64,fdatasync,rename ./build/debug/storage_test --gtest_filter='Wal.*' 2>&1 | head -40
```

The `strace` output shows the protocol: one `pwrite64` of the buffered records, one
`fdatasync`, and for `reset()` a write to `wal.tmp`, `fdatasync`, `rename`, and an `fsync` of
the directory.

### Tests

Add to `tests/storage_test.cpp`:

```cpp
TEST(Wal, AppendSyncReplay) {
    TempDir dir;
    const std::string path = dir.file("wal");
    Lsn first = 0;
    {
        Wal wal(path);
        wal.truncate_at(wal.start_lsn());
        Page page = page_with_byte('q');
        first = wal.append_page(7, page);
        EXPECT_EQ(page.lsn(), first);  // the page carries its record's LSN
        wal.append_commit(1);
        wal.sync();
        EXPECT_EQ(wal.durable_lsn(), wal.end_lsn());
    }
    Wal wal(path);
    std::vector<WalRecord> seen;
    EXPECT_EQ(wal.replay([&](const WalRecord& r) { seen.push_back(r); }), 2u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].type, RecordType::PageImage);
    EXPECT_EQ(seen[0].page_id, 7u);
    EXPECT_EQ(seen[0].lsn, first);
    EXPECT_TRUE(seen[0].image.verify());
    EXPECT_EQ(seen[1].type, RecordType::Commit);
    EXPECT_EQ(seen[1].txn_id, 1u);
}

TEST(Wal, TornTailIsIgnored) {
    TempDir dir;
    const std::string path = dir.file("wal");
    {
        Wal wal(path);
        Page page = page_with_byte('q');
        wal.append_page(1, page);
        wal.append_commit(1);
        wal.append_page(2, page);
        wal.append_commit(2);
        wal.sync();
    }
    {
        // Simulate a crash in the middle of writing the second transaction's page image.
        File f = File::open(path, false);
        f.truncate(Wal::kHeaderSize + (Wal::kRecordHeaderSize + kPageSize) + (Wal::kRecordHeaderSize + 8) + 1000);
    }
    Wal wal(path);
    std::size_t commits = 0;
    EXPECT_EQ(wal.replay([&](const WalRecord& r) { commits += r.type == RecordType::Commit ? 1 : 0; }), 2u);
    EXPECT_EQ(commits, 1u);
}

TEST(Wal, ResetKeepsLsnsMonotonic) {
    TempDir dir;
    const std::string path = dir.file("wal");
    Wal wal(path);
    Page page = page_with_byte('q');
    wal.append_page(1, page);
    wal.append_commit(1);
    wal.sync();
    const Lsn end = wal.end_lsn();
    wal.reset();
    EXPECT_EQ(wal.start_lsn(), end);
    EXPECT_EQ(wal.size_bytes(), Wal::kHeaderSize);
    EXPECT_EQ(wal.append_commit(2), end);  // the next record continues the sequence
}
```

### Commit

```bash
git add -A
git commit -m "Add write-ahead log: page-image and commit records, LSNs, replay, reset

- record: CRC over header and payload, length, LSN equal to its position, type, page id
- append buffers; sync() is one pwrite + one fdatasync; ensure_durable() for the WAL rule
- replay stops at the first invalid record (torn tail); truncate_at() cuts the log where
  recovery decides (after the last commit)
- reset() via tmp file + rename + directory fsync; LSNs continue across resets"
git push
```

### What to say in an interview

"The log is append-only and each record carries a CRC and its own LSN, which is its byte
position in a logical log that never restarts, so replay can tell the torn tail of a crash
from valid records. A commit is one `pwrite` and one `fdatasync` of everything the transaction
appended. Two details I got wrong first and my tests caught: after a crash the log must be cut
after the last *commit*, not the last valid record, or a crashed transaction's page images get
replayed as part of the next commit; and LSN 0 must be reserved for pages that were never
logged."

---

## Step 7. `Recovery` and `Db`: the commit protocol and checkpoints

**Time:** ~8 hours   **Week:** 9
**Read first:** 07_Databases_Internals/04_wal_and_crash_recovery.md again (ARIES: analysis,
redo, undo; steal/no-steal, force/no-force), `05_transactions_acid_mvcc.md` (atomicity and
durability). 04_Concurrency/03_mutexes_lock_guard_unique_lock.md. 02_Modern_CPP:
`10_error_handling_exceptions_optional_expected.md` (exception safety: the strong guarantee).

### Goal

Wire everything into the public `Db` class: transactions around every `put` and `del`, the
commit protocol, checkpoints, and redo-only crash recovery on open. After this step the
durability contract in `00_spec.md` section 2 holds.

### Design

The commit protocol, in the order that makes it crash-safe (`Db::commit`):

```
1. for every page in the transaction: Wal::append_page   (stamps the page LSN, seals it)
2. Wal::append_commit
3. Wal::sync()                 <-- durable from here; the call may now return
4. PageCache::commit_txn()     pages become ordinary dirty pages, evictable
```

The data file is not touched at commit (no-force). It catches up at eviction (after the WAL
hook) or at a checkpoint.

* **Strong exception guarantee for `put`/`del`.** `write_txn` runs the tree operation and the
  commit; if anything throws, `abort_txn()` restores every before-image and
  `discard_buffer()` forgets the buffered records. The database is exactly as it was before
  the call.
* **A failed `fdatasync` poisons the `Db`.** After a failed `fsync` the kernel may have dropped
  the dirty pages and cleared the error, so retrying can falsely succeed (PostgreSQL's 2018
  "fsyncgate"). The only safe response is to refuse further operations and let recovery decide
  on the next open.
* **Checkpoint** = `flush_all()`, `Pager::sync()`, `Wal::reset()`. Run when the log passes
  `checkpoint_wal_bytes`, on open after recovery, and in the destructor.
* **Recovery** (`00_spec.md` section 7): replay, collect page images per transaction, apply
  them at the commit record, skip pages whose LSN shows they are already current, discard the
  uncommitted tail, sync the data file, cut the log after the last commit.
* **The LSN-0 bug.** In the first draft the log started at LSN 0. A freshly created database
  has page 1 (the root leaf) with page LSN 0. The first `put` logged page 1 at LSN 0. After a
  crash, recovery compared the data file's page LSN (0) with the record's (0), concluded "already
  current", and skipped the redo: the first committed write of every database was lost if the
  process died before a checkpoint. The SIGKILL crash test did not catch it (by the time it
  kills, page 1 has been logged many times at higher LSNs); the deterministic test below did.
  The fix is one constant: logs start at LSN 1, and LSN 0 means "never logged".
* **Member order is construction order.** `Pager`, then `Wal`, then `RecoveryStats` (whose
  initialiser *runs* recovery), then the cache and the tree. Recovery must finish before the
  cache reads its first page. The constructor initialiser list makes that ordering explicit and
  impossible to get wrong later.

### Files

`include/minidb/recovery.hpp`, `src/recovery.cpp`, `include/minidb/db.hpp`, `src/db.cpp`,
`tests/db_test.cpp`.

### Code

```cpp
#pragma once

#include <cstddef>

#include "minidb/pager.hpp"
#include "minidb/wal.hpp"

namespace minidb {

struct RecoveryStats {
    std::size_t records = 0;              // valid records found in the log
    std::size_t committed_txns = 0;       // transactions with a commit record
    std::size_t pages_redone = 0;         // page images written to the data file
    std::size_t pages_skipped = 0;        // data file already had this image or a newer one
    std::size_t discarded_records = 0;    // records of a transaction that never committed
    Lsn end_lsn = 0;                      // log end after recovery (just past the last commit)
};

// Redo-only crash recovery.
//
// Why redo-only works: the PageCache never writes a page modified by an uncommitted
// transaction to the data file (no-steal), so the data file never needs undoing. It may,
// however, be missing committed changes (no-force: commit only syncs the log) and may hold a
// torn page (crash during a page write). Every committed change is in the log as a full page
// image, so replaying committed images in log order repairs both.
//
// Idempotent: running it twice, or crashing during it and running it again, gives the same
// result, because applying a full page image is idempotent and replay is in LSN order.
class Recovery {
public:
    static RecoveryStats run(Pager& pager, Wal& wal);
};

}  // namespace minidb
```

```cpp
#include "minidb/recovery.hpp"

#include <utility>
#include <vector>

namespace minidb {

RecoveryStats Recovery::run(Pager& pager, Wal& wal) {
    RecoveryStats stats;
    stats.end_lsn = wal.start_lsn();

    // Page images of the transaction being read; applied only when its commit record shows
    // up. Images of a transaction whose commit never reached the disk are never applied.
    std::vector<std::pair<PageId, Page>> pending;
    Page current;

    stats.records = wal.replay([&](const WalRecord& record) {
        if (record.type == RecordType::PageImage) {
            pending.emplace_back(record.page_id, record.image);
            return;
        }
        // Commit: redo every image of this transaction, in log order.
        for (auto& [page_id, image] : pending) {
            // The page LSN check is an optimisation, not a correctness requirement: if the
            // data file already holds this image or a later one, rewriting is unnecessary.
            // A torn or missing page fails read_if_valid() and is always rewritten.
            if (pager.read_if_valid(page_id, current) && current.lsn() >= image.lsn()) {
                ++stats.pages_skipped;
                continue;
            }
            pager.write(page_id, image);
            ++stats.pages_redone;
        }
        pending.clear();
        ++stats.committed_txns;
        stats.end_lsn = record.lsn + Wal::kRecordHeaderSize + 8;  // just past the commit record
    });

    stats.discarded_records = pending.size();
    pager.sync();                  // redone pages durable before the log can be reset
    wal.truncate_at(stats.end_lsn);  // drop the uncommitted tail before anything is appended
    return stats;
}

}  // namespace minidb
```

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minidb/btree.hpp"
#include "minidb/page_cache.hpp"
#include "minidb/pager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/wal.hpp"

namespace minidb {

struct Options {
    std::size_t cache_pages = 256;                    // 1 MiB of page cache
    std::uint64_t checkpoint_wal_bytes = 8u << 20;    // checkpoint when the log passes 8 MiB
};

// The public API. Every put/del is its own transaction and is durable when the call returns:
// its WAL records have been fdatasynced. One mutex serialises all calls.
class Db {
public:
    // Opens `path` (and "<path>-wal"), creating both if the data file does not exist,
    // and runs crash recovery.
    explicit Db(const std::string& path, Options options = {});
    ~Db();  // checkpoints, so a clean shutdown leaves an empty log
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void put(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key);
    bool del(std::string_view key);
    std::vector<std::pair<std::string, std::string>> scan(std::string_view start, std::string_view end = {},
                                                          std::size_t limit = SIZE_MAX);
    void checkpoint();
    std::size_t check();  // full structural check; returns key count

    const RecoveryStats& recovery_stats() const noexcept { return recovery_; }
    std::uint64_t wal_bytes() const noexcept { return wal_.size_bytes(); }
    const PageCache::Stats& cache_stats() const noexcept { return cache_.stats(); }

private:
    template <class F>
    auto write_txn(F&& body);
    void commit();
    void checkpoint_locked();
    void throw_if_failed() const;

    std::mutex mutex_;
    Options options_;
    Pager pager_;           // declaration order is construction order:
    Wal wal_;               // the log must exist before recovery,
    RecoveryStats recovery_;  // recovery must finish before the cache reads any page
    PageCache cache_;
    BPlusTree tree_;
    std::uint64_t next_txn_id_ = 1;
    bool failed_ = false;   // set when a commit fails part-way; see commit()
};

}  // namespace minidb
```

```cpp
#include "minidb/db.hpp"

#include <stdexcept>

namespace minidb {

namespace {

// Runs before the Pager member is constructed: makes sure a data file exists.
const std::string& ensure_database(const std::string& path) {
    if (!file_exists(path)) {
        // A log left over from an older database with the same name must not be replayed
        // into the new one: replace it with an empty log first, then create the data file.
        if (file_exists(path + "-wal")) {
            rename_file(path + "-wal", path + "-wal.orphaned");
        }
        Pager::create(path, BPlusTree::initial_pages());
    }
    return path;
}

void validate(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() > kMaxKeySize) {
        throw std::invalid_argument("minidb: key must be 1.." + std::to_string(kMaxKeySize) + " bytes");
    }
    if (value.size() > kMaxValueSize) {
        throw std::invalid_argument("minidb: value must be at most " + std::to_string(kMaxValueSize) + " bytes");
    }
}

}  // namespace

Db::Db(const std::string& path, Options options)
    : options_(options),
      pager_(ensure_database(path)),
      wal_(path + "-wal"),
      recovery_(Recovery::run(pager_, wal_)),
      cache_(pager_, options.cache_pages, [this](Lsn lsn) { wal_.ensure_durable(lsn); }),
      tree_(cache_) {
    {
        PageCache::PageRef meta = cache_.fetch(kMetaPageId);
        (void)Meta::decode(meta.read());  // throws CorruptionError for a non-minidb file
    }
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_locked();  // fold the recovered log into the data file; start with an empty log
}

Db::~Db() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_) {
            checkpoint_locked();
        }
    } catch (...) {
        // Destructors must not throw. Nothing is lost: the log still holds every commit.
    }
}

void Db::throw_if_failed() const {
    if (failed_) {
        throw DbError("minidb: a previous commit failed; reopen the database to recover");
    }
}

template <class F>
auto Db::write_txn(F&& body) {
    throw_if_failed();
    try {
        auto result = body();
        commit();
        return result;
    } catch (...) {
        cache_.abort_txn();     // restore before-images: memory matches the last commit again
        wal_.discard_buffer();  // nothing of this transaction reaches the log
        throw;
    }
}

// The commit protocol, in the order that makes it crash-safe:
//   1. append a full image of every page this transaction changed (stamps page LSNs)
//   2. append a commit record
//   3. fdatasync the log                  <- the transaction is durable from here
//   4. release the pages to the cache as ordinary dirty pages (they may now be evicted)
// The data file is not touched. It catches up at eviction or checkpoint.
void Db::commit() {
    if (!cache_.in_txn()) {
        return;  // e.g. del() of a missing key changed nothing
    }
    cache_.for_each_txn_page([this](PageId id, Page& page) { wal_.append_page(id, page); });
    wal_.append_commit(next_txn_id_++);
    try {
        wal_.sync();
    } catch (...) {
        // fsync failed. The kernel may have dropped the dirty log pages, so we cannot know
        // what is on disk ("fsyncgate", PostgreSQL 2018). The only safe answer is to stop
        // accepting writes and let recovery decide on the next open.
        failed_ = true;
        throw;
    }
    cache_.commit_txn();
}

void Db::put(std::string_view key, std::string_view value) {
    validate(key, value);
    std::lock_guard<std::mutex> lock(mutex_);
    write_txn([&] {
        tree_.put(key, value);
        return true;
    });
    if (wal_.size_bytes() >= options_.checkpoint_wal_bytes) {
        checkpoint_locked();
    }
}

bool Db::del(std::string_view key) {
    if (key.empty() || key.size() > kMaxKeySize) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = write_txn([&] { return tree_.erase(key); });
    if (wal_.size_bytes() >= options_.checkpoint_wal_bytes) {
        checkpoint_locked();
    }
    return removed;
}

std::optional<std::string> Db::get(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    return tree_.get(key);
}

std::vector<std::pair<std::string, std::string>> Db::scan(std::string_view start, std::string_view end,
                                                          std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    return tree_.scan(start, end, limit);
}

std::size_t Db::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tree_.check();
}

void Db::checkpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    checkpoint_locked();
}

// Checkpoint: make the data file contain everything the log contains, then empty the log.
//   1. write every dirty page (each one's WAL record is already durable: the WAL rule)
//   2. fdatasync the data file
//   3. replace the log with an empty one starting at the current end LSN
// A crash before step 3 completes leaves the old log in place; replaying it is harmless.
void Db::checkpoint_locked() {
    cache_.flush_all();
    pager_.sync();
    wal_.reset();
}

}  // namespace minidb
```

`ensure_database` renames a log left behind by an older database of the same name instead of
replaying it into the new one: without that, deleting the data file but not the log and then
reopening would apply a stranger's page images to a fresh database. (A stronger version stores
a random database id in both the meta page and the log header and refuses to replay on a
mismatch; a good exercise.)

### Build and run

```bash
cmake --build --preset debug && ctest --preset debug
cmake --build --preset asan  && ctest --preset asan
```

`db_test` is slow by design: every `put` waits for `fdatasync`. On the WSL2 machine used to
check this code, its roughly 15,000 synced writes took about 40 seconds. Run
`ctest --preset debug -R 'Db|Recovery' --output-on-failure` while iterating.

### Tests

`tests/db_test.cpp` in full. The three `Recovery` tests simulate a crash by copying the data
file and the log while the `Db` is still open (so its destructor's checkpoint has not run),
then damaging the copy in a specific way:

```cpp
// BPlusTree through the Db API, recovery, and a model-based property test against std::map.

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "minidb/db.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%08d", i);
    return buf;
}

}  // namespace

TEST(Db, PutGetDelOnFreshDatabase) {
    TempDir dir;
    Db db(dir.file("db"));
    EXPECT_FALSE(db.get("a").has_value());
    db.put("a", "1");
    db.put("b", "2");
    db.put("a", "3");  // overwrite
    EXPECT_EQ(db.get("a").value_or(""), "3");
    EXPECT_TRUE(db.del("a"));
    EXPECT_FALSE(db.del("a"));
    EXPECT_FALSE(db.get("a").has_value());
    EXPECT_EQ(db.check(), 1u);
}

TEST(Db, RejectsOversizedKeysAndValues) {
    TempDir dir;
    Db db(dir.file("db"));
    EXPECT_THROW(db.put("", "v"), std::invalid_argument);
    EXPECT_THROW(db.put(std::string(kMaxKeySize + 1, 'k'), "v"), std::invalid_argument);
    EXPECT_THROW(db.put("k", std::string(kMaxValueSize + 1, 'v')), std::invalid_argument);
    EXPECT_NO_THROW(db.put(std::string(kMaxKeySize, 'k'), std::string(kMaxValueSize, 'v')));
}

TEST(Db, ManySplitsKeepTheTreeValidAndScansOrdered) {
    TempDir dir;
    Db db(dir.file("db"), Options{32, 1u << 20});  // small cache: forces eviction and write-back
    constexpr int kCount = 6000;  // with 300..699-byte values: ~1,000 leaves, so internal nodes split too
    for (int i = 0; i < kCount; ++i) {
        const int k = (i * 7919) % kCount;  // a permutation: inserts arrive out of order
        db.put(key_of(k), std::string(static_cast<std::size_t>(300 + k % 400), 'v'));
    }
    EXPECT_EQ(db.check(), static_cast<std::size_t>(kCount));
    const auto all = db.scan("", "");
    ASSERT_EQ(all.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(all[static_cast<std::size_t>(i)].first, key_of(i));
    }
    const auto range = db.scan(key_of(100), key_of(110));
    ASSERT_EQ(range.size(), 10u);
    EXPECT_EQ(range.front().first, key_of(100));
    EXPECT_EQ(range.back().first, key_of(109));
    EXPECT_EQ(db.scan(key_of(5), "", 3).size(), 3u);
}

TEST(Db, MatchesStdMapUnderRandomOperations) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::map<std::string, std::string> model;
    std::mt19937 rng(12345);
    {
        Db db(path, Options{24, 256u << 10});
        for (int step = 0; step < 10000; ++step) {
            const std::string key = key_of(static_cast<int>(rng() % 1500));
            const unsigned op = rng() % 10;
            if (op < 6) {
                const std::string value(rng() % (kMaxValueSize + 1), static_cast<char>('a' + rng() % 26));
                db.put(key, value);
                model[key] = value;
            } else if (op < 9) {
                EXPECT_EQ(db.del(key), model.erase(key) == 1);
            } else {
                const auto got = db.get(key);
                const auto it = model.find(key);
                ASSERT_EQ(got.has_value(), it != model.end());
                if (got) {
                    EXPECT_EQ(*got, it->second);
                }
            }
        }
        EXPECT_EQ(db.check(), model.size());
    }
    Db reopened(path);  // clean close and reopen: same contents
    const auto all = reopened.scan("", "");
    ASSERT_EQ(all.size(), model.size());
    auto it = model.begin();
    for (const auto& [k, v] : all) {
        EXPECT_EQ(k, it->first);
        EXPECT_EQ(v, it->second);
        ++it;
    }
}

TEST(Recovery, CommittedWritesSurviveWithoutCheckpoint) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        // A huge checkpoint threshold and no clean close: everything after open lives only
        // in the log and in the cache. Simulate the crash by copying the files before the
        // destructor runs its checkpoint.
        Db db(path, Options{64, 1u << 30});
        for (int i = 0; i < 500; ++i) {
            db.put(key_of(i), "value" + std::to_string(i));
        }
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    Db db(path + ".crash");
    EXPECT_GT(db.recovery_stats().committed_txns, 0u);
    EXPECT_EQ(db.check(), 500u);
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(db.get(key_of(i)).value_or(""), "value" + std::to_string(i));
    }
}

namespace {

// Ends of the committed records in a log file, and the page ids they carry.
struct LogSummary {
    Lsn end_of_last_commit = 0;
    std::vector<PageId> pages;
};

LogSummary summarize_log(const std::string& wal_path) {
    LogSummary summary;
    Wal wal(wal_path);
    summary.end_of_last_commit = wal.start_lsn();
    wal.replay([&](const WalRecord& r) {
        if (r.type == RecordType::PageImage) {
            summary.pages.push_back(r.page_id);
        } else {
            summary.end_of_last_commit = r.lsn + Wal::kRecordHeaderSize + 8;
        }
    });
    return summary;
}

}  // namespace

TEST(Recovery, TornDataPageIsRepairedFromTheLog) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        Db db(path, Options{64, 1u << 30});
        for (int i = 0; i < 300; ++i) {
            db.put(key_of(i), "v");
        }
        db.checkpoint();                        // data file current, log empty
        db.put(key_of(0), "after-checkpoint");  // logged; the leaf is dirty only in the cache
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    const LogSummary log = summarize_log(path + ".crash-wal");
    ASSERT_FALSE(log.pages.empty());
    {
        // Tear the logged page in the copy: second half overwritten with garbage, as if the
        // power failed in the middle of the 4 KiB write.
        File f = File::open(path + ".crash", false);
        const std::string junk(kPageSize / 2, '\x7f');
        f.write_at(static_cast<std::uint64_t>(log.pages.front()) * kPageSize + kPageSize / 2, junk.data(),
                   junk.size());
    }
    Db db(path + ".crash");
    EXPECT_GE(db.recovery_stats().pages_redone, 1u);
    EXPECT_EQ(db.get(key_of(0)).value_or(""), "after-checkpoint");
    EXPECT_EQ(db.check(), 300u);
}

TEST(Recovery, UncommittedTailIsDiscardedAndNotReplayedLater) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        Db db(path, Options{64, 1u << 30});
        db.put("a", "1");
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    {
        // Append one page image with no commit record: a crash between append and commit.
        // The image is an empty leaf for the root page; applying it would wipe "a".
        const LogSummary log = summarize_log(path + ".crash-wal");
        Wal wal(path + ".crash-wal");
        wal.truncate_at(log.end_of_last_commit);
        Page empty_leaf = BPlusTree::initial_pages()[1];
        wal.append_page(1, empty_leaf);
        wal.sync();
    }
    {
        Db db(path + ".crash");
        EXPECT_EQ(db.recovery_stats().discarded_records, 1u);
        EXPECT_EQ(db.get("a").value_or(""), "1");
        db.put("b", "2");  // appended after the discarded tail was cut off
    }
    Db db(path + ".crash");
    EXPECT_EQ(db.get("a").value_or(""), "1");
    EXPECT_EQ(db.get("b").value_or(""), "2");
}
```

### Commit

```bash
git add -A
git commit -m "Add Db, commit protocol, checkpoints and redo-only recovery

- Db: one mutex; every put/del is a transaction with the strong exception guarantee
- commit: append page images + commit record, fdatasync, then release pages (no-force)
- a failed fdatasync poisons the Db until reopen (fsyncgate)
- checkpoint: flush dirty pages, sync data file, reset the log; on open and on close
- Recovery: redo committed images in LSN order, skip current pages, discard the
  uncommitted tail and cut the log after the last commit
- LSN 0 reserved for never-logged pages (first record at LSN 1 was skipped by redo)
- tests: model test through the API, recovery without checkpoint, torn page repair,
  uncommitted tail discarded and not replayed later"
git push
```

### What to say in an interview

"Every put is a transaction. Its changed pages are logged as full images plus a commit
record, the log is `fdatasync`ed once, and only then does the call return. The data file is
written later, at eviction or checkpoint, always after the log record it depends on. Because
uncommitted pages never reach the data file, recovery is redo-only: replay the log, apply
committed page images, drop the uncommitted tail. Full images make redo idempotent and repair
torn pages for free. Compared with ARIES I gave up steal, so a transaction's pages must fit in
the cache, and in exchange I have no undo pass and no compensation records. And I can tell
you about the bug where the first write of every database was lost because the first log
record had the same LSN as a never-logged page."

---

## Step 8. The crash test, CLI, benchmark, CI, README, tag v0.1.0

**Time:** ~6 hours   **Week:** 9
**Read first:** 06_Linux_Toolchain_DevOps/09_ci_cd_github_actions_and_azure_devops.md.
04_Concurrency/01_processes_threads_and_the_os.md (`fork`, signals, `waitpid`).
tcpkv step 10 (the release checklist; apply the same one here).

### Goal

Prove the durability contract with a test that kills the process mid-write, add a CLI and a
benchmark, make CI run everything under the sanitizers, and tag v0.1.0.

### Design

The crash test is specified in `00_spec.md` section 8. The design points to defend:

* **`fork()` plus `SIGKILL`**, not an exception or `exit()`: `SIGKILL` cannot be caught, so no
  destructor, checkpoint or flush runs. That is a process crash, faithfully.
* **The pipe is the acknowledgement channel.** The child writes `i + 1` after operation `i`
  returns. The parent drains the pipe after the kill: everything the child wrote before dying
  is still there, and `read` returns 0 once the only write end has died.
* **Deterministic operations.** Operation `i` is a pure function of `i`, so the parent rebuilds
  the expected state for any prefix with a `std::map`, without trusting the child.
* **`acked` or `acked + 1`.** The operation in flight may have committed just before the kill,
  without its acknowledgement reaching the pipe. Anything else is a bug.
* **A small cache and a small checkpoint threshold** in the child, so kills land during
  evictions and checkpoints, not only during commits.
* **The test must be able to fail.** It also asserts that something was acknowledged, and it
  was checked against a mutant: with redo disabled in `Recovery`, it fails on the first kill
  (`state matches neither 28 nor 29 operations`). Do the same mutation on your version once.
* **What it does not test: power loss.** After `SIGKILL` the kernel still writes its page
  cache to disk, so an engine that never called `fdatasync` would pass. Say so before you are
  asked, and name the tools that do test it (`dm-log-writes`, `dm-flakey`, LazyFS, VM
  snapshots).

### Files

`tests/crash_test.cpp`, `apps/minidb_cli.cpp`, `tools/bench.cpp`, `README.md`, CI updates.

### Code

`tests/crash_test.cpp`:

```cpp
// The crash test: a child process writes as fast as it can and reports every acknowledged
// operation through a pipe; the parent SIGKILLs it at a random moment, reopens the database
// and checks that the state equals the model after exactly the acknowledged operations
// (or after one more: the operation in flight may have committed just before the kill).
//
// SIGKILL cannot be caught, so no destructor, no checkpoint and no flush runs in the child.
// That is the same as a process crash. It is not the same as a power failure (the kernel
// still writes back the page cache); see the spec, section 8, for what that would take.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "minidb/db.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

constexpr int kKeySpace = 400;

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%05d", i % kKeySpace);
    return buf;
}

// Operation i is a pure function of i, so the parent can recompute the expected state for
// any prefix of the operation sequence without talking to the child.
struct Op {
    bool is_delete = false;
    std::string key;
    std::string value;
};

Op op_at(std::uint32_t i) {
    Op op;
    op.key = key_of(static_cast<int>((i * 7u) % kKeySpace));
    op.is_delete = i % 11 == 10;
    if (!op.is_delete) {
        op.value = "v" + std::to_string(i) + ":" + std::string(i % 600, 'x');  // 0..~610 bytes
    }
    return op;
}

std::map<std::string, std::string> model_after(std::uint32_t count) {
    std::map<std::string, std::string> model;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Op op = op_at(i);
        if (op.is_delete) {
            model.erase(op.key);
        } else {
            model[op.key] = op.value;
        }
    }
    return model;
}

[[noreturn]] void child_main(const std::string& path, int ack_fd) {
    try {
        // Small cache and a small checkpoint threshold, so kills land during evictions and
        // during checkpoints, not only during commits.
        Db db(path, Options{16, 128u << 10});
        for (std::uint32_t i = 0;; ++i) {
            const Op op = op_at(i);
            if (op.is_delete) {
                db.del(op.key);
            } else {
                db.put(op.key, op.value);
            }
            // The acknowledgement: operation i returned, so it must survive any crash.
            const std::uint32_t done = i + 1;
            if (::write(ack_fd, &done, sizeof done) != static_cast<ssize_t>(sizeof done)) {
                ::_exit(3);
            }
        }
    } catch (...) {
        ::_exit(2);
    }
}

std::map<std::string, std::string> contents(Db& db) {
    std::map<std::string, std::string> out;
    for (auto& [k, v] : db.scan("", "")) {
        out.emplace(std::move(k), std::move(v));
    }
    return out;
}

}  // namespace

TEST(Crash, SigkillMidWriteLosesNoAcknowledgedOperation) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::mt19937 rng(std::random_device{}());
    std::uint32_t total_acked = 0;

    for (int round = 0; round < 25; ++round) {
        // Every round starts from an empty database, so the expected state is simply the
        // model after the first `acked` operations.
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");

        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            ::close(fds[0]);
            child_main(path, fds[1]);  // never returns
        }
        ::close(fds[1]);

        // Let the child run for a random 20..200 ms, then kill it wherever it is.
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + rng() % 180));
        ASSERT_EQ(::kill(pid, SIGKILL), 0);
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFSIGNALED(status)) << "child exited early with status " << WEXITSTATUS(status);

        // Acknowledgements written before the kill are still in the pipe. read() returns 0
        // once they are drained, because the only write end died with the child.
        std::uint32_t acked = 0;
        std::uint32_t value = 0;
        while (::read(fds[0], &value, sizeof value) == static_cast<ssize_t>(sizeof value)) {
            acked = value;
        }
        ::close(fds[0]);

        Db db(path, Options{16, 128u << 10});  // runs recovery
        db.check();
        const auto actual = contents(db);
        EXPECT_TRUE(actual == model_after(acked) || actual == model_after(acked + 1))
            << "round " << round << ": state matches neither " << acked << " nor " << acked + 1
            << " operations (recovered " << db.recovery_stats().committed_txns << " txns, discarded "
            << db.recovery_stats().discarded_records << " records)";
        total_acked += acked;
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_GT(total_acked, 0u) << "the child never acknowledged anything; the test proved nothing";
    std::printf("[ crash ] %u operations acknowledged across 25 kills\n", total_acked);
}
```

Notes on the code:

* The child calls `::_exit`, never `exit`, on its error paths: `exit` would run GoogleTest's
  and the parent's static destructors in the child, a classic `fork` bug.
* `fork` is safe here because the test process is single-threaded at that point. Forking a
  multithreaded process copies only the calling thread, and any mutex another thread held
  stays locked forever in the child.
* A run prints how much it proved, for example `[ crash ] 1011 operations acknowledged across
  25 kills` (from one run of the reference code on WSL2; yours will differ). If that number is
  near zero, the kills are landing before the database even opens: raise the sleep range.

`apps/minidb_cli.cpp`, outlined (about 80 lines; you wrote `tcpkv_cli` already):

* `minidb_cli <file> <command> [args]`; commands as in `00_spec.md` section 3.2.
* Open `minidb::Db db(file);` print `db.recovery_stats()` on stderr if it redid or discarded
  anything, so you can watch recovery happen after you `kill -9` a writer.
* `put` takes the rest of the command line as the value; `get` prints the value or `(nil)`;
  `scan` prints `key<TAB>value` lines; `check` prints the key count or the `CorruptionError`
  message and exits 1; `stats` prints page count (meta), log size, cache hits/misses.
* Catch `std::invalid_argument` (usage error, exit 2), `minidb::CorruptionError` (exit 3),
  `std::exception` (exit 1).

A manual crash demo for the README:

```bash
./build/release/minidb_cli /tmp/demo.db put greeting hello
( while true; do ./build/release/minidb_cli /tmp/demo.db put k$RANDOM v; done ) &
sleep 2; kill -9 %1
./build/release/minidb_cli /tmp/demo.db check     # recovery runs, then the checker
```

(`kill -9` here hits the shell loop or one CLI process mid-write; either way the next open
recovers.)

`tools/bench.cpp`, outlined: `minidb_bench <file> <count> <value_size>` times `count` puts with
sequential keys, `count` puts with random keys, `count` gets, and one full scan, and prints
operations per second for each. Expect puts to be bound by `fdatasync` latency, so the number
mostly measures your disk (and WSL2's virtual disk), not the tree: say exactly that in the
README, next to the numbers and the machine. The experiment worth adding: a second run with a
`WriteBatch` of 100 puts per transaction (one sync per batch) shows how much group commit
would buy.

CI: add `crash` to the test list (already in the `CMakeLists.txt` above) and keep the
`debug, release, asan, tsan, ubsan` matrix from tcpkv. On the TSan job, apply the same
`vm.mmap_rnd_bits=28` step as tcpkv.

README: follow `00_spec.md` section 10. The first screen must state the durability contract
in one sentence and link to the crash test.

### Build and run

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan
setarch $(uname -m) -R cmake --preset tsan && setarch $(uname -m) -R cmake --build --preset tsan && setarch $(uname -m) -R ctest --preset tsan
./build/release/minidb_bench /tmp/bench.db 10000 100
```

### Tests

`Crash.SigkillMidWriteLosesNoAcknowledgedOperation` passes in every preset, and fails against
the redo-disabled mutant. The full suite is then 21 tests: 9 storage, 4 tree, 7 database and
recovery, 1 crash.

### Commit and tag

```bash
git add -A
git commit -m "Add SIGKILL crash test, CLI and benchmark; release 0.1.0

- crash test: forked writer acknowledges each op through a pipe, SIGKILLed at a random
  moment 25 times; reopened state must equal the model after acked or acked+1 ops
- verified to fail when recovery skips redo
- minidb_cli (put/get/del/scan/check/stats), minidb_bench (fsync-bound put rate, get rate)
- README: durability contract, format, commit protocol, recovery, limitations"
git push
git tag -a v0.1.0 -m "minidb 0.1.0: B+tree storage engine with WAL and crash recovery"
git push origin v0.1.0
```

Then make the repository public and pin it next to tcpkv, exactly as in tcpkv step 10.

### What to say in an interview

"The durability claim is tested, not asserted. A forked child writes as fast as it can and
reports each acknowledged operation through a pipe; the parent kills it with SIGKILL at a
random moment, reopens the database, which runs recovery, and checks that the contents equal a
model after exactly the acknowledged operations, or one more for the operation in flight. It
does that 25 times per run, under ASan and TSan too, and I checked that the test fails if I
disable redo. What it does not cover is power loss, because after a process crash the kernel
still flushes its page cache; for that I would use `dm-log-writes` or LazyFS to replay every
prefix of the write stream."

---

## After v0.1.0: the resume line, and what next

Only once the tag exists, CI is green and the repository is public:

> **minidb — B+tree storage engine with write-ahead logging** · C++17, Linux, CMake, GoogleTest ·
> github.com/khanshaki-r/minidb
> - Page-based key-value store: 4 KiB checksummed pages, LRU buffer pool with no-steal/no-force
>   policies, B+tree with byte-balanced splits and range scans, full-page-image WAL with LSNs,
>   checkpoints, and redo-only crash recovery.
> - Crash test kills the writer with SIGKILL mid-operation 25 times per run and verifies that
>   every acknowledged write survives and no unacknowledged one appears; runs under ASan, TSan
>   and UBSan in CI.

Do not add a throughput number unless you measured it with the procedure from tcpkv step 10,
and if you do, say that it is fsync-bound.

What next, in the order an interviewer would find most interesting (each is in `00_spec.md`
section 11): group commit; delta log records with a full image on first touch after a
checkpoint; latch crabbing for concurrent readers; merges and a free list; power-loss testing.
