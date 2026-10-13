cmake_minimum_required(VERSION 3.22)

project(tcpkv
    VERSION 1.0.0
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
option(TCPKV_BUILD_TOOLS "Build the load generator" ON)
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
# Sanitizers: applied to everything in this directory tree (library, apps, tests, gtest)
# so that the instrumented library and the instrumented tests agree.
# ---------------------------------------------------------------------------------------------
if(TCPKV_SANITIZER)
    message(STATUS "tcpkv: building with -fsanitize=${TCPKV_SANITIZER}")
    add_compile_options(-fsanitize=${TCPKV_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TCPKV_SANITIZER})
endif()

# ---------------------------------------------------------------------------------------------
# The library: everything except main()
# ---------------------------------------------------------------------------------------------
add_library(tcpkv STATIC
    src/fd.cpp
    src/socket.cpp
    src/protocol.cpp
    src/store.cpp
    src/thread_pool.cpp
    src/connection.cpp
    src/server.cpp
    src/client.cpp
    src/metrics.cpp
    src/log.cpp)
target_include_directories(tcpkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tcpkv
    PUBLIC Threads::Threads
    PRIVATE tcpkv_warnings)

# ---------------------------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------------------------
add_executable(tcpkv_server apps/tcpkv_server.cpp)
target_link_libraries(tcpkv_server PRIVATE tcpkv tcpkv_warnings)

add_executable(tcpkv_cli apps/tcpkv_cli.cpp)
target_link_libraries(tcpkv_cli PRIVATE tcpkv tcpkv_warnings)

if(TCPKV_BUILD_TOOLS)
    add_executable(load_gen tools/load_gen.cpp)
    target_link_libraries(load_gen PRIVATE tcpkv tcpkv_warnings)
endif()

install(TARGETS tcpkv_server tcpkv_cli RUNTIME DESTINATION bin)

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

    foreach(name IN ITEMS fd protocol store thread_pool integration)
        add_executable(${name}_test tests/${name}_test.cpp)
        target_link_libraries(${name}_test PRIVATE tcpkv GTest::gtest_main tcpkv_warnings)
        # One ctest entry per TEST(); DISCOVERY_TIMEOUT is generous because sanitizer builds
        # start slowly.
        gtest_discover_tests(${name}_test
            DISCOVERY_TIMEOUT 120
            PROPERTIES TIMEOUT 300)
    endforeach()
endif()
