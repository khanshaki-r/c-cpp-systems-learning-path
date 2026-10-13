// m05_unique_ptr_c_api.cpp
// std::unique_ptr with custom deleters: wrap any C API (FILE*, malloc, library handles) with zero overhead.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m05_unique_ptr_c_api.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

// Deleter as an EMPTY struct: unique_ptr stores it with the empty-base optimisation, so the whole
// smart pointer is exactly one pointer wide.
struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) { std::fclose(f); std::cout << "  fclose\n"; }
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

struct FreeDeleter {
    void operator()(void* p) const noexcept { std::free(p); std::cout << "  free\n"; }
};
template <class T>
using MallocPtr = std::unique_ptr<T, FreeDeleter>;

FilePtr open_file(const char* path, const char* mode) {
    return FilePtr(std::fopen(path, mode));   // may hold nullptr; the caller checks like with a raw FILE*
}

// A fake C library handle, standing in for a DB connection, a compression context, an SSL_CTX...
struct zctx { int level; };
zctx* zctx_new(int level) {
    auto* c = static_cast<zctx*>(std::malloc(sizeof(zctx)));
    if (c) c->level = level;
    return c;
}
void zctx_free(zctx* c) { std::cout << "  zctx_free\n"; std::free(c); }

// Deleter as a FUNCTION POINTER: flexible (chosen at run time) but costs one extra pointer of storage.
using ZctxPtr = std::unique_ptr<zctx, void (*)(zctx*)>;

int main() {
    static_assert(sizeof(FilePtr) == sizeof(std::FILE*), "stateless deleter adds no size");
    static_assert(sizeof(ZctxPtr) == 2 * sizeof(void*), "function-pointer deleter is stored inside");

    if (FilePtr f = open_file("/tmp/m05_demo.txt", "w")) {   // contextual bool: "is it non-null?"
        std::fputs("hello\n", f.get());                       // .get() hands the raw pointer to C code
    }   // fclose here, on every path out of the if-block

    MallocPtr<char> buf(static_cast<char*>(std::malloc(64)));
    std::strcpy(buf.get(), "owned by unique_ptr");
    std::cout << buf.get() << '\n';

    ZctxPtr z(zctx_new(6), &zctx_free);
    std::cout << "level " << z->level << '\n';   // operator-> works like a raw pointer

    ZctxPtr z2 = std::move(z);                   // ownership transfer; z is now null
    std::cout << "z null? " << (z == nullptr) << '\n';

    zctx* raw = z2.release();                    // hand ownership back to C code that frees it itself
    zctx_free(raw);                              // z2 is null now and will NOT free again

    auto arr = std::make_unique<int[]>(8);       // array form: calls delete[] on destruction
    arr[7] = 42;
    std::cout << arr[7] << '\n';

    std::remove("/tmp/m05_demo.txt");
    return 0;
}   // buf -> free. z, z2 are null -> nothing. arr -> delete[].
