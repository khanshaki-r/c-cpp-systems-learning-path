# 04. Make and CMake from scratch

**Time:** ~3 hours   **Prerequisites:** 02, 03

## 1. In plain words

A build system answers two questions: "which commands build this project?" and "which of those
commands need to run again after I change one file?". MSBuild answers both for you from a
`.vcxproj`. On Linux the traditional answer is **Make**: a file of rules "to make X from Y, run Z",
and Make runs only the rules whose inputs are newer than their outputs.

CMake is one level up. It does not build anything. It reads `CMakeLists.txt`, a description of your
**targets** (executables, libraries) and the relationships between them, and **generates** a native
build file: a Makefile, a Ninja file, a Visual Studio solution, an Xcode project. One description,
every platform. For your resume line "Builds clean under -Wall -Wextra -Werror with CMake", you need
to be able to write a CMakeLists.txt that a reviewer would accept: targets with correct visibility,
no global flag soup, tests wired in, presets for one-command builds.

Analogy: a Makefile is a recipe card. CMake is the cookbook author who writes the recipe card in
whatever format your kitchen uses.

## 2. The real thing

### 2.1 Why build systems exist

```bash
g++ -std=c++17 -c protocol.cpp -o protocol.o
g++ -std=c++17 -c store.cpp -o store.o
g++ -std=c++17 -c main.cpp -o main.o
g++ protocol.o store.o main.o -o tcpkv
```

Four commands. Change `store.cpp` and only two need to re-run. Change `store.hpp` and three do
(every TU that includes it). With 300 files this is unmanageable by hand, and the header dependency
tracking is the hard part. A build system records the dependency graph and does minimal rebuilds.

### 2.2 Make in ten lines

`code/Makefile`:

```makefile
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Werror -MMD -MP
LDFLAGS  :=
LDLIBS   := -pthread
SRCS     := $(wildcard src/*.cpp)
OBJS     := $(SRCS:src/%.cpp=build/%.o)

tcpkv: $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -Iinclude -c $< -o $@

build:
	mkdir -p build

-include $(OBJS:.o=.d)

.PHONY: clean
clean:
	rm -rf build tcpkv
```

Line by line:

- `VAR := value` assigns. `$(VAR)` expands. `:=` evaluates now; `=` evaluates at use (recursive).
- `$(wildcard src/*.cpp)` lists sources. `$(SRCS:src/%.cpp=build/%.o)` rewrites each path:
  `src/store.cpp` -> `build/store.o`.
- A rule is `target: prerequisites` then a **tab**-indented recipe. Spaces instead of a tab is the
  classic "missing separator" error.
- Make rebuilds a target if any prerequisite is newer than the target (by mtime) or the target
  does not exist.
- `$@` is the target, `$<` the first prerequisite, `$^` all prerequisites.
- `build/%.o: src/%.cpp` is a **pattern rule**; `%` matches the stem.
- `| build` is an **order-only** prerequisite: `build/` must exist, but its mtime changing does not
  trigger rebuilds (directory mtimes change whenever a file inside is created).
- `-MMD -MP` tells g++ to write `build/store.d` containing
  `build/store.o: src/store.cpp include/tcpkv/store.hpp ...`; `-include` pulls those in so header
  changes trigger rebuilds. Without this, editing a header rebuilds nothing. That is the single most
  common bug in hand-written Makefiles.
- `.PHONY` marks targets that are not files, so `make clean` works even if a file named `clean` exists.
- `make -j8` runs 8 recipes in parallel wherever the graph allows.

Run: `make`, `make -j$(nproc)`, `make clean`, `make -n` (dry run: print commands), `make -B`
(force rebuild), `make V=1` if the Makefile honours it, `make -p` (print the database).

Make is fine for a 20-file project and for learning. It is painful at scale: no portable way to
find libraries, no built-in test runner, no out-of-source build by default, platform conditionals
by hand, and link order mistakes are yours to make.

### 2.3 CMake: the model

CMake's vocabulary:

- **Target**: something that gets built (`add_executable`, `add_library`) or an abstraction
  (`INTERFACE` library, imported target like `Threads::Threads`).
