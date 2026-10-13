// m03_flags_demo.cpp - watch optimisation levels and warning flags change the result.
//
//   g++ -std=c++17 -O0 -S -o - m03_flags_demo.cpp | grep -A20 '_Z6sum_tom:'   -> a real loop
//   g++ -std=c++17 -O2 -S -o - m03_flags_demo.cpp | grep -A20 '_Z6sum_tom:'   -> closed form, no loop
//   g++ -std=c++17 -Wall -Wextra          -c m03_flags_demo.cpp                 -> compiles clean
//   g++ -std=c++17 -Wall -Wextra -Wshadow -c m03_flags_demo.cpp                 -> shadow warning
//
// Build line that the style guide requires must pass, so the shadowing lives behind a macro:
//   g++ -std=c++17 -Wall -Wextra -Werror -pthread m03_flags_demo.cpp            -> OK
//   g++ -std=c++17 -Wall -Wextra -Wshadow -DSHOW_SHADOW_BUG m03_flags_demo.cpp  -> warning
#include <cstdint>
#include <cstdio>

// A loop the optimiser turns into n*(n+1)/2 at -O2 (induction variable elimination).
std::uint64_t sum_to(std::uint64_t n) {
    std::uint64_t s = 0;
    for (std::uint64_t i = 1; i <= n; ++i) s += i;
    return s;
}

// The classic shadowing bug: the inner 'total' hides the outer one, so the function returns 0.
int shadow_demo(int x) {
    int total = 0;
#ifdef SHOW_SHADOW_BUG
    if (x > 0) {
        int total = x * 2;     // -Wshadow: declaration of 'total' shadows a previous local
        (void)total;
    }
#else
    if (x > 0) total = x * 2;  // what the author meant
#endif
    return total;
}

// -Wconversion would warn on `return v;` here (int -> short may lose data).
// An explicit static_cast says "I know" and silences it; the truncation still happens.
short narrow(int v) { return static_cast<short>(v); }

int main() {
    std::printf("sum_to(100000) = %llu\n", static_cast<unsigned long long>(sum_to(100000)));
    std::printf("shadow_demo(5) = %d\n", shadow_demo(5));
    std::printf("narrow(70000)  = %d (wrapped)\n", narrow(70000));
    return 0;
}
