#include <iostream>

#include "greeter.hpp"

int main(int argc, char** argv) {
    std::cout << hello::greet(argc > 1 ? argv[1] : "") << '\n';
    return 0;
}
