# tcpkv - TCP key-value server. Root CMakeLists.txt.
# Configure:  cmake --preset debug      (see CMakePresets.json)
# Build:      cmake --build --preset debug
# Test:       ctest --preset debug
cmake_minimum_required(VERSION 3.22)
project(tcpkv VERSION 0.1.0 DESCRIPTION "TCP key-value server" LANGUAGES CXX)

# Where our own .cmake helper modules live (include(CompilerWarnings) finds cmake/CompilerWarnings.cmake).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Language level for every target. CXX_EXTENSIONS OFF gives -std=c++17 instead of -std=gnu++17,
# so code that compiles here also compiles under MSVC /permissive-.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Feeds clangd / clang-tidy. Symlink build/<preset>/compile_commands.json to the source root.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options: ON for developers and CI; packagers may turn WERROR off without editing this file.
option(TCPKV_WERROR      "Treat warnings as errors" ON)
option(TCPKV_BUILD_TESTS "Build unit tests" ON)
set(TCPKV_SANITIZE "" CACHE STRING "Sanitizer list passed to -fsanitize=, e.g. address,undefined or thread")

# A single-config generator with no build type gives NO optimisation and NO debug info. Never ship that.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(CompilerWarnings)      # defines tcpkv_set_warnings() and tcpkv_set_sanitizers()
find_package(Threads REQUIRED) # imported target Threads::Threads (-pthread on Linux)

# ---------------------------------------------------------------- library: protocol parser + store
add_library(tcpkv_core
    src/protocol.cpp
    src/store.cpp)
add_library(tcpkv::core ALIAS tcpkv_core)

# PUBLIC: consumers #include <tcpkv/store.hpp>, so they need this directory too.
# BUILD_INTERFACE / INSTALL_INTERFACE: the path differs between "building in this tree" and
# "using the installed package".
target_include_directories(tcpkv_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(tcpkv_core PUBLIC cxx_std_17)

# PUBLIC because store.hpp holds a std::mutex: every consumer compiles code touching pthread types.
target_link_libraries(tcpkv_core PUBLIC Threads::Threads)

# Version string available to both the library and anyone linking it (PUBLIC).
target_compile_definitions(tcpkv_core PUBLIC TCPKV_VERSION="${PROJECT_VERSION}")

tcpkv_set_warnings(tcpkv_core)
tcpkv_set_sanitizers(tcpkv_core)

# ---------------------------------------------------------------- executable: the server
add_executable(tcpkv src/main.cpp)
# PRIVATE: nothing links against an executable, and this is the only line main.cpp needs.
# Include dirs, -pthread and C++17 all arrive transitively from tcpkv::core.
target_link_libraries(tcpkv PRIVATE tcpkv::core)
tcpkv_set_warnings(tcpkv)
tcpkv_set_sanitizers(tcpkv)

# ---------------------------------------------------------------- tests
if(TCPKV_BUILD_TESTS)
    enable_testing()          # must be called in the top-level CMakeLists for ctest to find tests
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- install
# What `cmake --install <build> --prefix <dir>` copies. The Dockerfile runtime stage copies only this.
include(GNUInstallDirs)
install(TARGETS tcpkv
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS tcpkv_core EXPORT tcpkvTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT tcpkvTargets
        NAMESPACE tcpkv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tcpkv)
install(FILES deploy/tcpkv.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)

# Summary printed at the end of configure so a CI log shows exactly what was built.
message(STATUS "tcpkv ${PROJECT_VERSION}")
message(STATUS "  compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  -Werror      : ${TCPKV_WERROR}")
message(STATUS "  sanitizers   : '${TCPKV_SANITIZE}'")
message(STATUS "  tests        : ${TCPKV_BUILD_TESTS}")