- **Property**: a setting attached to a target: include directories, compile definitions, compile
  options, link libraries, C++ standard.
- **Usage requirement**: a property that a target **exports** to whatever links against it.
  This is the idea that makes modern CMake work: a library says "to use me you need these include
  dirs and these defines", and consumers inherit them automatically.
- **Generator**: the native build tool CMake writes files for: `Ninja`, `Unix Makefiles`,
  `Visual Studio 17 2022`.
- **Configure** (CMake runs, writes build files into the build dir) then **build** (the generator
  tool runs the compiler). CMake is not involved in the second step except to re-run itself if a
  `CMakeLists.txt` changed.

```
CMakeLists.txt  --[cmake -S . -B build -G Ninja]-->  build/build.ninja, build/CMakeCache.txt
                --[cmake --build build]----------->  ninja runs g++ ... -> build/tcpkv
                --[ctest --test-dir build]-------->  runs registered tests
```

### 2.4 Minimal executable

```cmake
cmake_minimum_required(VERSION 3.22)       # policy baseline; 3.22 ships with Ubuntu 22.04, 3.28 with 24.04
project(hello VERSION 0.1.0 LANGUAGES CXX) # no C: do not enable languages you do not use
add_executable(hello main.cpp)
target_compile_features(hello PUBLIC cxx_std_17)
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/hello
```

`-S` source dir, `-B` build dir. **Out-of-source**: everything CMake generates goes into `build/`;
your source tree stays clean; `rm -rf build` is a full clean; you can have `build-debug/`,
`build-asan/`, `build-release/` side by side from one source tree. Never run `cmake .` in the source
directory.

### 2.5 A library and an executable with correct visibility

```cmake
add_library(tcpkv_core src/protocol.cpp src/store.cpp)     # STATIC by default (BUILD_SHARED_LIBS flips it)
add_library(tcpkv::core ALIAS tcpkv_core)                 # namespaced alias: consumers use tcpkv::core

target_include_directories(tcpkv_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(tcpkv_core PUBLIC cxx_std_17)
target_compile_definitions(tcpkv_core PRIVATE TCPKV_INTERNAL=1)

find_package(Threads REQUIRED)
target_link_libraries(tcpkv_core PUBLIC Threads::Threads)

add_executable(tcpkv src/main.cpp)
target_link_libraries(tcpkv PRIVATE tcpkv::core)
```

**PUBLIC / PRIVATE / INTERFACE** decide who sees a requirement:

| Keyword | Applied to the target itself | Propagated to consumers | Use when |
|---|---|---|---|
| `PRIVATE` | yes | no | needed only to build this target's own sources |
| `PUBLIC` | yes | yes | appears in this target's public headers, so consumers need it too |
| `INTERFACE` | no | yes | header-only libraries; or requirement only consumers need |

Rule of thumb: ask "does my public header `#include` it or use its types?" If yes, `PUBLIC`.
If only my `.cpp` files use it, `PRIVATE`. The `include/` directory is `PUBLIC` because consumers
must find `tcpkv/store.hpp`. `src/` is `PRIVATE` because internal headers are not for consumers.
`Threads::Threads` is `PUBLIC` here because `store.hpp` has a `std::mutex` member, so any consumer
compiles code that touches pthread types.

Why this matters: with correct visibility, `target_link_libraries(tcpkv PRIVATE tcpkv::core)` is
the **only** line the executable needs. Include paths, `-pthread`, the C++17 requirement all arrive
transitively. Get it wrong (everything PRIVATE) and the executable fails to find headers; get it
wrong the other way (everything PUBLIC) and every consumer inherits your internal junk, and your
build graph becomes a hairball where touching one define rebuilds the world.

Generator expressions `$<...>` are evaluated at generate time, per configuration. `$<BUILD_INTERFACE:...>`
is used when building in this tree, `$<INSTALL_INTERFACE:...>` when a consumer uses the installed
package. `$<$<CONFIG:Debug>:-O0>` applies only in Debug. `$<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond>`
only for GCC.

