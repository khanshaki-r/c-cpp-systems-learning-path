# 05. Testing with GoogleTest and ctest

**Time:** ~2.5 hours   **Prerequisites:** 04

## 1. In plain words

A unit test is a small program that calls one piece of your code with known inputs and checks the
outputs. Hundreds of them run in a second. They are the difference between "I think the parser
handles CRLF" and "the parser handles CRLF, here is the test, it has run 4,000 times in CI". When
you refactor the store from one mutex to sharded locks, the tests tell you in two seconds whether
you broke anything.

GoogleTest is the de facto C++ unit test framework: macros that define tests, assertions that
compare values and print both sides on failure, fixtures for shared setup, and a runner. gMock, in
the same package, creates fake implementations of interfaces so you can test code that talks to a
database or socket without a database or socket. ctest is CMake's test driver: it knows about every
test, runs them in parallel, and gives CI a single pass/fail exit code.

Analogy: your banking server's acceptance environment runs end-to-end scenarios overnight. Unit
tests are the two-second version that runs every time you save: they do not replace the overnight
run, they make it rarely fail.

## 2. The real thing

### 2.1 What to test, and what not

Test **pure logic** with many edge cases: the protocol parser (every malformed line), the store
(overwrite, delete-missing, empty value, large value), any serialisation, any arithmetic with
boundaries. These are cheap, deterministic, and where the bugs are.

Test **behaviour, not implementation**: assert that `get` after `set` returns the value, not that
the hash map has one bucket occupied. Tests that mirror the implementation break on every refactor
and catch nothing.

Do **not** unit test the socket accept loop with real sockets in the same binary as the parser
tests. That is an integration test: slower, flaky under load, needs ports. Keep it separate
(a ctest with a label `integration`, run less often) or drive it from the Python client (module 10).

Design for testability: the reason `parse_command` takes a `string_view` and returns a struct,
instead of reading from a socket and writing to one, is so the test can call it directly. Functions
that do I/O should be thin wrappers around functions that do not.

### 2.2 GoogleTest basics

```cpp
#include <gtest/gtest.h>

TEST(SuiteName, TestName) {            // SuiteName groups related tests; both are identifiers
    EXPECT_EQ(2 + 2, 4);               // on failure: records, prints both values, CONTINUES
    ASSERT_TRUE(ptr != nullptr);       // on failure: records and RETURNS from this test function
    EXPECT_EQ(ptr->x, 1);              // safe: we would not be here if ptr were null
}
```

**EXPECT vs ASSERT:** `EXPECT_*` lets one test report several failures, which is what you want
when checking five fields of a result. `ASSERT_*` stops the test when continuing would crash or
be meaningless (null pointer, empty optional, failed parse). `ASSERT_*` works by `return`, so it
can only be used in functions returning `void`; in a helper that returns a value, use `EXPECT` or
restructure.

Assertion families:

| Assertion | Checks |
|---|---|
| `EXPECT_TRUE(c)`, `EXPECT_FALSE(c)` | boolean |
| `EXPECT_EQ(a,b)`, `NE`, `LT`, `LE`, `GT`, `GE` | `==`, `!=`, `<`, ... with both values printed |
| `EXPECT_STREQ(a,b)`, `STRNE`, `STRCASEEQ` | C strings by content (`EQ` on `char*` compares pointers) |
| `EXPECT_FLOAT_EQ`, `DOUBLE_EQ`, `NEAR(a,b,eps)` | floating point within 4 ULPs / tolerance |
| `EXPECT_THROW(stmt, Type)`, `ANY_THROW`, `NO_THROW` | exceptions |
| `EXPECT_THAT(value, matcher)` | gMock matchers: `Eq`, `HasSubstr`, `ElementsAre`, `Contains`, `SizeIs`, `AllOf`... |
| `EXPECT_DEATH(stmt, regex)` | statement kills the process (assert, abort) with matching stderr |
| `FAIL()`, `ADD_FAILURE()`, `SUCCEED()` | explicit |

