// m02_odr_violation/b.cpp
#include <cstdio>

#include "bad_header.h"

int read_counter() { return g_counter; }

int main() {
    bump();
    bump();
    std::printf("counter=%d\n", read_counter());   // expect 2 once it links
    return 0;
}
