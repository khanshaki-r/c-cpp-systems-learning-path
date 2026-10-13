# tests/CMakeLists.txt
# GoogleTest: prefer a system install (apt install libgtest-dev provides GTestConfig.cmake on
# Ubuntu 24.04); otherwise fetch a pinned release. Never track 'main'.

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found on system; fetching v1.15.2 via FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE)
    # MSVC: use the dynamic CRT (/MD) to match our own targets; avoids LNK2038.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Do not pollute `cmake --install` with gtest's own install rules.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(tcpkv_tests
    test_protocol.cpp
    test_store.cpp
    test_gmock_example.cpp)

# gtest_main provides main(); gmock for the mocking examples in module 05.
target_link_libraries(tcpkv_tests PRIVATE
    tcpkv::core
    GTest::gtest_main
    GTest::gmock)

# Tests get the same warnings and sanitizers as production code.
tcpkv_set_warnings(tcpkv_tests)
tcpkv_set_sanitizers(tcpkv_tests)

# One ctest entry per TEST()/TEST_F()/TEST_P() instance, discovered after the binary is built.
include(GoogleTest)
gtest_discover_tests(tcpkv_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "unit")
