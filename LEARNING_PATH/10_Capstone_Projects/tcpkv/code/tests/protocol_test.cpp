#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/protocol.hpp"

using tcpkv::Command;
using tcpkv::CommandType;
using tcpkv::ParseError;
using tcpkv::ParseEvent;
using tcpkv::Parser;
namespace limits = tcpkv::limits;
namespace reply = tcpkv::reply;

namespace {

std::optional<Command> next_command(Parser& parser) {
    std::optional<ParseEvent> event = parser.next();
    if (!event) {
        return std::nullopt;
    }
    if (Command* command = std::get_if<Command>(&*event)) {
        return std::move(*command);
    }
    return std::nullopt;
}

std::optional<ParseError> next_error(Parser& parser) {
    std::optional<ParseEvent> event = parser.next();
    if (!event) {
        return std::nullopt;
    }
    if (ParseError* error = std::get_if<ParseError>(&*event)) {
        return std::move(*error);
    }
    return std::nullopt;
}

}  // namespace

// ------------------------------------------------------------------ regression: EXPIRE overflow

// Before the kMaxExpireSeconds bound, these values overflowed inside std::chrono (UBSan:
// "signed integer overflow") while every CI job stayed green, because no test sent them.
TEST(Parser, ExpireSecondsOutOfRangeIsNonFatal) {
    Parser parser;
    parser.feed("EXPIRE k 10000000000\nEXPIRE k -10000000000\nEXPIRE k 3153600000\nPING\n");
    for (int i = 0; i < 2; ++i) {
        const auto error = next_error(parser);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->message, "invalid expire seconds");
        EXPECT_FALSE(error->fatal);
    }
    const auto at_limit = next_command(parser);  // exactly 100 years: accepted
    ASSERT_TRUE(at_limit.has_value());
    EXPECT_EQ(at_limit->type, CommandType::Expire);
    const auto ping = next_command(parser);  // the stream is still in sync
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->type, CommandType::Ping);
}

// ------------------------------------------------------------------ happy paths

