# Step 01. Project skeleton, CMake, CI with a hello test

**Time:** ~5 hours   **Week:** 1 (Oct 6 – Oct 12)
**Read first:** 06_Linux_Toolchain_DevOps (CMake basics, Ninja, GitHub Actions, git workflow).
Nothing from the C++ courses is needed yet.

## 1. Goal

At the end of this step there is a public GitHub repository with a CMake project that builds
one GoogleTest binary, a `CMakePresets.json` so every build is one command, and a GitHub
Actions workflow that turns green. No tcpkv code yet. The point is that from the very first
commit the repo has the shape and the tooling of the finished product, so every later step
is "add a file and a test", never "set up infrastructure".

In plain words: you are building the factory before the first car.

## 2. Set up the machine (WSL Ubuntu 24.04)

```bash
sudo apt update
sudo apt install -y build-essential g++ cmake ninja-build git valgrind clang-format gdb python3
g++ --version      # 13.x
cmake --version    # 3.28.x
ninja --version    # 1.11.x
```

Configure git once:

```bash
git config --global user.name "Shakir"
git config --global user.email "<the email on your GitHub account>"
git config --global init.defaultBranch main
```

Create the repository on GitHub first (web UI): name `tcpkv`, **public**, no README, no
licence, no .gitignore (we add our own). Then:

```bash
mkdir -p ~/src/tcpkv && cd ~/src/tcpkv
git init
git remote add origin git@github.com:khanshaki-r/tcpkv.git
```

Set up an SSH key if you have not (`ssh-keygen -t ed25519`, add the public key in GitHub
settings). HTTPS with a token also works.

## 3. Files to create

### `CMakeLists.txt`

This is the step-01 version. Later steps add the library and executables; the structure stays.

```cmake
cmake_minimum_required(VERSION 3.22)

project(tcpkv
    VERSION 0.1.0
    DESCRIPTION "Multithreaded TCP key-value server"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------------------------
# Language and build type
# ---------------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++17, not -std=gnu++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # compile_commands.json for clangd / clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(TCPKV_BUILD_TESTS "Build the GoogleTest suite" ON)
set(TCPKV_SANITIZER "" CACHE STRING "Sanitizer to enable: address | thread | undefined | (empty)")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------------
# Warnings: one INTERFACE target so every binary gets exactly the same flags
# ---------------------------------------------------------------------------------------------
add_library(tcpkv_warnings INTERFACE)
target_compile_options(tcpkv_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Werror)

# ---------------------------------------------------------------------------------------------
# Sanitizers: applied to everything in this directory tree so library, tests and gtest agree
# ---------------------------------------------------------------------------------------------
if(TCPKV_SANITIZER)
    message(STATUS "tcpkv: building with -fsanitize=${TCPKV_SANITIZER}")
    add_compile_options(-fsanitize=${TCPKV_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TCPKV_SANITIZER})
endif()

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
if(TCPKV_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    add_executable(hello_test tests/hello_test.cpp)
    target_link_libraries(hello_test PRIVATE GTest::gtest_main tcpkv_warnings Threads::Threads)
    gtest_discover_tests(hello_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
endif()
```

Why each piece:

* `CMAKE_CXX_EXTENSIONS OFF` gives `-std=c++17` rather than `-std=gnu++17`. You want the
  standard language; the GNU extensions you need (like `pipe2`, `accept4`) come from glibc
  headers, which g++ enables anyway by defining `_GNU_SOURCE`.
* The warnings are on an INTERFACE library instead of `add_compile_options` so that
  *GoogleTest's own sources* are not built with `-Werror`. Third-party code you do not control
  must never be able to break your build.
* Sanitizers *are* applied globally (`add_compile_options`) because an instrumented test
  linked against an uninstrumented gtest, or vice versa, gives false positives.
* `FetchContent` with a pinned tag: reproducible builds, no system package needed, works
  identically in CI.
* `gtest_discover_tests` registers every `TEST()` as its own ctest entry, so a failure names
  the test, and ctest can run them in parallel.

### `CMakePresets.json`

This is the final version; it does not change later. A preset is a named bundle of
generator + build directory + cache variables, so "build with TSan" is one command for you
and for CI.

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    {
      "name": "debug",
      "displayName": "Debug",
      "inherits": "base",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    },
    {
      "name": "release",
      "displayName": "Release (-O3, no debug info)",
      "inherits": "base",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    },
    {
      "name": "relwithdebinfo",
      "displayName": "RelWithDebInfo (for Valgrind and perf)",
      "inherits": "base",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" }
    },
    {
      "name": "asan",
      "displayName": "AddressSanitizer + LeakSanitizer",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "TCPKV_SANITIZER": "address"
      }
    },
    {
      "name": "tsan",
      "displayName": "ThreadSanitizer",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "TCPKV_SANITIZER": "thread"
      }
    },
    {
      "name": "ubsan",
      "displayName": "UndefinedBehaviorSanitizer",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "TCPKV_SANITIZER": "undefined"
      }
    }
  ],
  "buildPresets": [
    { "name": "debug", "configurePreset": "debug" },
    { "name": "release", "configurePreset": "release" },
    { "name": "relwithdebinfo", "configurePreset": "relwithdebinfo" },
    { "name": "asan", "configurePreset": "asan" },
    { "name": "tsan", "configurePreset": "tsan" },
    { "name": "ubsan", "configurePreset": "ubsan" }
  ],
  "testPresets": [
    {
      "name": "base-test",
      "hidden": true,
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error", "stopOnFailure": false }
    },
    { "name": "debug", "inherits": "base-test", "configurePreset": "debug" },
    { "name": "release", "inherits": "base-test", "configurePreset": "release" },
    { "name": "relwithdebinfo", "inherits": "base-test", "configurePreset": "relwithdebinfo" },
    {
      "name": "asan",
      "inherits": "base-test",
      "configurePreset": "asan",
      "environment": { "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:strict_string_checks=1" }
    },
    {
      "name": "tsan",
      "inherits": "base-test",
      "configurePreset": "tsan",
      "environment": { "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1" }
    },
    {
      "name": "ubsan",
      "inherits": "base-test",
      "configurePreset": "ubsan",
      "environment": { "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1" }
    }
  ]
}
```

TSan uses `RelWithDebInfo` because TSan on an unoptimised build is painfully slow and the
compiler at `-O0` keeps values in memory in ways that produce no false positives but a lot of
noise in timing-sensitive tests.

### `tests/hello_test.cpp`

A throwaway. It exists so the whole chain (configure, fetch gtest, build, discover, run, CI)
is proven before any real code exists. Step 02 deletes it.

```cpp
#include <utility>

