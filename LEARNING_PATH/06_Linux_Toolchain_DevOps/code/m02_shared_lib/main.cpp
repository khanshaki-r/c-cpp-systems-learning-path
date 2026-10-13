// m02_shared_lib/main.cpp - consumer of the shared library.
// Nothing in the source says "shared": that decision is made entirely at link and load time.
#include <cstdio>

#include "mathlib.h"

int main() {
    std::printf("add=%d mul=%d\n", mathlib::add(2, 3), mathlib::mul(4, 5));
    return 0;
}