### 2.6 Compiler flags the right way

Do **not** write `set(CMAKE_CXX_FLAGS "-Wall ...")`. That is a global string that affects every
target including third-party code pulled in by FetchContent, and it concatenates badly. Attach
options to targets, through a helper function so the list is defined once:

```cmake
# cmake/CompilerWarnings.cmake
function(tcpkv_set_warnings target)
    set(gcc_clang_warnings
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough)
    set(gcc_only_warnings
        -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)
    set(msvc_warnings /W4 /permissive- /w14640 /w14265 /w14062)

    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:${gcc_clang_warnings}>
        $<$<CXX_COMPILER_ID:GNU>:${gcc_only_warnings}>
        $<$<CXX_COMPILER_ID:MSVC>:${msvc_warnings}>)

    if(TCPKV_WERROR)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>)
    endif()
endfunction()
```

Warnings are `PRIVATE`: they are about how **I** compile, not a usage requirement. If they were
`PUBLIC`, consumers of your library would be forced to compile with your `-Werror`.

Options:

```cmake
option(TCPKV_WERROR      "Treat warnings as errors" ON)
option(TCPKV_BUILD_TESTS "Build unit tests" ON)
set(TCPKV_SANITIZE "" CACHE STRING "Sanitizer list, e.g. address,undefined or thread")
```

`option()` creates a boolean cache variable; `-DTCPKV_WERROR=OFF` on the command line overrides it.
Cache variables live in `build/CMakeCache.txt` and persist between configure runs; that is why
"I changed the flag and nothing happened" usually means "the cache still has the old value" and
`cmake -D...` or deleting the cache fixes it.

Sanitizers as a target option:

```cmake
function(tcpkv_set_sanitizers target)
    if(TCPKV_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${TCPKV_SANITIZE} -fno-omit-frame-pointer -g)
        target_link_options(${target}    PRIVATE -fsanitize=${TCPKV_SANITIZE})
    endif()
endfunction()
```

### 2.7 Build types

`CMAKE_BUILD_TYPE` for single-config generators (Ninja, Make): `Debug` (`-g`), `Release`
(`-O3 -DNDEBUG`), `RelWithDebInfo` (`-O2 -g -DNDEBUG`), `MinSizeRel` (`-Os -DNDEBUG`). If you
set nothing, you get **no flags at all**: not Debug, just empty. Always set one. `RelWithDebInfo`
is what you ship (module 03). For multi-config generators (Visual Studio) the type is chosen at
build time: `cmake --build build --config Release`.

Defaulting it:

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
```

### 2.8 Generator choice: Ninja

```bash
cmake -S . -B build -G Ninja
cmake -S . -B build -G "Unix Makefiles"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64    # on Windows
export CMAKE_GENERATOR=Ninja                              # make Ninja the default
```

Ninja is a small, fast build executor designed to be generated, not written. It starts faster than
Make on large trees (it loads one file instead of recursively running `make` in subdirectories), it is
parallel by default (`-j` = cores + 2), it only prints output of failed commands, and it tracks
header dependencies natively via the compiler's `-MD` output. There is no reason to use the
Makefile generator for new work. `ninja -C build -v` shows full commands; `ninja -C build -t
commands tcpkv` prints the commands for one target; `ninja -C build -d explain` says why things rebuilt.

### 2.9 `compile_commands.json`

```cmake
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

