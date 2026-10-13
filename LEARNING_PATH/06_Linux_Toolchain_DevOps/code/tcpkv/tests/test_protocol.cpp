// tests/test_protocol.cpp - unit tests for the parser and formatters.
// Naming: TEST(<Unit>, <Behaviour>_<Condition>) reads as a sentence in the ctest output.
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include "tcpkv/protocol.hpp"

using tcpkv::CommandType;
using tcpkv::parse_command;

// ---------------------------------------------------------------- plain TESTs

TEST(ParseCommand, PingWithoutArgumentsIsPing) {
    const auto r = parse_command("PING");
    ASSERT_TRUE(r.ok()) << r.error;         // ASSERT: nothing below makes sense if this fails
    EXPECT_EQ(r.command->type, CommandType::Ping);
    EXPECT_TRUE(r.command->key.empty());
    EXPECT_TRUE(r.command->value.empty());
}

TEST(ParseCommand, TrailingCarriageReturnIsStripped) {
    const auto r = parse_command("PING\r");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->type, CommandType::Ping);
}

TEST(ParseCommand, GetExtractsKey) {
    const auto r = parse_command("GET user:42");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->type, CommandType::Get);
    EXPECT_EQ(r.command->key, "user:42");
}

TEST(ParseCommand, SetValueKeepsInternalSpaces) {
    const auto r = parse_command("SET greeting hello   world ");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->type, CommandType::Set);
    EXPECT_EQ(r.command->key, "greeting");
    EXPECT_EQ(r.command->value, "hello   world ");  // value is the raw remainder of the line
}

TEST(ParseCommand, SetWithEmptyValueIsAllowed) {
    const auto r = parse_command("SET k ");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->value, "");
}

TEST(ParseCommand, MultipleSpacesBetweenVerbAndKeyAreTolerated) {
    const auto r = parse_command("GET    k");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->key, "k");
}

TEST(ParseCommand, CommandsAreCaseSensitive) {
    EXPECT_FALSE(parse_command("get k").ok());
    EXPECT_FALSE(parse_command("Ping").ok());
}

TEST(ParseCommand, KeyAtMaxLengthIsAccepted) {
    const std::string key(tcpkv::kMaxKeyLength, 'k');
    EXPECT_TRUE(parse_command("GET " + key).ok());
}

TEST(ParseCommand, KeyOverMaxLengthIsRejected) {
    const std::string key(tcpkv::kMaxKeyLength + 1, 'k');
    const auto r = parse_command("GET " + key);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, "invalid key");
}

TEST(ParseCommand, ValueOverMaxLengthIsRejected) {
    const std::string value(tcpkv::kMaxValueLength + 1, 'v');
    const auto r = parse_command("SET k " + value);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error, "value too long");
}

// ---------------------------------------------------------------- parameterised: many bad inputs

struct BadInput {
    const char* line;
    const char* expected_error;
};

// Printing the struct makes failing instances readable in the test log.
std::ostream& operator<<(std::ostream& os, const BadInput& b) {
    return os << "line=\"" << b.line << "\" expect=\"" << b.expected_error << "\"";
}

class ParseCommandRejects : public ::testing::TestWithParam<BadInput> {};

TEST_P(ParseCommandRejects, ReturnsExpectedError) {
    const BadInput& in = GetParam();
    const auto r = parse_command(in.line);
    EXPECT_FALSE(r.ok()) << "unexpectedly parsed: " << in.line;
    EXPECT_EQ(r.error, in.expected_error);
}

INSTANTIATE_TEST_SUITE_P(MalformedLines, ParseCommandRejects,
    ::testing::Values(
        BadInput{"",            "empty command"},
        BadInput{"\r",          "empty command"},
        BadInput{"FLY",         "unknown command"},
        BadInput{"PING now",    "PING takes no arguments"},
        BadInput{"QUIT 1",      "QUIT takes no arguments"},
        BadInput{"GET",         "invalid key"},
        BadInput{"GET ",        "invalid key"},
        BadInput{"GET a b",     "GET takes exactly one argument"},
        BadInput{"DEL a b",     "DEL takes exactly one argument"},
        BadInput{"SET",         "invalid key"},
        BadInput{"SET \x01 v",  "invalid key"},    // control character in key
        BadInput{"GET k\xC3\xA9", "invalid key"}  // non-ASCII in key
    ));

// ---------------------------------------------------------------- parameterised: verb -> type

// tuple<line, expected type, expected to_string(type)>
class ParseVerb
    : public ::testing::TestWithParam<std::tuple<const char*, CommandType, const char*>> {};

TEST_P(ParseVerb, MapsToCommandTypeAndName) {
    const auto [line, expected_type, expected_name] = GetParam();
    const auto r = parse_command(line);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->type, expected_type);
    EXPECT_STREQ(tcpkv::to_string(r.command->type), expected_name);
}

INSTANTIATE_TEST_SUITE_P(AllVerbs, ParseVerb,
    ::testing::Values(
        std::make_tuple("PING",    CommandType::Ping, "PING"),
        std::make_tuple("GET k",   CommandType::Get,  "GET"),
        std::make_tuple("SET k v", CommandType::Set,  "SET"),
        std::make_tuple("DEL k",   CommandType::Del,  "DEL"),
        std::make_tuple("QUIT",    CommandType::Quit, "QUIT")));

// ---------------------------------------------------------------- formatters

TEST(Format, EveryResponseEndsWithNewline) {
    EXPECT_EQ(tcpkv::format_pong().back(), '\n');
    EXPECT_EQ(tcpkv::format_ok().back(), '\n');
    EXPECT_EQ(tcpkv::format_not_found().back(), '\n');
    EXPECT_EQ(tcpkv::format_value("x").back(), '\n');
    EXPECT_EQ(tcpkv::format_error("x").back(), '\n');
}

TEST(Format, ExactWireStrings) {
    EXPECT_EQ(tcpkv::format_pong(), "PONG\n");
    EXPECT_EQ(tcpkv::format_ok(), "OK\n");
    EXPECT_EQ(tcpkv::format_not_found(), "NOT_FOUND\n");
    EXPECT_EQ(tcpkv::format_value("hello world"), "VALUE hello world\n");
    EXPECT_EQ(tcpkv::format_error("invalid key"), "ERROR invalid key\n");
}

TEST(Format, RoundTripSetThenParseValue) {
    // A property-style test: whatever SET accepts, VALUE must carry back byte for byte.
    const std::string value = "tabs\tand  spaces and \x7f bytes";
    const auto r = parse_command("SET k " + value);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(tcpkv::format_value(r.command->value), "VALUE " + value + "\n");
}
