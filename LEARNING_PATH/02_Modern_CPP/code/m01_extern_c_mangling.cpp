// m01_extern_c_mangling.cpp
// Name mangling and extern "C".
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m01_extern_c_mangling.cpp -o mangling
// Then:  nm mangling | grep -E 'area|checksum'       (raw symbol names)
//        nm -C mangling | grep -E 'area|checksum'    (demangled by nm, like c++filt)
// You will see _Z4areai and _Z4aread for the two C++ overloads and a plain c_area for the C one.
#include <cstdio>
#include <typeinfo>

int    area(int side)    { return side * side; }   // mangled: _Z4areai  (Itanium C++ ABI: _Z + length + name + param types)
double area(double side) { return side * side; }   // mangled: _Z4aread

extern "C" int c_area(int side) { return side * side; }   // NOT mangled: symbol is 'c_area'. Callable from C. No overloading allowed.

// A C header that must be includable from both C and C++ looks like this:
#ifdef __cplusplus
extern "C" {
#endif
int legacy_checksum(const char* data, int len);   // declared with C linkage
#ifdef __cplusplus
}
#endif

// The definition inherits C linkage from the declaration above.
int legacy_checksum(const char* data, int len) {
    int sum = 0;
    for (int i = 0; i < len; ++i) sum += data[i];
    return sum;
}

int main() {
    std::printf("%d %.1f %d\n", area(3), area(2.5), c_area(4));
    std::printf("checksum=%d\n", legacy_checksum("abc", 3));

    // typeid(T).name() returns the MANGLED type name on GCC: "i" for int, "d" for double,
    // "NSt7__cxx1112basic_string..." for std::string. Pipe it through c++filt -t to demangle.
    std::printf("typeid(int)=%s typeid(double)=%s\n", typeid(int).name(), typeid(double).name());
    return 0;
}