or `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Writes `build/compile_commands.json`: one entry per TU with
the exact command line. `clangd` (VS Code C++ IntelliSense via the clangd extension), `clang-tidy`
and `include-what-you-use` read it to know your flags and include paths. Symlink it to the source
root: `ln -sf build/compile_commands.json .` so tools find it. This is how a Linux editor gets the
same accuracy Visual Studio gets from the `.vcxproj`.

### 2.10 Presets: one command to configure

`CMakePresets.json` lives next to `CMakeLists.txt`, is committed, and records configure/build/test
recipes so nobody types twenty `-D` flags. CI uses the same presets as developers.

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON", "TCPKV_WERROR": "ON" }
    },
    { "name": "debug",   "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
    { "name": "asan",    "inherits": "debug", "cacheVariables": { "TCPKV_SANITIZE": "address,undefined" } },
    { "name": "tsan",    "inherits": "debug", "cacheVariables": { "TCPKV_SANITIZE": "thread" } },
    { "name": "clang",   "inherits": "debug", "cacheVariables": { "CMAKE_CXX_COMPILER": "clang++" } }
  ],
  "buildPresets": [
    { "name": "debug",   "configurePreset": "debug" },
    { "name": "release", "configurePreset": "release" },
    { "name": "asan",    "configurePreset": "asan" }
  ],
  "testPresets": [
    { "name": "debug", "configurePreset": "debug", "output": { "outputOnFailure": true } },
    { "name": "asan",  "configurePreset": "asan",  "output": { "outputOnFailure": true },
      "environment": { "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1", "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1" } }
  ]
}
```

```bash
cmake --preset asan          # configure into build/asan
cmake --build --preset asan  # build
ctest --preset asan          # test
cmake --list-presets         # what is available
```

`CMakeUserPresets.json` is the gitignored personal overlay (your local compiler path, your
`-j` count).

### 2.11 Finding packages

Two mechanisms.

**`find_package`** looks for something already installed: a `FooConfig.cmake` shipped by the
package (config mode), or a `FindFoo.cmake` module shipped with CMake (module mode). It defines
**imported targets** you link against:

```cmake
find_package(Threads REQUIRED)             # module: Threads::Threads
find_package(OpenSSL REQUIRED)             # module: OpenSSL::SSL OpenSSL::Crypto
find_package(PostgreSQL REQUIRED)          # module: PostgreSQL::PostgreSQL (libpq)
find_package(fmt CONFIG REQUIRED)          # config: fmt::fmt  (after apt install libfmt-dev)
find_package(GTest CONFIG)                 # config: GTest::gtest GTest::gtest_main GTest::gmock
target_link_libraries(tcpkv PRIVATE OpenSSL::SSL PostgreSQL::PostgreSQL)
```

Where it looks: `CMAKE_PREFIX_PATH`, `/usr/lib/cmake/<Pkg>`, `/usr/local/lib/cmake/<Pkg>`,
`<Pkg>_DIR`. With vcpkg: `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake` makes `find_package` see vcpkg
ports (module 12). `pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)` via
`find_package(PkgConfig)` handles libraries that ship only a `.pc` file.

**`FetchContent`** downloads and builds a dependency as part of your build. Right for test
frameworks and small header libraries; wrong for OpenSSL.

```cmake
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)   # MSVC: match /MD
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
# now targets GTest::gtest, GTest::gtest_main, GTest::gmock exist
```

Pin a tag, never `main`. The download happens at configure time into `build/_deps/`. CI caches that
directory (module 09). Prefer `find_package(GTest)` first and fall back to FetchContent so a
system install is used when present:

```cmake
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    # FetchContent block above
endif()
```

### 2.12 Tests with ctest

```cmake
# top level
if(TCPKV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# tests/CMakeLists.txt
add_executable(tcpkv_tests test_protocol.cpp test_store.cpp)
target_link_libraries(tcpkv_tests PRIVATE tcpkv::core GTest::gtest_main GTest::gmock)
tcpkv_set_warnings(tcpkv_tests)
tcpkv_set_sanitizers(tcpkv_tests)
include(GoogleTest)
gtest_discover_tests(tcpkv_tests)       # one ctest entry per TEST(), discovered after build
```

`ctest --test-dir build/debug --output-on-failure -j8`. Module 05 covers the test side.

### 2.13 Install rules

```cmake
include(GNUInstallDirs)                                       # CMAKE_INSTALL_BINDIR = bin, LIBDIR = lib, etc.
install(TARGETS tcpkv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS tcpkv_core EXPORT tcpkvTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT tcpkvTargets NAMESPACE tcpkv:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tcpkv)
install(FILES deploy/tcpkv.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
```

```bash
cmake --install build/release --prefix /opt/tcpkv       # or DESTDIR=/tmp/stage cmake --install ...
```

