// m04_tcpkv_parser_test.cpp - assert-based tests for the incremental tcpkv parser.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_tcpkv_parser_test.cpp -o parser_test
// Run:    ./parser_test            (prints "all tests passed" and exits 0)
//
// The important tests are the ones that feed the SAME bytes in DIFFERENT chunkings and demand the
// SAME parsed result. That is the property TCP forces on us.
#undef NDEBUG
#include "m04_tcpkv_parser.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace tcpkv;

static bool same(const Command& a, const Command& b) {
    return a.op == b.op && a.key == b.key && a.value == b.value;
}

// Feed `wire` to a fresh parser in the given chunk sizes and collect everything it emits.
struct Emitted {
    std::vector<Command> cmds;
    std::vector<ParseError> errs;
};

static Emitted run(const std::string& wire, const std::vector<size_t>& chunks) {
    Parser p;
    Emitted out;
    size_t off = 0;
    for (size_t c : chunks) {
        size_t n = std::min(c, wire.size() - off);
        p.feed(wire.data() + off, n);
        off += n;
        for (;;) {
            Command cmd;
            ParseError err;
            ParseStatus st = p.next(cmd, err);
            if (st == ParseStatus::NeedMore) break;
            if (st == ParseStatus::Ok) out.cmds.push_back(cmd);
            else {
                out.errs.push_back(err);
                if (err.fatal) return out;
            }
        }
    }
    return out;
}

static std::vector<size_t> whole(const std::string& w) { return {w.size()}; }
static std::vector<size_t> bytewise(const std::string& w) { return std::vector<size_t>(w.size(), 1); }

static void test_simple_commands() {
    std::string wire = "PING\r\nGET alpha\r\nDEL beta\r\nEXISTS gamma\r\nQUIT\r\n";
    for (const auto& chunks : {whole(wire), bytewise(wire)}) {
        Emitted e = run(wire, chunks);
        assert(e.errs.empty());
        assert(e.cmds.size() == 5);
        assert(e.cmds[0].op == Op::Ping);
        assert(e.cmds[1].op == Op::Get && e.cmds[1].key == "alpha");
        assert(e.cmds[2].op == Op::Del && e.cmds[2].key == "beta");
        assert(e.cmds[3].op == Op::Exists && e.cmds[3].key == "gamma");
        assert(e.cmds[4].op == Op::Quit);
    }
}

static void test_set_binary_payload() {
    // The payload contains CRLF, spaces and NULs: a delimiter-based protocol would choke here.
    std::string payload = std::string("a\r\nb c\0d", 8);
    Command c{Op::Set, "k1", payload};
    std::string wire = fmt_cmd(c) + "GET k1\r\n";
    // "SET k1 8\r\n" (10) + payload (8) + "\r\n" (2) + "GET k1\r\n" (8)
    assert(wire.size() == 28);
    for (const auto& chunks : {whole(wire), bytewise(wire), std::vector<size_t>{3, 5, 1, 100}}) {
        Emitted e = run(wire, chunks);
        assert(e.errs.empty());
        assert(e.cmds.size() == 2);
        assert(same(e.cmds[0], c));
        assert(e.cmds[1].op == Op::Get && e.cmds[1].key == "k1");
    }
}

static void test_empty_value() {
    std::string wire = "SET e 0\r\n\r\nEXISTS e\r\n";
    Emitted e = run(wire, bytewise(wire));
    assert(e.errs.empty());
    assert(e.cmds.size() == 2);
    assert(e.cmds[0].op == Op::Set && e.cmds[0].key == "e" && e.cmds[0].value.empty());
}

static void test_pipelining_in_one_chunk() {
    // Many requests arriving in one recv(): must all be parsed without more input.
    std::string wire;
    for (int i = 0; i < 1000; ++i) wire += "GET key" + std::to_string(i) + "\r\n";
    Emitted e = run(wire, whole(wire));
    assert(e.cmds.size() == 1000);
    assert(e.cmds[999].key == "key999");
}

static void test_recoverable_errors() {
    std::string wire = "FLY away\r\nGET\r\nGET a b\r\nSET k notanumber\r\nGET bad key\r\nPING extra\r\nPING\r\n";
    Emitted e = run(wire, bytewise(wire));
    assert(e.errs.size() == 6);
    assert(e.errs[0].code == "ERR_UNKNOWN_CMD" && !e.errs[0].fatal);
    assert(e.errs[1].code == "ERR_ARGS");
    assert(e.errs[2].code == "ERR_ARGS");
    assert(e.errs[3].code == "ERR_SYNTAX");
    assert(e.errs[4].code == "ERR_ARGS");
    assert(e.errs[5].code == "ERR_ARGS");
    // After six bad lines the stream recovers and the final PING parses.
    assert(e.cmds.size() == 1 && e.cmds[0].op == Op::Ping);
}

