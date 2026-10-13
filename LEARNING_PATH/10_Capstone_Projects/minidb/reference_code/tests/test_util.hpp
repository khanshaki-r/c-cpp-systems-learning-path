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