#include <gtest/gtest.h>

TEST(Hello, ToolchainWorks) {
    EXPECT_EQ(1 + 1, 2);
}

TEST(Hello, Cpp17IsEnabled) {
    // Structured bindings are C++17; this fails to compile on an older standard.
    const auto [a, b] = std::pair<int, int>{1, 2};
    EXPECT_EQ(a + b, 3);
}
```

### `.github/workflows/ci.yml`

Step-01 version: debug and asan only. Step 08 extends it to the full matrix plus Valgrind.

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  build-and-test:
    name: ${{ matrix.preset }}
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        preset: [debug, asan]
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends ninja-build g++

      - name: Configure
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        run: cmake --build --preset ${{ matrix.preset }} --parallel

      - name: Test
        run: ctest --preset ${{ matrix.preset }} --parallel 2
```

`ubuntu-24.04` is the same OS as your WSL, so "works on my machine" and "works in CI" mean
the same compiler and the same glibc.

### `.gitignore`

```gitignore
# Build trees (one per CMake preset)
build/
cmake-build-*/

# Editor / tooling
.cache/
.vscode/
.idea/
compile_commands.json
*.swp

# Valgrind, perf, core dumps
vgcore.*
perf.data*
core
core.*

# Python
__pycache__/
*.pyc
```

### `.clang-format`

```yaml
# Run: find include src apps tools tests -name '*.?pp' | xargs clang-format -i
BasedOnStyle: Google
Language: Cpp
Standard: c++17
IndentWidth: 4
ColumnLimit: 100
AccessModifierOffset: -4
DerivePointerAlignment: false
PointerAlignment: Left
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: WithoutElse
AllowShortCaseLabelsOnASingleLine: true
AllowShortLoopsOnASingleLine: false
BreakConstructorInitializers: BeforeColon
ConstructorInitializerAllOnOneLineOrOnePerLine: true
IncludeBlocks: Preserve
SortIncludes: CaseSensitive
IndentCaseLabels: true
NamespaceIndentation: None
FixNamespaceComments: true
SpacesBeforeTrailingComments: 2
```

Having a formatter from day one means no later commit is "reformat everything", and no code
review discussion is ever about braces.

### `LICENSE`

The MIT text, exactly as in `code/LICENSE`, with the line
`Copyright (c) 2026 Shakir (github.com/khanshaki-r)`. Use your legal name if you prefer.

### `README.md` (stub)

```markdown
# tcpkv

A multithreaded TCP key-value server in C++17. Work in progress; v1.0.0 is the first release.

## Build

    cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

## 4. Build and run

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Expected: the configure step clones googletest (one time, ~20 s), the build produces
`build/debug/hello_test`, and ctest prints `100% tests passed, 0 tests failed out of 2`.

Then prove the sanitizer path works too:

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## 5. Test

The test here is the pipeline itself:

1. Break something on purpose: change `EXPECT_EQ(1 + 1, 2)` to `3`, rebuild, run ctest.
   You must see the failure named `Hello.ToolchainWorks` with file and line. Revert.
2. Add an unused variable `int x;` to the test. The build must fail with
   `error: unused variable 'x' [-Werror=unused-variable]`. That is `-Werror` doing its job. Revert.

## 6. Commit and push

```bash
git add -A
git commit -m "Project skeleton: CMake, presets, GoogleTest via FetchContent, CI

- C++17, -Wall -Wextra -Wpedantic -Wshadow -Werror on a shared INTERFACE target
- CMakePresets for debug/release/relwithdebinfo/asan/tsan/ubsan
- GitHub Actions: debug and asan jobs on ubuntu-24.04
- hello_test proves the toolchain end to end"
git push -u origin main
```

Open `https://github.com/khanshaki-r/tcpkv/actions`. Wait for both jobs to go green. If a
job is red, read the log, fix, push again. Do not move to step 02 with a red CI; the habit
of "CI is always green on main" is the whole point of setting it up first.

## 7. What you learned, and what to say in an interview

* **CMake targets, not variables.** Flags live on targets (`tcpkv_warnings`) and propagate
  through `target_link_libraries`. That is how you keep `-Werror` off third-party code.
* **Presets** make "configure with TSan" a single reproducible command for humans and CI.
* **Sanitizers must be applied to everything** that ends up in the same process.
* **Pinned dependencies** (`GIT_TAG v1.14.0`) are what makes a build reproducible a year later.

Interview sentence: "I set the repo up so that from the first commit every build was
`-Werror` clean and every push ran the tests under AddressSanitizer in CI. Adding a feature
was always just adding a file and a test; the infrastructure never changed after week one."
