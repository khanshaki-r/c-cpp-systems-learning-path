#include "greeter.hpp"

namespace hello {

std::string greet(std::string_view name) {
    if (name.empty()) name = "world";
    return "Hello, " + std::string(name) + "!";
}

}  // namespace hello
