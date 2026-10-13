cmake_minimum_required(VERSION 3.22)

# Answer key for the minidb build plan: the storage library and its four test suites.
# The plan's apps/minidb_cli.cpp and tools/bench.cpp are left for you to write (steps 7 and 8),
# so they are not built here. Everything else matches 01_build_plan.md exactly.
project(minidb_reference
    VERSION 0.1.0
    DESCRIPTION "Page-based key-value storage engine with a B+tree and a write-ahead log"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(MINIDB_SANITIZER "" CACHE STRING "Sanitizer to enable: address | thread | undefined | (empty)")

find_package(Threads REQUIRED)

add_library(minidb_warnings INTERFACE)
target_compile_options(minidb_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Werror)

if(MINIDB_SANITIZER)
    message(STATUS "minidb: building with -fsanitize=${MINIDB_SANITIZER}")
    add_compile_options(-fsanitize=${MINIDB_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${MINIDB_SANITIZER})
endif()

add_library(minidb STATIC
    src/crc32.cpp
    src/file.cpp
    src/pager.cpp
    src/page_cache.cpp
    src/wal.cpp
    src/recovery.cpp
    src/btree.cpp
    src/db.cpp)
target_include_directories(minidb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minidb PUBLIC Threads::Threads PRIVATE minidb_warnings)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)
foreach(name IN ITEMS storage btree db crash)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_include_directories(${name}_test PRIVATE tests)
    target_link_libraries(${name}_test PRIVATE minidb GTest::gtest_main minidb_warnings)
    # fsync-bound tests are slow, more so under sanitizers: generous timeouts.
    gtest_discover_tests(${name}_test DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 900)
endforeach()
