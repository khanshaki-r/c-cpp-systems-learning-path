#include <gtest/gtest.h>

#include "greeter.hpp"

TEST(Greeter, UsesName) {
    EXPECT_EQ(hello::greet("Shakir"), "Hello, Shakir!");
}

TEST(Greeter, EmptyNameBecomesWorld) {
    EXPECT_EQ(hello::greet(""), "Hello, world!");
}