`install(EXPORT)` is what makes **your** library consumable by `find_package(tcpkv)` in someone
else's project. The multi-stage Dockerfile (module 08) uses `cmake --install` to copy only the
runtime into the final image.

### 2.14 Things to know when reading other people's CMake

- `${CMAKE_CURRENT_SOURCE_DIR}` / `${CMAKE_CURRENT_BINARY_DIR}`: this directory's source and build
  dir. `${PROJECT_SOURCE_DIR}`: the nearest `project()` dir. `${CMAKE_SOURCE_DIR}`: the top.
- `add_subdirectory(dir)` processes `dir/CMakeLists.txt`; variables are scoped (child sees parent's,
  changes do not propagate up without `PARENT_SCOPE`), targets are global.
- `file(GLOB ...)` for sources is discouraged: CMake does not re-run when a file appears, so a new
  `.cpp` is silently not built until you reconfigure. List sources explicitly, or use
  `file(GLOB_RECURSE ... CONFIGURE_DEPENDS ...)` and accept the configure-time cost.
- `message(STATUS "...")`, `message(FATAL_ERROR "...")`. `cmake --trace-expand` when lost.
- `cmake -LH build` lists cache variables with help. `ccmake build` is a TUI for them.
- `set_target_properties(t PROPERTIES CXX_EXTENSIONS OFF)` makes `-std=c++17` instead of `gnu++17`.
  Set `CMAKE_CXX_EXTENSIONS OFF` globally at the top for the same effect.
- Old style you will still see: `include_directories()`, `link_libraries()`, `add_definitions()`,
  `set(CMAKE_CXX_FLAGS ...)`. These are directory-scoped globals. Read them, do not write them.
- Policies: `cmake_minimum_required(VERSION 3.22)` sets behaviour to 3.22 rules. Warnings like
  "Policy CMP0135 is not set" mean a newer CMake changed a default; add `cmake_policy(SET CMP0135 NEW)`.

### 2.15 tcpkv laid out fully

```
tcpkv/
  CMakeLists.txt              root: project, options, library, executable, tests, install
  CMakePresets.json           debug / release / asan / tsan / clang presets
  cmake/CompilerWarnings.cmake   tcpkv_set_warnings(), tcpkv_set_sanitizers()
  include/tcpkv/protocol.hpp  public headers (PUBLIC include dir)
  include/tcpkv/store.hpp
  src/protocol.cpp            library sources
  src/store.cpp
  src/main.cpp                executable: socket loop, uses the library
  tests/CMakeLists.txt        GoogleTest via find_package or FetchContent, gtest_discover_tests
  tests/test_protocol.cpp
  tests/test_store.cpp
  scripts/build.sh            cmake --preset wrappers (module 10)
  scripts/run_tests.sh
  Dockerfile                  multi-stage (module 08)
  .github/workflows/ci.yml    (module 09)
  .gitignore                  build/, CMakeUserPresets.json, compile_commands.json
```

The complete files are in `code/tcpkv/`. Root `CMakeLists.txt` in full:

```cmake
cmake_minimum_required(VERSION 3.22)
project(tcpkv VERSION 0.1.0 DESCRIPTION "TCP key-value server" LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TCPKV_WERROR      "Treat warnings as errors" ON)
option(TCPKV_BUILD_TESTS "Build unit tests" ON)
set(TCPKV_SANITIZE "" CACHE STRING "Sanitizer list, e.g. address,undefined or thread")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(CompilerWarnings)
find_package(Threads REQUIRED)

add_library(tcpkv_core src/protocol.cpp src/store.cpp)
add_library(tcpkv::core ALIAS tcpkv_core)
target_include_directories(tcpkv_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
target_compile_features(tcpkv_core PUBLIC cxx_std_17)
target_link_libraries(tcpkv_core PUBLIC Threads::Threads)
target_compile_definitions(tcpkv_core PUBLIC TCPKV_VERSION="${PROJECT_VERSION}")
tcpkv_set_warnings(tcpkv_core)
tcpkv_set_sanitizers(tcpkv_core)

add_executable(tcpkv src/main.cpp)
target_link_libraries(tcpkv PRIVATE tcpkv::core)
tcpkv_set_warnings(tcpkv)
tcpkv_set_sanitizers(tcpkv)

if(TCPKV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS tcpkv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/tcpkv.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
```