TEST(Parser, ParsesPing) {
    Parser parser;
    parser.feed("PING\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Ping);
    EXPECT_TRUE(cmd->args.empty());
    EXPECT_FALSE(parser.next().has_value());
}

TEST(Parser, CommandNamesAreCaseInsensitive) {
    Parser parser;
    parser.feed("ping\r\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Ping);
}

TEST(Parser, ParsesGetWithKey) {
    Parser parser;
    parser.feed("GET user:42\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Get);
    ASSERT_EQ(cmd->args.size(), 1u);
    EXPECT_EQ(cmd->args[0], "user:42");
}

TEST(Parser, ParsesSetWithLengthPrefixedValue) {
    Parser parser;
    parser.feed("SET k 5\nhello\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Set);
    ASSERT_EQ(cmd->args.size(), 1u);  // the length is consumed by the parser, not passed on
    EXPECT_EQ(cmd->args[0], "k");
    EXPECT_EQ(cmd->value, "hello");
    EXPECT_EQ(parser.buffered(), 0u);
}

TEST(Parser, SetValueMayContainSpacesNewlinesAndNul) {
    Parser parser;
    const std::string value("a b\n\r\0c", 7);
    parser.feed("SET k 7\n" + value + "\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->value, value);
}

TEST(Parser, SetAllowsEmptyValue) {
    Parser parser;
    parser.feed("SET k 0\n\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Set);
    EXPECT_TRUE(cmd->value.empty());
}

TEST(Parser, SetAcceptsCrLfAfterValue) {
    Parser parser;
    parser.feed("SET k 2\r\nhi\r\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->value, "hi");
    EXPECT_EQ(parser.buffered(), 0u);
}

TEST(Parser, ByteByByteFeedingProducesExactlyOneCommand) {
    Parser parser;
    const std::string wire = "SET key 5\nhello\n";
    int commands = 0;
    for (const char c : wire) {
        parser.feed(&c, 1);
        while (auto cmd = next_command(parser)) {
            ++commands;
            EXPECT_EQ(cmd->type, CommandType::Set);
            EXPECT_EQ(cmd->args[0], "key");
            EXPECT_EQ(cmd->value, "hello");
        }
    }
    EXPECT_EQ(commands, 1);
}

TEST(Parser, PipelinedCommandsInOneBuffer) {
    Parser parser;
    parser.feed("PING\nGET a\nSET b 1\nx\nDEL a b\n");
    std::vector<CommandType> types;
    while (auto cmd = next_command(parser)) {
        types.push_back(cmd->type);
    }
    const std::vector<CommandType> expected = {CommandType::Ping, CommandType::Get,
                                               CommandType::Set, CommandType::Del};
    EXPECT_EQ(types, expected);
}

TEST(Parser, ValueSplitAcrossFeeds) {
    Parser parser;
    parser.feed("SET k 10\n01234");
    EXPECT_FALSE(parser.next().has_value());
    parser.feed("56789");
    EXPECT_FALSE(parser.next().has_value());  // still missing the trailing newline
    parser.feed("\nPING\n");
    auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->value, "0123456789");
    cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Ping);
}

TEST(Parser, BlankLinesAreIgnored) {
    Parser parser;
    parser.feed("\n\r\n   \nPING\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Ping);
    EXPECT_FALSE(parser.next().has_value());
}

TEST(Parser, TabsAndMultipleSpacesSeparateTokens) {
    Parser parser;
    parser.feed("DEL  a\tb   c\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    const std::vector<std::string> expected = {"a", "b", "c"};
    EXPECT_EQ(cmd->args, expected);
}

TEST(Parser, KeysPrefixIsOptional) {
    Parser parser;
    parser.feed("KEYS\nKEYS user:\n");
    auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Keys);
    EXPECT_TRUE(cmd->args.empty());
    cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    ASSERT_EQ(cmd->args.size(), 1u);
    EXPECT_EQ(cmd->args[0], "user:");
}

TEST(Parser, ExpireAcceptsSignedIntegers) {
    Parser parser;
    parser.feed("EXPIRE k 10\nEXPIRE k -5\nTTL k\nSTATS\nQUIT\n");
    const CommandType expected[] = {CommandType::Expire, CommandType::Expire, CommandType::Ttl,
                                    CommandType::Stats, CommandType::Quit};
    for (const CommandType type : expected) {
        const auto cmd = next_command(parser);
        ASSERT_TRUE(cmd.has_value());
        EXPECT_EQ(cmd->type, type);
    }
}

TEST(Parser, MaximumValueLengthIsAccepted) {
    Parser parser;
    const std::string value(limits::kMaxValueLength, 'v');
    parser.feed("SET k " + std::to_string(value.size()) + "\n");
    parser.feed(value);
    parser.feed("\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->value.size(), limits::kMaxValueLength);
}

// ------------------------------------------------------------------ malformed input

TEST(Parser, UnknownCommandIsNonFatalAndParsingContinues) {
    Parser parser;
    parser.feed("FROBNICATE x\nPING\n");
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_FALSE(error->fatal);
    EXPECT_NE(error->message.find("unknown command"), std::string::npos);
    EXPECT_NE(error->message.find("FROBNICATE"), std::string::npos);

    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, CommandType::Ping);
    EXPECT_FALSE(parser.dead());
}

TEST(Parser, WrongArityIsNonFatal) {
    const char* lines[] = {"GET\n", "GET a b\n", "PING x\n", "SET k\n", "SET k 1 2\n",
                           "DEL\n", "EXISTS\n", "KEYS a b\n", "EXPIRE k\n", "TTL\n", "QUIT now\n"};
    for (const char* line : lines) {
        Parser parser;
        parser.feed(line);
        const auto error = next_error(parser);
        ASSERT_TRUE(error.has_value()) << line;
        EXPECT_FALSE(error->fatal) << line;
        EXPECT_FALSE(parser.dead()) << line;
    }
}

TEST(Parser, InvalidKeysAreRejected) {
    const std::string too_long(limits::kMaxKeyLength + 1, 'k');
    const std::string lines[] = {"GET " + too_long + "\n", "GET \x01\n", "GET k\x7f\n",
                                 "SET \xc3\xa9 1\n"};
    for (const std::string& line : lines) {
        Parser parser;
        parser.feed(line);
        const auto error = next_error(parser);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->message, "invalid key");
        EXPECT_FALSE(error->fatal);
    }
}

TEST(Parser, MaximumKeyLengthIsAccepted) {
    Parser parser;
    const std::string key(limits::kMaxKeyLength, 'k');
    parser.feed("GET " + key + "\n");
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->args[0], key);
}

TEST(Parser, SetWithBadLengthIsNonFatal) {
    const char* lines[] = {"SET k abc\n", "SET k -1\n", "SET k +5\n", "SET k 5x\n", "SET k\t\n"};
    for (const char* line : lines) {
        Parser parser;
        parser.feed(line);
        const auto error = next_error(parser);
        ASSERT_TRUE(error.has_value()) << line;
        EXPECT_FALSE(error->fatal) << line;
    }
}

TEST(Parser, OversizedValueIsFatal) {
    Parser parser;
    parser.feed("SET k " + std::to_string(limits::kMaxValueLength + 1) + "\n");
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(error->fatal);
    EXPECT_EQ(error->message, "value too large");
    EXPECT_TRUE(parser.dead());

    parser.feed("PING\n");  // a dead parser ignores further input
    EXPECT_FALSE(parser.next().has_value());
    EXPECT_EQ(parser.buffered(), 0u);
}

TEST(Parser, LineWithoutNewlineBeyondLimitIsFatal) {
    Parser parser;
    parser.feed(std::string(limits::kMaxLineLength + 1, 'A'));
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(error->fatal);
    EXPECT_EQ(error->message, "line too long");
}

TEST(Parser, LineAtLimitIsNotFatal) {
    Parser parser;
    parser.feed(std::string(limits::kMaxLineLength, 'A'));
    EXPECT_FALSE(parser.next().has_value());  // waiting for the newline, not dead
    EXPECT_FALSE(parser.dead());
}

TEST(Parser, ValueNotFollowedByNewlineIsFatal) {
    Parser parser;
    parser.feed("SET k 2\nabc\n");
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_TRUE(error->fatal);
    EXPECT_EQ(error->message, "value not terminated by newline");
}

TEST(Parser, TooManyArgumentsIsNonFatal) {
    std::string line = "DEL";
    for (std::size_t i = 0; i < limits::kMaxArgs + 1; ++i) {
        line += " k" + std::to_string(i);
    }
    line += "\n";
    Parser parser;
    parser.feed(line);
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->message, "too many arguments");
    EXPECT_FALSE(error->fatal);
}

TEST(Parser, ExpireWithNonIntegerSecondsIsNonFatal) {
    Parser parser;
    parser.feed("EXPIRE k ten\n");
    const auto error = next_error(parser);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->message, "invalid expire seconds");
}

// ------------------------------------------------------------------ validators and encoders

TEST(Protocol, KeyValidation) {
    EXPECT_TRUE(tcpkv::is_valid_key("a"));
    EXPECT_TRUE(tcpkv::is_valid_key("user:42/profile.json"));
    EXPECT_FALSE(tcpkv::is_valid_key(""));
    EXPECT_FALSE(tcpkv::is_valid_key("has space"));
    EXPECT_FALSE(tcpkv::is_valid_key("tab\there"));
    EXPECT_FALSE(tcpkv::is_valid_key(std::string(limits::kMaxKeyLength + 1, 'x')));
    EXPECT_TRUE(tcpkv::is_valid_key_prefix(""));
    EXPECT_TRUE(tcpkv::is_valid_key_prefix("user:"));
    EXPECT_FALSE(tcpkv::is_valid_key_prefix("a b"));
}

TEST(Protocol, CommandTypeNames) {
    EXPECT_STREQ(tcpkv::to_string(CommandType::Set), "SET");
    EXPECT_STREQ(tcpkv::to_string(CommandType::Quit), "QUIT");
}

TEST(Protocol, RequestEncoders) {
    EXPECT_EQ(tcpkv::encode_command({"GET", "k"}), "GET k\n");
    EXPECT_EQ(tcpkv::encode_command({"PING"}), "PING\n");
    EXPECT_EQ(tcpkv::encode_set("k", "hi"), "SET k 2\nhi\n");
    EXPECT_EQ(tcpkv::encode_set("k", ""), "SET k 0\n\n");
}

TEST(Protocol, ReplyEncoders) {
    EXPECT_EQ(reply::status("OK"), "+OK\n");
    EXPECT_EQ(reply::error("boom"), "-ERR boom\n");
    EXPECT_EQ(reply::integer(-2), ":-2\n");
    EXPECT_EQ(reply::integer(0), ":0\n");
    EXPECT_EQ(reply::bulk("ab"), "$2\nab\n");
    EXPECT_EQ(reply::bulk(""), "$0\n\n");
    EXPECT_EQ(reply::null(), "$-1\n");
    EXPECT_EQ(reply::array({"a", "bc"}), "*2\n$1\na\n$2\nbc\n");
    EXPECT_EQ(reply::array({}), "*0\n");
}

TEST(Protocol, EncodedSetRoundTripsThroughParser) {
    const std::string value("\x00\x01\xff binary \n stuff", 18);
    Parser parser;
    parser.feed(tcpkv::encode_set("bin", value));
    const auto cmd = next_command(parser);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->value, value);
}