Every assertion accepts a streamed message: `ASSERT_TRUE(r.ok()) << "parse error: " << r.error;`.
On failure the output is:

```
tests/test_protocol.cpp:31: Failure
Expected equality of these values:
  r.command->key
    Which is: "user:42"
  "user:43"
```

Both expressions and both values. This is why `EXPECT_EQ(a, b)` beats `EXPECT_TRUE(a == b)`.
Custom types need `operator<<` or a `PrintTo` overload to print nicely; otherwise you get bytes.

### 2.3 Fixtures

A fixture is a class deriving from `::testing::Test`; each `TEST_F` gets a **fresh instance**.
`SetUp()` runs before the body, `TearDown()` after (also on failure). Use the constructor and
destructor for things that cannot fail; use `SetUp`/`TearDown` when you need assertions in them.

```cpp
class StoreTest : public ::testing::Test {
protected:
    void SetUp() override { store_.set("a", "1"); store_.set("b", "2"); }
    tcpkv::Store store_;
};
TEST_F(StoreTest, DelReturnsTrueOnlyWhenKeyExisted) {
    EXPECT_TRUE(store_.del("a"));
    EXPECT_FALSE(store_.del("a"));
}
```

Isolation per test is the point: a test cannot pass because of what a previous test left behind,
and `--gtest_shuffle` can run them in random order to prove it. Expensive shared state (a database
connection) goes in `static void SetUpTestSuite()` / `TearDownTestSuite()`, once per suite.

### 2.4 Parameterised tests

When the same check applies to many inputs, write it once and feed a table:

```cpp
class ParseCommandRejects : public ::testing::TestWithParam<BadInput> {};
TEST_P(ParseCommandRejects, ReturnsExpectedError) {
    const BadInput& in = GetParam();
    const auto r = parse_command(in.line);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, in.expected_error);
}
INSTANTIATE_TEST_SUITE_P(MalformedLines, ParseCommandRejects,
    ::testing::Values(BadInput{"", "empty command"}, BadInput{"FLY", "unknown command"}, ...));
```

Each value becomes its own test: `MalformedLines/ParseCommandRejects.ReturnsExpectedError/3`. A
failure names the index and, with an `operator<<` on the param type, prints the input. Generators:
`Values(...)`, `ValuesIn(container)`, `Range(begin, end, step)`, `Bool()`, `Combine(g1, g2)` for a
cross product. **Typed** tests (`TYPED_TEST`) run the same body against several types, useful for
testing a template against `int`, `std::string`, a custom type.

### 2.5 Death tests

A death test asserts that code terminates the process: a failed `assert`, an `abort()`, a
`std::terminate` from `noexcept`. Use them to verify precondition checks fire.

```cpp
TEST(StoreDeathTest, NullBufferAborts) {
    EXPECT_DEATH(copy_into(nullptr, 10), "buffer must not be null");
}
```

Mechanism: GoogleTest forks; the child runs the statement; the parent checks the child died and
that stderr matched the regex. Name the suite `*DeathTest` so GoogleTest runs them first (before
threads exist; forking a multithreaded process is unsafe, see module 11). Under
`--gtest_death_test_style=threadsafe` the child re-executes the binary to run only that test, which
is slower but safe with threads. Death tests do not work under some sanitizers' leak detection
without `ASAN_OPTIONS=detect_leaks=0` in the child; the tcpkv asan preset sets `halt_on_error`
which is compatible.

### 2.6 Running tests

```bash
./build/debug/tests/tcpkv_tests                       # all
./build/debug/tests/tcpkv_tests --gtest_filter='ParseCommand.*'       # suite
./build/debug/tests/tcpkv_tests --gtest_filter='*Reject*:-*Length*'   # include : exclude
./build/debug/tests/tcpkv_tests --gtest_repeat=100 --gtest_shuffle    # flakiness hunt
./build/debug/tests/tcpkv_tests --gtest_break_on_failure              # SIGTRAP into gdb
./build/debug/tests/tcpkv_tests --gtest_list_tests
./build/debug/tests/tcpkv_tests --gtest_output=xml:report.xml         # JUnit XML for CI
gdb --args ./build/debug/tests/tcpkv_tests --gtest_filter='StoreTest.Del*' --gtest_break_on_failure
```