Day-to-day commands:

```bash
cd ~/projects/tcpkv
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset release && cmake --build --preset release
./build/release/tcpkv 7000
```

## 3. Code

- `code/Makefile`: the ten-line Makefile above, with comments. Copy it beside a `src/` and
  `include/` directory and run `make -j`.
- `code/tcpkv/CMakeLists.txt`, `code/tcpkv/cmake/CompilerWarnings.cmake`,
  `code/tcpkv/CMakePresets.json`, `code/tcpkv/tests/CMakeLists.txt`: the complete project.
- The source files `include/tcpkv/*.hpp`, `src/*.cpp`, `tests/*.cpp` are introduced in module 05
  but are present in `code/tcpkv/` now so the project configures and builds.

The key part of `tests/CMakeLists.txt`:

```cmake
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(tcpkv_tests test_protocol.cpp test_store.cpp)
target_link_libraries(tcpkv_tests PRIVATE tcpkv::core GTest::gtest_main GTest::gmock)
tcpkv_set_warnings(tcpkv_tests)
tcpkv_set_sanitizers(tcpkv_tests)

include(GoogleTest)
gtest_discover_tests(tcpkv_tests DISCOVERY_TIMEOUT 30)
```

## 4. Under the hood (senior-level)

**What configure actually does.** CMake runs a scripting-language interpreter over
`CMakeLists.txt`, builds an in-memory graph of targets and properties, evaluates generator
expressions per configuration, resolves transitive usage requirements (walking `INTERFACE_*`
properties of every linked target, depth-first, de-duplicating), then the generator writes
`build.ninja`. It also probes the compiler on the first run (the "Check for working CXX compiler"
lines): it compiles tiny test programs to learn the compiler ID, version, supported features, and
caches the answers in `CMakeCache.txt` and `CMakeFiles/`. That is why the first configure is slow
and later ones are fast, and why switching compilers requires a fresh build directory.

**How transitive linking becomes a command line.** `target_link_libraries(tcpkv PRIVATE
tcpkv::core)` with `tcpkv_core` having `PUBLIC Threads::Threads` produces
`g++ ... main.cpp.o -o tcpkv libtcpkv_core.a -pthread`. CMake orders libraries by the dependency
graph so the static-library ordering rule from module 02 is satisfied automatically. For `PUBLIC`
include dirs it emits `-I/home/.../include` on `tcpkv`'s compile lines without you naming it. The
`INTERFACE_LINK_LIBRARIES` property on `tcpkv_core` is the thing being read.

**How Ninja decides what to rebuild.** For each edge it compares the mtime of outputs against
inputs and the recorded command hash in `.ninja_log`; if the command line changed (you added a
flag), it rebuilds even if sources did not. Header dependencies come from `-MD -MF` depfiles the
compiler writes during the compile, parsed into `.ninja_deps` (a binary cache) so the next run
loads them instantly. Make re-reads every `.d` file as text on every run; on a 5,000-file tree that
is noticeable.

**Why `file(GLOB)` is a trap.** The glob is evaluated at configure time and the result baked into
`build.ninja`. Ninja does not know the glob exists. A new `.cpp` is invisible until something
triggers a reconfigure. `CONFIGURE_DEPENDS` adds a check that re-globs on every build and
reconfigures if the set changed; it costs a directory scan per build.

**Cache semantics.** `set(VAR value CACHE TYPE "doc")` sets a cache entry only if it does not
already exist; `FORCE` overrides. `option()` is the same. A normal `set(VAR value)` creates a
normal variable that **shadows** the cache variable in that scope, which is the source of a
class of "why does my -D have no effect" questions: a `set()` later in the file overrides the
command line. Since policy CMP0077 (3.13), `option()` does not override a normal variable set by
the parent, which is what lets you `set(INSTALL_GTEST OFF)` before `FetchContent_MakeAvailable`.

