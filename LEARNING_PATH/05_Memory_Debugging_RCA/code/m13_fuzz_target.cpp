// m13_fuzz_target.cpp - a libFuzzer harness for a small request parser, plus a plain main() so
// the file also builds with g++ as an ordinary program.
//
// libFuzzer is a clang feature (not available in GCC). On WSL: sudo apt install clang
//
// Fuzz build:   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined
//                   -fno-sanitize-recover=all -DFUZZING -o build/m13_fuzz m13_fuzz_target.cpp
// Run:          mkdir -p corpus && printf 'D:ACC-1:100\n' > corpus/seed1
//               ./build/m13_fuzz corpus -max_len=256 -timeout=5
//               Within seconds it finds the off-by-one in parse_request and prints a UBSan
//               "index 12 out of bounds for type 'char[12]'" report plus a "crash-<sha1>" file.
//               NOT an ASan report: the stray byte lands in struct padding inside the same
//               allocation, which ASan cannot see. Module 13 section 3.2 explains why.
// Reproduce:    ./build/m13_fuzz crash-<sha1>
//
// Plain build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 -o build/m13_fuzz_plain m13_fuzz_target.cpp
//               ./build/m13_fuzz_plain            (runs a few hand-written inputs)
//
// Warnings: none.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The code under test. Wire format: "<kind>:<account-id>:<amount>\n"
struct Request { char kind; char account[12]; long amount; };

static bool parse_request(const uint8_t *data, size_t size, Request &out)
{
    if (size < 3 || data[1] != ':') return false;
    out.kind = static_cast<char>(data[0]);

    size_t i = 2, j = 0;
    while (i < size && data[i] != ':') {
        if (j > sizeof out.account) return false;     // BUG: should be j >= sizeof(account) - 1
        out.account[j++] = static_cast<char>(data[i++]);
    }
    if (i >= size) return false;
    out.account[j] = '\0';                            // writes one past the end when j == 12

    long amount = 0; ++i;
    while (i < size && data[i] >= '0' && data[i] <= '9') amount = amount * 10 + (data[i++] - '0');
    out.amount = amount;
    return true;
}

// libFuzzer entry point: called millions of times with mutated inputs.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Request r{};
    parse_request(data, size, r);
    return 0;
}

#ifndef FUZZING
int main()
{
    const char *samples[] = { "D:ACC-1:100\n", "C:ACC-22:5\n", "X:bad", "D:ACCOUNT-TOO-LONG-1:1\n" };
    for (const char *s : samples) {
        Request r{};
        bool ok = parse_request(reinterpret_cast<const uint8_t *>(s), std::strlen(s), r);
        std::printf("%-28s -> %s kind=%c account=%s amount=%ld\n", s[0] ? s : "(empty)",
                    ok ? "ok " : "bad", ok ? r.kind : '-', ok ? r.account : "-", ok ? r.amount : 0L);
    }
    return 0;
}
#endif
