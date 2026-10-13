#pragma once
#include <string>
#include <string_view>

namespace hello {

// Returns "Hello, <name>!"; an empty name becomes "Hello, world!".
std::string greet(std::string_view name);

}  // namespace hello
