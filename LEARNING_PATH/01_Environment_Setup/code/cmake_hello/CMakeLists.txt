cmake_minimum_required(VERSION 3.20)
project(cmake_hello LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # gives clangd / VS Code exact flags

# One place for the warning policy; every target links it.
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE -Wall -Wextra -Werror -Wshadow -Wconversion)

# The library under test.
add_library(greeter src/greeter.cpp)
target_include_directories(greeter PUBLIC include)
target_link_libraries(greeter PRIVATE project_warnings)

# The application.
add_executable(greeter_app src/main.cpp)
target_link_libraries(greeter_app PRIVATE greeter project_warnings)

# Tests: GoogleTest downloaded at configure time.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)   # use extraction time as timestamp for downloaded archives
endif()
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(greeter_test tests/greeter_test.cpp)
target_link_libraries(greeter_test PRIVATE greeter GTest::gtest_main project_warnings)

include(GoogleTest)
gtest_discover_tests(greeter_test)
