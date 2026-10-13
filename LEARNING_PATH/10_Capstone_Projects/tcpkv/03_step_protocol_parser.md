# Step 03. Protocol parser: incremental state machine, with malformed-input tests

**Time:** ~8 hours   **Week:** 2 (Oct 13 – Oct 19)
**Read first:** 02_Modern_CPP: `std::string_view`, `std::optional`, `std::variant` and
`std::get_if`, `std::from_chars`, enums with `enum class`, `inline constexpr`, namespaces.
08_Networking_Distributed: why TCP is a byte stream (message boundaries do not exist;
one `read()` may return half a command or three commands).

## 1. Goal

A `Parser` that you feed whatever bytes `read()` returned and that hands back complete
commands, one at a time, never blocking and never touching a socket. Plus the encoders for
requests and replies, and the limits from `00_spec.md` section 3.5. Plus a test for every
malformed input the spec lists.

In plain words: TCP gives you a hose of bytes with no markers. The parser is the thing that
cuts the hose into commands. Because it only ever sees bytes, you can test every nasty case
(a command split at every possible byte, a 2 GB length, a missing newline) in microseconds
without a network.

## 2. Design

```
         feed(bytes)                         next()
  socket ----------->  buffer_ [pos_ ....]  ----------> Command | ParseError | nullopt (need more)

  State machine:
    Line  --"SET k n" header-->  Value  --n bytes + '\n'-->  Line   (emit Command)
    Line  --any other line---->  Line   (emit Command or non-fatal ParseError)
    any   --fatal error------>  Dead   (emit fatal ParseError once; then nullopt forever)
```

* `feed()` only appends. `next()` does all the work and is called in a loop until it
  returns `nullopt`. This separation is what makes pipelining free: one `read()` of
  `"PING\nPING\nGET a\n"` yields three commands.
* The buffer is a `std::string` plus a read offset `pos_`. Consumed bytes are erased lazily
  (`compact()`), only when the offset is large or everything is consumed, so the common
  case is amortised O(1) per byte.
* Errors are data (`ParseError{message, fatal}`), not exceptions. A bad command from a
  client is a normal event, not an exceptional one, and the server must keep running.
* `fatal` splits the errors the spec lists: after an unknown command we know the next line
  starts a new request; after `SET k 999999999999` we do not know where the next request is.
* The SET `length` token is consumed by the parser and *not* passed on in `args`; the
  executor gets `args = {key}` and `value`. One less thing for the executor to parse.

## 3. Files to create

### `include/tcpkv/protocol.hpp`

Identical to `code/include/tcpkv/protocol.hpp`. Key parts:

```cpp
namespace limits {
inline constexpr std::size_t kMaxKeyLength = 256;
inline constexpr std::size_t kMaxValueLength = 1024 * 1024;  // 1 MiB
inline constexpr std::size_t kMaxLineLength = 4096;
inline constexpr std::size_t kMaxArgs = 64;
}  // namespace limits

enum class CommandType { Ping, Get, Set, Del, Exists, Keys, Expire, Ttl, Stats, Quit };

struct Command {
    CommandType type = CommandType::Ping;
    std::vector<std::string> args;  // everything after the command name, SET's length excluded
    std::string value;              // SET only
};

struct ParseError {
    std::string message;
    bool fatal = false;
};

using ParseEvent = std::variant<Command, ParseError>;

class Parser {
public:
    void feed(const char* data, std::size_t len);
    void feed(std::string_view data) { feed(data.data(), data.size()); }
    std::optional<ParseEvent> next();
    std::size_t buffered() const noexcept { return buffer_.size() - pos_; }
    bool dead() const noexcept { return state_ == State::Dead; }

private:
    enum class State { Line, Value, Dead };
    std::optional<ParseEvent> parse_line(std::string_view line);
    std::optional<ParseEvent> fail(std::string message, bool fatal);
    void compact();

    State state_ = State::Line;
    std::string buffer_;
    std::size_t pos_ = 0;
    Command pending_;
    std::size_t value_len_ = 0;
};
```

`inline constexpr` (C++17) lets a constant live in a header with one definition across all
translation units. `std::variant` is a type-safe union: an event is *either* a command *or*
an error, and `std::get_if` makes the caller handle both.

All ten command types are recognised now, including EXPIRE, TTL and STATS, even though the
store does not implement TTL until step 09. The parser is the protocol; it should be
complete and tested in one go. The executor will answer `-ERR not implemented yet` for them
until step 09.

### `src/protocol.cpp`

Identical to `code/src/protocol.cpp` (about 330 lines). The heart of it:

```cpp
std::optional<ParseEvent> Parser::next() {
    for (;;) {
        switch (state_) {
            case State::Dead:
                return std::nullopt;

            case State::Line: {
                const std::size_t newline = buffer_.find('\n', pos_);
                if (newline == std::string::npos) {
                    if (buffered() > limits::kMaxLineLength) {
                        return fail("line too long", /*fatal=*/true);
                    }
                    compact();
                    return std::nullopt;
                }
                std::string_view line(buffer_.data() + pos_, newline - pos_);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                pos_ = newline + 1;
                if (line.size() > limits::kMaxLineLength) {
                    return fail("line too long", /*fatal=*/true);
                }
                if (auto event = parse_line(line)) {
                    return event;
                }
                break;  // blank line or SET header: loop and look at the next thing
            }

            case State::Value: {
                const std::size_t available = buffer_.size() - pos_;
                if (available < value_len_ + 1) {
                    compact();
                    return std::nullopt;
                }
                const char* data = buffer_.data() + pos_;
                std::size_t consumed = 0;
                if (data[value_len_] == '\n') {
                    consumed = value_len_ + 1;
                } else if (data[value_len_] == '\r') {
                    if (available < value_len_ + 2) {
                        compact();
                        return std::nullopt;
                    }
                    if (data[value_len_ + 1] != '\n') {
                        return fail("value not terminated by newline", /*fatal=*/true);
                    }
                    consumed = value_len_ + 2;
                } else {
                    return fail("value not terminated by newline", /*fatal=*/true);
                }
                pending_.value.assign(data, value_len_);
                pos_ += consumed;
                state_ = State::Line;
                Command command = std::move(pending_);
                pending_ = Command{};
                return ParseEvent(std::move(command));
            }
        }
    }
}
```

Things to notice:

* `line` is a `string_view` *into* `buffer_`. That is only safe because nothing modifies
  `buffer_` between creating the view and finishing `parse_line` (which copies the tokens
  into `Command::args`). `compact()` runs only on the "need more" path, after the view is dead.
* The `for (;;)` with `break` inside the switch is how a blank line or a SET header continues
  to the next line without recursion.
* `pending_ = Command{};` after the move: a moved-from `std::string` is valid but unspecified;
  resetting it keeps the next SET from inheriting stale state.

Integer parsing uses `std::from_chars`:

```cpp
bool parse_unsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    // from_chars rejects leading '+', whitespace and (for unsigned types) '-': exactly the
    // strictness a wire protocol wants.
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}
```

Why not `std::stoul` or `atoi`? `atoi("12abc")` returns 12; `stoul("-1")` returns a huge
number; both accept leading whitespace and `+`. `from_chars` does none of that, does not
allocate, does not throw, and reports overflow. Checking `ptr == end` rejects trailing junk.

### `tests/protocol_test.cpp`

Identical to `code/tests/protocol_test.cpp` (about 330 lines, 36 tests). The pattern:

```cpp
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
```

The byte-by-byte test is the most important one in the file. It simulates the worst TCP can
do (every `read()` returns one byte) and proves the state machine resumes correctly from
every possible split point.

The malformed-input section covers each error from the spec: unknown command (non-fatal,
parsing continues with the next line), every wrong arity, invalid keys (too long, control
character, UTF-8), bad SET lengths (`abc`, `-1`, `+5`, `5x`), oversized value (fatal; parser
dead; further input ignored), 4097 bytes without newline (fatal) vs exactly 4096 (not
fatal, just waiting), value not followed by newline (fatal), 65 arguments, non-integer
EXPIRE seconds. Plus the encoders round-tripping binary data through the parser.

### `CMakeLists.txt`

Add `src/protocol.cpp` to the library and `protocol` to the test `foreach`.

## 4. Build and run

```bash
cmake --build --preset debug && ctest --preset debug
cmake --build --preset asan  && ctest --preset asan
cmake --preset ubsan && cmake --build --preset ubsan && ctest --preset ubsan
```

Expected: 55 tests pass under all three. UBSan matters here: a parser is where off-by-one
indexing (`data[value_len_]` when `available == value_len_`) would show up as an
out-of-bounds read. The `available < value_len_ + 1` check prevents it; UBSan with
`-fsanitize=undefined` does not check std::string bounds, but ASan does.

## 5. Test it by hand

Write a 20-line throwaway program (do not commit it) that reads stdin in 1-byte chunks, feeds
the parser, and prints each event. Pipe the spec's session example into it. Then pipe
`/dev/urandom | head -c 100000` into it and confirm it never crashes and ends either with a
fatal error or waiting for more. That is a free fuzz test.

## 6. Commit

```bash
git add -A
git commit -m "Add incremental protocol parser, encoders and limits with 36 tests

- Parser: Line/Value/Dead state machine over a buffer with lazy compaction;
  feed() never blocks, next() yields Command | ParseError | need-more
- length-prefixed SET values (binary safe), CRLF tolerant, blank lines ignored
- fatal vs non-fatal errors per the spec; dead parser drops input
- std::from_chars for all integers; key alphabet 0x21..0x7E, 1..256 bytes
- reply encoders: status/error/integer/bulk/null/array
- tests: byte-by-byte split, pipelining, every malformed case, round trips"
git push
```

## 7. What you learned, and what to say in an interview

* **TCP is a stream.** A parser for a TCP protocol must be resumable at any byte. The
  byte-by-byte test is how you prove it.
* **Separate parsing from I/O.** The parser never sees a socket, so 36 tests run in
  milliseconds with no ports, no threads, no flakiness.
* **Errors as values** for expected failures (bad client input); exceptions for unexpected
  ones (the kernel refused a syscall).
* **Fatal vs recoverable** is a protocol design decision: can the server resynchronise?
* **Length prefix beats escaping** for binary safety, O(1) framing and pipelining.
* **`from_chars`** is the correct way to parse integers off the wire; `atoi` is a bug.

Interview sentence: "The parser is a three-state machine over a byte buffer. I tested it by
feeding a SET command one byte at a time and asserting exactly one command comes out, and by
feeding every malformed input the spec names and asserting the exact error and whether the
connection survives it."