**Config-mode `find_package` internals.** `fmtConfig.cmake` is a CMake script that creates
IMPORTED targets with `IMPORTED_LOCATION` and `INTERFACE_INCLUDE_DIRECTORIES`. Linking an imported
target is identical to linking an in-tree target from the consumer's perspective. This is the
mechanism `install(EXPORT)` generates for your own library.

**MSBuild comparison.** A `.vcxproj` is per-project; a `.sln` ties them together with GUIDs;
`ProjectReference` is roughly `target_link_libraries` but include paths do not propagate (you
repeat `AdditionalIncludeDirectories` or use property sheets). CMake's usage requirements are the
feature MSBuild lacks. CMake can generate a `.sln` from the same `CMakeLists.txt`, which is how
cross-platform teams work: one CMake, Visual Studio on Windows, Ninja on Linux (module 12).

## 5. In production

- **One `CMakeLists.txt`, three build directories.** `build/debug` for daily work, `build/asan`
  for running tests before a PR, `build/release` for perf measurements and the Docker image. The
  presets make this `cmake --preset X`. Developers who type raw `-D` flags drift from CI.
- **`-Werror` toggle.** `TCPKV_WERROR` ON by default and in CI; a packager building with GCC 15
  can pass `-DTCPKV_WERROR=OFF` instead of patching your files.
- **Pin dependency versions.** `GIT_TAG v1.15.2`, not `main`. Reproducible builds are a security
  and debugging requirement. Record the exact compiler too (Docker image tag, module 08).
- **Compile times.** `ninja -C build -d stats`, `ClangBuildAnalyzer` with `-ftime-trace`, and
  `ccache` (`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`) are the tools. A 40-minute build becomes 4
  with ccache in CI (module 09).
- **Install, do not copy.** The Dockerfile runs `cmake --install build --prefix /opt/tcpkv`; the
  runtime stage copies `/opt/tcpkv`. Install rules are the contract of what ships.
- **Mapping to your work.** Your RPC server probably has a `.sln` with a dozen projects and
  property sheets for common flags. The CMake equivalent is one root `CMakeLists.txt`, one
  `add_subdirectory` per component, a `cmake/` folder with the shared functions, and no flags in
  individual targets beyond what is specific to them. Being able to sketch that migration is a strong
  interview answer.
- **3 AM:** "the build is green but the binary is stale". Someone ran `cmake --build` after editing
  a header that only a `file(GLOB)`-collected source includes, and the glob did not see a renamed
  file. Fix the glob, then add `CONFIGURE_DEPENDS` or list files explicitly.

## 6. Try it

1. **(easy)** Copy `code/Makefile` next to a `src/` with two `.cpp` files and an `include/` header.
   Run `make`, touch the header, run `make` again. Done when the header edit rebuilds both objects
   and you can show the `.d` file that made it happen.
2. **(easy)** Configure `code/tcpkv` with `cmake --preset debug`, build, and run
   `ninja -C build/debug -t commands tcpkv | tail -3`. Done when you can point at `-I.../include`,
   `-pthread`, `-std=c++17` in the command and name the CMake line responsible for each.
3. **(medium)** Change `target_link_libraries(tcpkv_core PUBLIC Threads::Threads)` to `PRIVATE`,
   reconfigure and build. Observe whether `tcpkv` still links and explain why or why not (hint:
   `store.hpp` holds a `std::mutex`; glibc 2.34+ merged libpthread). Then change the include
   directory from `PUBLIC` to `PRIVATE` and read the resulting error. Restore both. Done when you
   can state the rule for choosing visibility in one sentence.
4. **(medium)** Add a `clang` preset that sets `CMAKE_CXX_COMPILER` to `clang++`. Build with it.
   Fix any warnings Clang finds that GCC did not. Done when both presets build with `-Werror`.
5. **(hard)** Add a second library `tcpkv_net` in `src/net/` with its own `include/tcpkv/net/`
   header and make `tcpkv` depend on it; `tcpkv_net` must depend on `tcpkv_core`. Verify the
   transitive include dirs arrive at `tcpkv` without you adding them. Done when
   `ninja -t commands tcpkv` shows the include path exactly once.