### 2.7 gMock introduction

A mock is a generated implementation of an interface where the test says which calls to expect,
what to return, and how many times. It lets you test a class that depends on something slow,
external or non-deterministic (disk, network, clock, random, PostgreSQL).

```cpp
class WriteAheadLog {
public:
    virtual ~WriteAheadLog() = default;
    virtual bool append(std::string_view key, std::string_view value) = 0;
    virtual void sync() = 0;
};
class MockWal : public WriteAheadLog {
public:
    MOCK_METHOD(bool, append, (std::string_view, std::string_view), (override));
    MOCK_METHOD(void, sync, (), (override));
};

TEST(PersistentStore, FailedLogAppendLeavesStoreUnchanged) {
    StrictMock<MockWal> wal;                              // unexpected calls fail the test
    PersistentStore ps(wal);
    EXPECT_CALL(wal, append(_, _)).WillOnce(Return(false));
    EXPECT_FALSE(ps.set("k", "v"));
    EXPECT_FALSE(ps.get("k").has_value());
}
```

Vocabulary: `EXPECT_CALL(mock, method(matchers))` with `.Times(n)`, `.WillOnce(action)`,
`.WillRepeatedly(action)`, `.InSequence(seq)`. Matchers: `_` (anything), `Eq(v)`, `Gt(v)`,
`HasSubstr(s)`, `Pointee(m)`. Actions: `Return(v)`, `ReturnRef(r)`, `Throw(e)`, `Invoke(fn)`,
`SaveArg<N>(&var)`. `NiceMock` ignores unexpected calls, `StrictMock` fails on them, plain mock warns.
`ON_CALL` sets a default behaviour without expecting the call.

When **not** to mock: do not mock the thing under test; do not mock value types; do not mock
`std::unordered_map`. Mock at architectural boundaries (I/O, time, external services). Mocks need a
virtual interface or a template parameter; that is a design cost, paid only where testing needs it.
Full example: `code/tcpkv/tests/test_gmock_example.cpp`.

### 2.8 ctest integration

```cmake
enable_testing()                        # top-level CMakeLists.txt
include(GoogleTest)
gtest_discover_tests(tcpkv_tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "unit")
```

`gtest_discover_tests` runs the binary with `--gtest_list_tests` after build and writes one ctest
entry per test. Then:

```bash
ctest --test-dir build/debug                      # run all; one line per test
ctest --test-dir build/debug -j8 --output-on-failure
ctest --test-dir build/debug -R 'StoreTest'       # regex on test name
ctest --test-dir build/debug -L unit              # by label
ctest --test-dir build/debug --rerun-failed
ctest --test-dir build/debug -N                   # list without running
ctest --preset asan                               # with the preset's environment (ASAN_OPTIONS)
```

ctest exits non-zero if any test failed. That exit code is what CI consumes. For integration tests
that need a running server, `add_test(NAME integration COMMAND python3 scripts/tcpkv_client.py
--selftest)` with `set_tests_properties(integration PROPERTIES LABELS integration FIXTURES_REQUIRED
server)` and a fixture test that starts the server; or simply run it as a separate CI step.

### 2.9 Sanitizers in tests