static void test_fatal_errors() {
    {
        // Line without CRLF longer than max_line => fatal.
        std::string wire(5000, 'A');
        Emitted e = run(wire, whole(wire));
        assert(e.errs.size() == 1 && e.errs[0].fatal && e.errs[0].code == "ERR_TOO_LARGE");
    }
    {
        // Declared length exceeds max_value => fatal before buffering anything.
        std::string wire = "SET k 9999999999\r\n";
        Emitted e = run(wire, whole(wire));
        assert(e.errs.size() == 1 && e.errs[0].fatal);
    }
    {
        // Payload not followed by CRLF => fatal (we lost sync).
        std::string wire = "SET k 3\r\nabcXXGET k\r\n";
        Emitted e = run(wire, whole(wire));
        assert(e.errs.size() == 1 && e.errs[0].fatal && e.errs[0].code == "ERR_SYNTAX");
        assert(e.cmds.empty());
    }
    {
        // Looks like HTTP: must be rejected, not crash.
        std::string wire = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        Emitted e = run(wire, whole(wire));
        assert(!e.errs.empty() && e.errs[0].code == "ERR_ARGS");
    }
}

static void test_key_validation() {
    std::string big(257, 'k');
    std::string wire = "GET " + big + "\r\nGET " + std::string(256, 'k') + "\r\nGET a\tb\r\n";
    Emitted e = run(wire, whole(wire));
    assert(e.errs.size() == 2);
    assert(e.cmds.size() == 1 && e.cmds[0].key.size() == 256);
}

// Fuzz 1: random valid command streams, random chunking. Parsed output must equal the input list.
static void fuzz_random_splits(unsigned seed, int rounds) {
    std::mt19937 rng(seed);
    for (int r = 0; r < rounds; ++r) {
        std::vector<Command> expected;
        std::string wire;
        int n = static_cast<int>(rng() % 50) + 1;
        for (int i = 0; i < n; ++i) {
            Command c;
            switch (rng() % 6) {
            case 0: c.op = Op::Ping; break;
            case 1: c.op = Op::Quit; break;
            case 2: c.op = Op::Get; break;
            case 3: c.op = Op::Del; break;
            case 4: c.op = Op::Exists; break;
            default: c.op = Op::Set; break;
            }
            if (c.op != Op::Ping && c.op != Op::Quit) {
                size_t klen = rng() % 40 + 1;
                for (size_t k = 0; k < klen; ++k) c.key += static_cast<char>('a' + rng() % 26);
            }
            if (c.op == Op::Set) {
                size_t vlen = rng() % 3000;
                for (size_t v = 0; v < vlen; ++v) c.value += static_cast<char>(rng() % 256);
            }
            wire += fmt_cmd(c);
            expected.push_back(c);
        }
        std::vector<size_t> chunks;
        size_t left = wire.size();
        while (left > 0) {
            size_t c = rng() % 7 == 0 ? 1 : rng() % 1500 + 1;
            if (c > left) c = left;
            chunks.push_back(c);
            left -= c;
        }
        Emitted e = run(wire, chunks);
        assert(e.errs.empty());
        assert(e.cmds.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i) assert(same(e.cmds[i], expected[i]));
    }
}

// Fuzz 2: random garbage. The parser may report errors, but must never crash, never loop
// forever, and never hold more than max_line + max_value bytes.
static void fuzz_garbage(unsigned seed, int rounds) {
    std::mt19937 rng(seed);
    const char alphabet[] = "GETSDLPINQUXIST \r\n0123456789abc";
    for (int r = 0; r < rounds; ++r) {
        Parser p(1024, 4096);
        size_t total = rng() % 20000;
        for (size_t i = 0; i < total;) {
            std::string chunk;
            size_t clen = rng() % 64 + 1;
            for (size_t j = 0; j < clen; ++j) chunk += alphabet[rng() % (sizeof alphabet - 1)];
            p.feed(chunk);
            i += clen;
            bool dead = false;
            for (int guard = 0; guard < 100000; ++guard) {
                Command cmd;
                ParseError err;
                ParseStatus st = p.next(cmd, err);
                if (st == ParseStatus::NeedMore) break;
                if (st == ParseStatus::Error && err.fatal) {
                    dead = true;
                    break;
                }
            }
            if (dead) break;
            assert(p.buffered() <= 1024 + 4096 + 2);
        }
    }
}

int main() {
    test_simple_commands();
    test_set_binary_payload();
    test_empty_value();
    test_pipelining_in_one_chunk();
    test_recoverable_errors();
    test_fatal_errors();
    test_key_validation();
    fuzz_random_splits(12345, 300);
    fuzz_garbage(777, 200);
    std::puts("all tests passed");
    return 0;
}