6. **(hard)** Add `install(EXPORT ...)` rules, run `cmake --install build/release --prefix
   /tmp/tcpkv-install`, then create a separate consumer project with
   `find_package(tcpkv CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE tcpkv::core)`,
   configured with `-DCMAKE_PREFIX_PATH=/tmp/tcpkv-install`. Done when the consumer builds and runs.

## 7. Interview questions

**Q: What is the difference between CMake and Make?**
Make is a build executor: it runs rules whose inputs are newer than their outputs. CMake is a
build-system generator: from one platform-independent description of targets it produces Makefiles,
Ninja files or Visual Studio solutions. CMake also handles finding libraries, tests, installation
and packaging.

**Q: Explain PUBLIC, PRIVATE and INTERFACE in `target_link_libraries`.**
They control propagation of usage requirements. PRIVATE: the dependency is used only to build this
target. PUBLIC: used to build this target and propagated to consumers, because it appears in public
headers. INTERFACE: not used by this target itself but consumers need it, as with header-only
libraries. Correct visibility means a consumer links one target and inherits everything it needs.

**Q: Why is `set(CMAKE_CXX_FLAGS "-Wall")` considered bad practice?**
It is a global string applied to every target in the directory and below, including third-party
code, and it cannot be scoped or expressed per configuration cleanly. `target_compile_options`
with PRIVATE scope and generator expressions attaches flags to the targets that should have them.

**Q: What is an out-of-source build and why does it matter?**
Build artefacts go in a separate directory from the sources. The tree stays clean, several
configurations (debug, asan, release) coexist, and cleaning is `rm -rf build`. In-source builds
pollute the tree and make a full clean impossible to do safely.

**Q: How does CMake know to rebuild when a header changes?**
It does not; the generator does. CMake emits rules with depfile support; the compiler writes `-MD`
dependency files during compilation listing every header included; Ninja (or Make via `-include`)
reads them on the next build. That is why the first build must happen before header dependencies
are known.

**Q: `find_package` vs `FetchContent`: when do you use each?**
`find_package` consumes an installed library through its config file or a find module and is right
for system dependencies like OpenSSL or libpq. `FetchContent` downloads and builds a dependency as
part of your tree, pinned to a tag, and is right for test frameworks and small libraries where you
want an exact version regardless of what the system has.

**Q: What are presets for?**
`CMakePresets.json` records named configure/build/test recipes (generator, build dir, cache
variables, environment) so developers and CI run the identical `cmake --preset asan` instead of
long ad-hoc command lines. Hidden base presets and inheritance avoid repetition.

**Q: What does `gtest_discover_tests` do differently from `add_test`?**
`add_test` registers one ctest entry that runs the whole test binary. `gtest_discover_tests` runs
the binary with `--gtest_list_tests` after it is built and registers one ctest entry per test
case, so `ctest -j` parallelises across tests and failures are reported per test name.

## 8. Recap

- Build systems exist for minimal rebuilds; header dependency tracking is the hard part (`-MMD`).
- Make: `target: prereqs` + tab-indented recipe; `$@ $< $^`; pattern rules; `.PHONY`; `-include *.d`.
- CMake generates native build files; configure then build; Ninja is the generator to use.
- Everything is a target with properties; usage requirements propagate via PUBLIC/INTERFACE.
- PRIVATE for implementation needs, PUBLIC when it is in your public headers, INTERFACE for header-only.
- Warnings and sanitizers are PRIVATE compile options attached by a helper function, never global flags.
- Always set `CMAKE_BUILD_TYPE`; `RelWithDebInfo` is what you ship.
- Out-of-source builds, one directory per configuration, driven by `CMakePresets.json`.
- `CMAKE_EXPORT_COMPILE_COMMANDS ON` feeds clangd and clang-tidy.
- `find_package` for installed libs, `FetchContent` with a pinned tag for GoogleTest; `install()` defines what ships.