The test binary is the ideal sanitizer host: it exercises the code with many inputs, it is already
in CI, and it runs for seconds. The tcpkv presets `asan` and `tsan` build the library **and** the
tests with the sanitizer (both must be instrumented), and the test presets set `ASAN_OPTIONS` so a
report fails the run:

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R Concurrency
```

Under TSan, `StoreConcurrency.ReadersAndWritersOnSameKeySeeOnlyValidValues` becomes a proof that
`Store` has no data race. Remove the `lock_guard` from `Store::get` and run it: TSan prints the two
stacks (the write in `set`, the read in `get`) and the test fails. A test that passes without TSan
and fails with it is exactly what the TSan job in CI is for (module 09).

### 2.10 Test naming and organisation

- One test file per production file: `src/protocol.cpp` -> `tests/test_protocol.cpp`.
- Suite = the unit (`ParseCommand`, `StoreTest`), test = behaviour under a condition:
  `GetReturnsNulloptForMissingKey`, `TrailingCarriageReturnIsStripped`. The ctest output then reads
  as a specification.
- Arrange, act, assert: set up inputs, call once, check. A test that calls three functions and
  asserts after each is three tests.
- One concept per test; several `EXPECT`s on the fields of one result are fine.
- No logic in tests (loops, conditionals). If you need a loop, you want a parameterised test.
- Tests are production code: same warnings, same sanitizers, same review.

### 2.11 What to test in a server like tcpkv

| Layer | Test type | Examples |
|---|---|---|
| Protocol parser | unit, parameterised | every verb, CRLF, limits, every malformed shape, round trip |
| Store | unit, fixture, TSan | set/get/del/clear, overwrite, empty value, concurrency |
| Connection handler | unit with injected fake socket, or integration | partial lines across `recv`, pipelined commands in one `recv`, oversize line disconnect |
| Server lifecycle | integration (Python client) | start, PING, SET/GET, SIGTERM drains, exit 0 |
| Persistence (future) | gMock for the log, integration with a temp dir | append-before-store, fsync on checkpoint, recovery |

The connection handler in `main.cpp` reads from a file descriptor. To unit test the line-splitting
logic, factor it into a class that takes bytes and emits lines (`LineReader::feed(string_view) ->
vector<string>`), test that, and let `serve_client` call it. That refactor is exercise 5.

## 3. Code

- `code/tcpkv/tests/test_protocol.cpp`: plain tests, two parameterised suites, formatter tests.
- `code/tcpkv/tests/test_store.cpp`: fixture, concurrency tests meant for TSan.
- `code/tcpkv/tests/test_gmock_example.cpp`: interface, mock, `StrictMock`, `EXPECT_CALL`.
- `code/tcpkv/tests/CMakeLists.txt`: `find_package(GTest)` with FetchContent fallback,
  `gtest_discover_tests`.

Run the whole cycle:

```bash
cd code/tcpkv
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
# expect: 100% tests passed, N tests
./build/debug/tests/tcpkv_tests --gtest_filter='MalformedLines/*' --gtest_brief=1
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R Concurrency
```

Break something on purpose and watch it fail:

```bash
sed -i 's/if (!s.empty() \&\& s.back() == .\\r.) s.remove_suffix(1);/\/\/ removed/' src/protocol.cpp
cmake --build --preset debug && ctest --preset debug     # TrailingCarriageReturnIsStripped fails
git checkout src/protocol.cpp
```

## 4. Under the hood (senior-level)

**How `TEST` works.** The macro expands to a class `SuiteName_TestName_Test` deriving from
`::testing::Test` with a `TestBody()` override, plus a static `TestInfo*` initialised by calling
`MakeAndRegisterTestInfo(...)` at static-initialisation time. That registration is why tests in a
static library are **silently dropped**: the linker does not pull an archive member that nothing
references (module 02). Rule: tests are compiled directly into the test executable, or linked with
`--whole-archive`, or use object libraries.

**`gtest_main`** is a library containing `int main(int argc, char** argv) { InitGoogleTest(&argc,
argv); return RUN_ALL_TESTS(); }`. `RUN_ALL_TESTS` iterates registered suites, for each test
constructs the fixture, runs `SetUp`, `TestBody`, `TearDown`, destroys it, and collects results.
Return value is 0 iff all passed. If you need your own `main` (global environment, custom flags),
link `GTest::gtest` instead.

**Why EXPECT prints both expressions.** `EXPECT_EQ(a, b)` expands to a call to
`EqHelper::Compare("a", "b", a, b)` with the stringified expressions, so the failure message can
show the source text and the runtime values. The comparison is done with `operator==` on the
deduced types; mixing signed/unsigned can trigger `-Wsign-compare` inside the template, which is
why `EXPECT_EQ(v.size(), 2u)` and not `2`.

**Death tests fork.** In "fast" style the child continues from the fork point inside the test
body, so any threads, locks or buffered output in the parent are duplicated or lost. In
"threadsafe" style the child `exec`s the test binary with a filter for that one test, replaying up
to the death statement. That is why `DeathTest` suites run first and why you should not have
threads alive before them.

**`gtest_discover_tests` has a build-time cost.** It adds a POST_BUILD step running the binary;
if the binary needs a display, a device or a long static init, discovery can fail or time out
(`DISCOVERY_TIMEOUT`). `DISCOVERY_MODE PRE_TEST` defers discovery to ctest run time.

**Sanitizers and GoogleTest.** ASan's leak checker runs at process exit, after `RUN_ALL_TESTS`
returned; a leak therefore fails the binary, and ctest sees the non-zero exit even though every
test printed `[ OK ]`. That is correct behaviour and a reason to read the whole log. TSan needs
`-fsanitize=thread` on gtest itself too; FetchContent-built gtest inherits your flags only if you
set them globally or through `CMAKE_CXX_FLAGS` for the fetched target, which is why the presets
set `TCPKV_SANITIZE` and the sanitizer helper applies to your targets; gtest's own code is not
instrumented, which is fine because TSan instruments the code where the race happens (yours).
For ASan, uninstrumented gtest means ASan cannot see overflows **inside** gtest, which you do not
care about.

## 5. In production

- **Tests are what make `-Werror` and refactoring affordable.** Without them, every warning fix
  is a risk; with them, you fix 200 warnings in an afternoon and know nothing broke.
- **Flaky tests destroy trust.** A test that fails 1 in 50 runs is ignored, then a real failure is
  ignored. Hunt flakes with `--gtest_repeat=1000 --gtest_shuffle` and TSan. Common causes:
  shared static state, time-based assertions, real sockets with fixed ports, thread scheduling
  assumptions.
- **Test the error paths.** Production code is 30% happy path and 70% error handling; most test
  suites are the inverse. Every `return fail(...)` in the parser has a test. Every `errno` branch in
  the server should be reachable from a test with an injected fake descriptor.
- **Regression tests for every bug.** When a core dump leads to a root cause (05_Memory_Debugging_RCA),
  the fix commit includes a test that fails before and passes after. The test is the durable
  record of the incident.
- **In your banking server:** the RPC message codec and the transaction state machine are pure
  logic and should have hundreds of unit tests. The Berkeley DB adapter should be behind an
  interface so the migration logic can be tested with a mock, including the "DB returns
  DB_LOCK_DEADLOCK on the third call" path that only ever happens in production at month-end.
- **3 AM:** CI is green but production corrupts a record. The test suite had `EXPECT_TRUE(result
  == expected)` on a struct with a buggy `operator==` that ignored one field. Lesson: test the
  comparison operators too, and prefer field-by-field `EXPECT_EQ` for important types.

## 6. Try it

1. **(easy)** Add a test for `SET` with a key containing `:` and `-` characters (valid) and one with
   a tab (invalid). Done when both pass and you can show them in `ctest -N` output.
2. **(easy)** Run the suite with `--gtest_shuffle --gtest_repeat=50`. Then introduce a static
   variable shared between two tests so one depends on the other, and show that shuffle catches it.
   Revert. Done when you can explain why per-test fixture instances matter.
3. **(medium)** Write a death test: add `void Store::require_key(std::string_view key)` that
   `abort()`s with a message on an empty key, call it from `set`, and assert with `EXPECT_DEATH`.
   Done when the test passes in the `debug` preset. Then decide whether aborting is the right
   production behaviour for a server and write your answer in a comment.
4. **(medium)** Remove the `lock_guard` from `Store::get`, build the `tsan` preset and run
   `ctest --preset tsan -R Concurrency`. Read the report: identify the two stacks and the
   two threads. Restore the lock. Done when you can describe the report format from memory.
5. **(hard)** Extract a `LineReader` class from `serve_client`: `std::vector<std::string>
   feed(std::string_view bytes)` returning complete lines and keeping the partial tail, with an
   overflow flag when the tail exceeds `kMaxLineLength`. Write tests for: a line split across
   three feeds, two lines in one feed, CRLF, and overflow. Make `serve_client` use it. Done when
   all tests pass under `asan` and the server still works with `nc localhost 7000`.
6. **(hard)** Add an integration test: `add_test(NAME integration COMMAND python3
   ${CMAKE_SOURCE_DIR}/scripts/tcpkv_client.py --selftest --port 7123)` using ctest
   `FIXTURES_SETUP`/`FIXTURES_CLEANUP` tests that start and stop the server binary. Label it
   `integration` and make `ctest -L unit` skip it. Done when `ctest -L integration` passes and
   `ctest -L unit` finishes in under two seconds.

## 7. Interview questions

**Q: EXPECT vs ASSERT in GoogleTest?**
`EXPECT_*` records a failure and continues, so one run reports every broken field. `ASSERT_*`
records and returns from the test function, for cases where continuing would dereference null or
otherwise make the rest meaningless. `ASSERT` only works in `void` functions because it uses
`return`.

**Q: What is a fixture and why does GoogleTest create a new one per test?**
A class with shared setup and members used by several `TEST_F`s. A fresh instance per test
guarantees isolation: no test can depend on another's leftovers, so tests can run in any order
or in parallel, and a failure points at one test's own logic.

**Q: When would you use a parameterised test?**
When one behaviour must hold for many inputs: a parser against a table of malformed lines. One
body, a table of values, each value reported as its own test with its index and printed input.
It replaces a loop in a test, which would stop at the first failure and hide the rest.

**Q: What is a mock and where does it belong?**
A generated implementation of an interface where the test specifies expected calls, return values
and call counts. It belongs at architectural boundaries, such as disk, network, clock, external
services, so the unit under test can be exercised without them. Mocking value types or the unit
under test is a smell.

**Q: How do sanitizers fit into a test strategy?**
Build the library and the tests with `-fsanitize=address,undefined` in one configuration and
`thread` in another; run the same suite under each in CI. The suite already exercises edge cases,
so sanitizers turn it into a memory-safety and race detector at no extra test-writing cost.

**Q: Why might tests placed in a static library never run?**
GoogleTest registers tests via static initialisers. The linker only pulls archive members that
resolve an undefined symbol; nothing references the test objects, so they are dropped and their
initialisers never run. Compile tests into the executable, or use `--whole-archive`.

**Q: How does ctest relate to GoogleTest?**
GoogleTest is the framework inside the test binary. ctest is CMake's runner that knows about test
binaries, runs them in parallel with timeouts, labels and filters, and returns one exit code.
`gtest_discover_tests` bridges them by registering each GoogleTest case as a ctest entry.

## 8. Recap

- Test pure logic exhaustively (parser, store); keep I/O behind thin wrappers so it can be injected or integration-tested.
- `TEST`, `TEST_F` (fixture, fresh per test), `TEST_P` (parameterised table), `EXPECT_DEATH`.
- `EXPECT` continues, `ASSERT` returns; stream a message with `<<`.
- `EXPECT_EQ(a, b)` prints both sides; give custom types an `operator<<`.
- gMock: interface + `MOCK_METHOD` + `EXPECT_CALL(...).Times/WillOnce`; `StrictMock` for exactness.
- `gtest_discover_tests` gives one ctest entry per case; `ctest --preset X -j --output-on-failure`.
- Run the same suite under `asan` and `tsan` presets; the sanitizer failures are real bugs.
- Name tests as specification sentences; one behaviour per test; no logic in tests.
- Tests in static libraries vanish at link; compile them into the executable.
- Every bug fix ships with a regression test.
