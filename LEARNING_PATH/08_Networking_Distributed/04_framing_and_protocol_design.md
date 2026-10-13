# 04. Framing and protocol design

**Time:** ~3 hours   **Prerequisites:** 02, 03

## 1. In plain words

TCP hands you a river of bytes with no marks in it. **Framing** is how you put the marks back: a
rule that lets the receiver say "this message ends here, the next one starts there" using only the
bytes themselves. There are three ways to do it. Put the length first ("the next 57 bytes are one
message"). Put a special marker at the end ("a message ends at `\r\n`"). Or make every message the
same size. Everything else in protocol design builds on this choice.

Think of a telegraph operator receiving Morse code all day. Without a convention for where one
telegram ends, the operator cannot deliver anything. "STOP" at the end of each telegram is
delimiter framing. "This telegram is 12 words" is length-prefix framing.

A **protocol** is the complete contract on top of framing: what the messages mean, what replies
are allowed, what errors look like, and how the two sides stay compatible when one is upgraded.
This module designs the tcpkv protocol from scratch, writes a parser that works no matter how TCP
chops the bytes, and looks at how Redis does the same.

## 2. The real thing

### 2.1 Three framing strategies

**Length prefix.** Each message starts with a fixed-size header that includes the payload length.

```
| len: 4 bytes | type: 2 | flags: 2 | payload: len bytes |
```

Pros: binary-safe (payload may contain anything), the receiver knows exactly how much to read,
parsing is O(1) to find the boundary, and you can allocate once. Cons: not human-readable on the
wire, you must fix the byte order of the header, and a corrupted length desynchronises the stream
forever (so always bound it and close on nonsense). This is what almost every RPC transport uses,
including, almost certainly, your employer's.

**Delimiter.** Messages end with a marker, usually `\n` or `\r\n`.

```
GET account:1001\r\n
```

Pros: human-readable, testable with `nc` and `telnet`, trivially extensible. Cons: the payload
cannot contain the delimiter unless you escape it (escaping is a bug factory), the receiver must
scan every byte, and you must cap the line length or an attacker sends 10 GB without a newline.
Used by SMTP, HTTP/1.x headers, Redis inline commands, your tcpkv.

**Fixed size.** Every message is N bytes. Pros: trivial. Cons: wastes space or cannot grow.
Used for some legacy financial protocols and sensor data. Headers inside length-prefixed protocols
are fixed size, so this strategy is always present at a small scale.

**Hybrid (the good answer).** A delimited command line that announces the length of any binary
part that follows. HTTP does this (`Content-Length` header, then that many bytes). Redis RESP does
it (`$5\r\nhello\r\n`). tcpkv does it for `SET`.

### 2.2 Text vs binary

| | Text | Binary |
|-|------|--------|
| Debugging | `nc`, `tcpdump -A`, read it by eye | Need a decoder or Wireshark dissector |
| Size | Larger (digits, spaces, CRLF) | Compact |
| Parsing cost | Scan and convert digits | memcpy and byte swap |
| Extensibility | Add a word | Add a field; need versioning discipline |
| Numeric precision | Decimal strings are exact | Fixed widths; floats need IEEE agreement |
| Where used | HTTP/1, SMTP, Redis, PostgreSQL startup | HTTP/2, gRPC, PostgreSQL data rows, your RPC |

For an internal RPC between your own clients and servers, binary wins on throughput and CPU. For
a tool people operate by hand, text wins. Many systems do text for control and binary for bulk.

### 2.3 Endianness

A 32-bit integer `0x01020304` is stored in memory as bytes `04 03 02 01` on x86 and ARM
(little-endian) and `01 02 03 04` on big-endian machines. The network standard is big-endian,
called **network byte order**. `htonl`/`htons` (host to network, long/short) and `ntohl`/`ntohs`
convert. On x86 they swap bytes; on a big-endian host they are no-ops.

Rules:
- Never `*(uint32_t*)buf`. It violates alignment on some CPUs and strict aliasing in C++. Use
  `memcpy` into a `uint32_t`, then `ntohl`. The compiler turns memcpy of 4 bytes into one load.
- Never send a struct with `send(fd, &s, sizeof s)`. Padding, field order and byte order are all
  compiler and platform dependent. Serialise field by field. (If your employer's protocol does send
  raw structs, it works only because both ends are the same compiler on the same architecture.
  That is a known risk worth naming in an interview.)
- Choose one order for your protocol and write it down. Little-endian is acceptable for an
  internal protocol (Protocol Buffers and FlatBuffers use little-endian); the point is to fix it.

### 2.4 Protocol design checklist

1. **Framing**: how do I find message boundaries? Max sizes for every variable-length part?
2. **Grammar**: exact syntax of every request and response. Write it as a BNF-like list.
3. **Error model**: machine-readable codes plus human text. The client must distinguish "key not
   found" (normal) from "server broke" (retry) from "your request is malformed" (do not retry).
4. **Request-response matching**: in a strict request-response protocol over one connection, the
   Nth reply answers the Nth request (pipelining). If replies can be out of order, every request
   needs an id (module 07).
5. **Versioning**: how does a v2 client talk to a v1 server? Options: a version in the handshake,
   a version in every frame header, "unknown fields are ignored" (protobuf style), or a new port.
6. **Idempotency**: which operations can a client safely retry after a timeout? `SET k v` yes.
   `INCR k` no, unless the client attaches a request id the server remembers (module 07).
7. **Limits and defence**: max line, max value, max in-flight requests, idle timeout. Anything
   unbounded is a denial-of-service hole.
8. **Observability**: can you tell from a packet capture what is going on? Can you count commands
   by type?

### 2.5 The tcpkv protocol

Requests (`\r\n` terminated; a key is 1 to 256 bytes with no bytes <= 0x20 and no 0x7F):

```
PING
GET <key>
SET <key> <len>\r\n<len bytes of value>\r\n
DEL <key>
EXISTS <key>
QUIT
```

Responses:

```
+OK\r\n                      SET succeeded; also +PONG for PING
$<len>\r\n<bytes>\r\n        GET found: bulk value, binary-safe
$-1\r\n                      GET on missing key: nil
:<n>\r\n                     DEL / EXISTS: integer 0 or 1
-<CODE> <message>\r\n        error. CODE is one of:
                             ERR_SYNTAX       malformed request; do not retry as-is
                             ERR_UNKNOWN_CMD  verb not recognised (maybe version skew)
                             ERR_ARGS         wrong argument count/shape
                             ERR_TOO_LARGE    line or value over the limit; server closes
                             ERR_INTERNAL     server-side failure; safe to retry (all ops idempotent)
```

Design decisions and why:
- Text, because the resume line says "text protocol" and because `nc localhost 7000` as a client
  is worth a lot while learning and debugging.
- The value is length-prefixed, not delimited, so binary values with `\r\n` inside work and no
  escaping is needed. This is the hybrid approach.
- Replies start with one type byte (`+ $ : -`), so a client can parse the reply without knowing
  which request it sent. Stolen from Redis.
- Every command is idempotent (SET, DEL, GET, EXISTS). A client that times out can retry anything.
  If you later add `INCR`, you will have to add request ids.
- Errors have codes that classify retry behaviour. That is the single most useful protocol feature
  for the client team.
- Strict request-response, one at a time or pipelined in order. No ids needed.
- Versioning: a future `HELLO <version>` as the first command; servers ignore unknown verbs with
  `ERR_UNKNOWN_CMD`, so an old server tells a new client exactly what is wrong.
- Limits: line 1024 bytes, value 1 MiB, enforced by the parser. Exceeding them is fatal to the
  connection because the server cannot resynchronise safely.

### 2.6 Incremental parsing with a state machine

A **non-incremental** parser assumes it has the whole message. It cannot exist over TCP. An
**incremental** parser keeps state between calls: "I have seen `GET al` so far, waiting for CRLF".

tcpkv needs three states:

```
            CRLF found, verb != SET          complete command
   +------+ ---------------------------->  emit Command
   | Line |
   +------+ ---- CRLF found, verb == SET, len parsed ----+
      ^                                                   v
      |                                             +---------+   len bytes available
      |                                             | Payload | ---------------------+
      |                                             +---------+                      v
      |                                                                      +-------------+
      +-------------- 2 bytes available, must be CRLF -----------------------| PayloadCrlf |
                       (else fatal: lost sync)                               +-------------+
```

Rules the implementation follows:
- `feed()` only appends. `next()` only consumes complete units. Never consume a partial line.
- A `std::string` buffer plus a read offset, compacted when the consumed prefix is large. Erasing
  from the front on every message would be O(n^2).
- Recoverable errors (bad verb, bad args) skip exactly one line and let the stream continue.
  Fatal errors (line too long, length too large, payload not terminated) mark the error `fatal`
  and the caller closes. The distinction: a fatal error means we no longer know where the next
  message begins.
- Limits are checked **before** allocating or buffering. `SET k 9999999999` must be rejected at the
  header, not after trying to read 10 GB.

### 2.7 Fuzzing the parser

A parser is the most attacked and most bug-prone piece of a server. Two cheap, high-value tests:

1. **Random splits**: generate valid command streams, serialise, split the byte string at random
   points (including one-byte chunks), feed, assert the parsed list equals the input. This is the
   TCP property made into a test.
2. **Random garbage**: feed random bytes from a small alphabet including `\r`, `\n`, digits and
   command letters. The parser may error; it must not crash, loop, or grow its buffer unboundedly.

The test file does both with `std::mt19937` and plain `assert`. Beyond that, `libFuzzer`
(`clang++ -fsanitize=fuzzer,address`) finds things random testing misses; the parser's `feed`/`next`
interface is exactly the shape a fuzz target needs. Always run the tests under
`-fsanitize=address,undefined` at least once.

### 2.8 What Redis RESP does and why

RESP (REdis Serialization Protocol) version 2:

```
*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n      array of 3 bulk strings = SET key hello
+OK\r\n                                              simple string
-ERR unknown command 'FOO'\r\n                       error
:42\r\n                                              integer
$5\r\nhello\r\n                                      bulk string
$-1\r\n                                              null bulk
```

Why it is designed this way:
- First byte is the type. The parser branches once and knows the grammar of the rest.
- Lengths are prefixed as decimal text. Binary-safe and still readable; converting a few digits
  is cheap compared to a syscall.
- Requests are arrays of bulk strings, so the command parser never splits on spaces and keys may
  contain spaces. (Redis also accepts "inline" space-separated commands for `telnet` users.)
- No escaping anywhere. Every variable-length thing has an explicit length.
- Simple to implement in every language; a RESP parser is about 100 lines. RESP3 adds maps, sets,
  doubles and push messages while keeping the same shape.

tcpkv is "RESP for replies, inline commands with a length-prefixed value for requests". If you
ever extend it, moving requests to the RESP array form is the natural step.

## 3. Code

### 3.1 The parser (`code/m04_tcpkv_parser.hpp`)

Public interface:

```cpp
enum class Op { Ping, Get, Set, Del, Exists, Quit };
struct Command { Op op; std::string key; std::string value; };
struct ParseError { std::string code, message; bool fatal = false; };
enum class ParseStatus { NeedMore, Ok, Error };

class Parser {
public:
    explicit Parser(size_t max_line = 1024, size_t max_value = 1u << 20);
    void feed(const char* data, size_t len);        // append bytes from recv()
    ParseStatus next(Command& cmd, ParseError& err); // pull one complete command
    size_t buffered() const;
};
```

The server loop that uses it (module 05 has the full version):

```cpp
tcpkv::Parser parser;
char buf[4096];
for (;;) {
    size_t n = net::read_some(fd, buf, sizeof buf);
    if (n == 0) break;                                  // EOF
    parser.feed(buf, n);
    for (;;) {                                          // drain: one recv may hold many commands
        tcpkv::Command cmd; tcpkv::ParseError err;
        auto st = parser.next(cmd, err);
        if (st == tcpkv::ParseStatus::NeedMore) break;
        if (st == tcpkv::ParseStatus::Error) {
            net::write_all(fd, tcpkv::fmt_err(err.code, err.message));
            if (err.fatal) return;                      // cannot resync: close
            continue;
        }
        net::write_all(fd, execute(cmd));
    }
}
```

The heart of `next()`:

```cpp
case State::Line: {
    size_t nl = avail.find("\r\n");
    if (nl == npos) {
        if (avail.size() > max_line_) { err = {"ERR_TOO_LARGE", "line too long", true}; return Error; }
        return NeedMore;                                 // do not consume a partial line
    }
    std::string_view line = avail.substr(0, nl);
    pos_ += nl + 2;
    ParseStatus st = parse_line(line, cmd, err);        // copies out of `line` before compact()
    compact();
    if (st == NeedMore) continue;                       // SET: fall into Payload state
    return st;
}
case State::Payload: {
    if (avail.size() < payload_len_) return NeedMore;   // wait for the whole value
    pending_.value.assign(avail.data(), payload_len_);
    pos_ += payload_len_; state_ = State::PayloadCrlf;
    continue;
}
case State::PayloadCrlf: {
    if (avail.size() < 2) return NeedMore;
    bool ok = avail[0] == '\r' && avail[1] == '\n';
    pos_ += 2; compact(); state_ = State::Line;
    if (!ok) { err = {"ERR_SYNTAX", "payload not terminated by CRLF", true}; return Error; }
    cmd = std::move(pending_); return Ok;
}
```

### 3.2 The tests (`code/m04_tcpkv_parser_test.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_tcpkv_parser_test.cpp -o parser_test && ./parser_test
g++ -std=c++17 -g -fsanitize=address,undefined m04_tcpkv_parser_test.cpp -o parser_test_asan && ./parser_test_asan
```

The random-split fuzz in one picture:

```cpp
std::string wire;  std::vector<Command> expected;
for (...) { Command c = random_command(rng); wire += fmt_cmd(c); expected.push_back(c); }
std::vector<size_t> chunks = random_partition(wire.size(), rng);   // many 1-byte chunks
Emitted e = run(wire, chunks);
assert(e.errs.empty() && e.cmds.size() == expected.size());
for (i...) assert(same(e.cmds[i], expected[i]));
```

### 3.3 Binary framing (`code/m04_framing.cpp`)

```cpp
struct Header { uint32_t length; uint16_t type; uint16_t flags; };   // 8 bytes on the wire

void encode_header(const Header& h, unsigned char out[8]) {
    uint32_t len_be = htonl(h.length); uint16_t type_be = htons(h.type), flags_be = htons(h.flags);
    std::memcpy(out + 0, &len_be, 4);                 // memcpy, never pointer casts
    std::memcpy(out + 4, &type_be, 2);
    std::memcpy(out + 6, &flags_be, 2);
}
Header decode_header(const unsigned char in[8]) { /* memcpy out, ntohl/ntohs */ }
```

The program writes four frames one byte per `send` through a `socketpair` and decodes them with
an incremental `Decoder` that refuses lengths above 16 MiB before allocating. Run it; then change
`write_all(sv[0], &c, 1)` to send the whole wire at once and confirm the decoder does not care.

## 4. Under the hood (senior-level)

**Why the kernel cannot frame for you.** TCP's job is a byte stream; it deliberately knows nothing
about your messages. The `PSH` flag was meant as a hint ("deliver now") but Linux sets it on the
last segment of every `send` and readers ignore it. Record-oriented transports exist (SCTP, SOCK_SEQPACKET
on Unix sockets) but are not usable across the public internet, so every application protocol frames
itself.

**Parsing cost.** A length-prefixed header is parsed with two or three loads and byte swaps, no
scanning: nanoseconds. A delimiter parser must look at every byte (`memchr` is SIMD-accelerated and
runs at tens of GB/s, so this is also cheap in practice). Converting decimal lengths costs a few
nanoseconds per digit. None of this matters next to the syscall and the copy; protocol choice is
about correctness, debuggability and bandwidth, not CPU, until you reach millions of messages per
second per core.

**Buffer management.** The parser keeps one contiguous `std::string` per connection and compacts
lazily. Alternatives: a ring buffer (no copies, but messages may wrap, which complicates
`string_view` handoff), or a list of received chunks with a "gather" step. For a server with 10,000
idle connections, a 4 KB buffer each is 40 MB; sizing the initial buffer small and growing on
demand matters at scale (module 06).

**Zero-copy temptation.** Returning `string_view`s into the receive buffer avoids copying keys and
values, but the view dies on the next `feed`/compact. The parser copies into `Command` strings to
make ownership simple. For a 1 MiB value that is one extra copy; `std::string::assign` runs at
memory bandwidth. Measure before optimising.

**Integer parsing pitfalls.** `atoi`/`strtol` accept leading whitespace, signs, and overflow
silently or saturate. The parser's `parse_uint` accepts only ASCII digits, at most 10 of them, and
nothing else. A length of `-1` or ` 5` or `5x` is rejected. Explicit is safer than a library
function with a permissive grammar.

**Strict aliasing and alignment.** `*(uint32_t*)(buf + 1)` is undefined behaviour in C++ (and
C): the compiler may assume a `uint32_t*` never aliases `char` storage written through a different
type, and on ARM or SPARC the misaligned load traps. `memcpy` is the sanctioned way and compiles
to a single `mov` on x86.

**Windows note.** `htonl`/`ntohl` live in `<winsock2.h>`; the semantics are identical. Windows is
little-endian on every supported architecture, so the same swaps happen.

## 5. In production

- **The "works on LAN, breaks on WAN" bug.** A client parsed the reply with a single `recv` into
  a 4 KB buffer and then searched for `\r\n`. Replies over about 1460 bytes arrived in two segments
  across a WAN; the first `recv` had no `\r\n`, the client reported "protocol error" and
  disconnected. Fix: incremental parser. Every single time.
- **Desynchronisation.** A length-prefixed protocol where the client once sent a payload shorter
  than its declared length (a bug in a rarely used code path). The server read the next request's
  header bytes as payload, then interpreted the following payload bytes as a header: a 2 GB length,
  `bad_alloc`, crash. Fixes: validate lengths against a sane maximum before allocating; on any
  framing inconsistency, close the connection rather than guess. The parser's `fatal` flag is that rule.
- **Escaping disasters.** A delimiter protocol allowed values with the delimiter if "escaped".
  Three different clients implemented escaping three different ways. The fix was a flag day move to
  length-prefixed values. Design the protocol so escaping is unnecessary.
- **Version skew during rolling upgrade.** New clients sent a new command; old servers replied
  with a generic error the client treated as "retry"; the clients retried in a tight loop and
  overwhelmed the servers. The fix: an `ERR_UNKNOWN_CMD` code that clients treat as permanent, and
  deploying servers before clients. Codes that classify retry behaviour are not optional.
- **Your RPC transport.** Look at its header: how many bytes, which field is the length, byte
  order (probably host order because both ends are x86 Windows), max length check (is there one?),
  what happens on a bad header (does it close or try to continue?). Being able to describe your
  own protocol with this module's vocabulary is a strong interview signal.
- **Debugging with `nc` and `tcpdump -A`.** A text protocol lets you type `GET account:1001` into
  `nc` and see the reply. With `tcpdump -A -i lo port 7000` you see every request and response in
  ASCII. This is worth real money at 3 AM, and is a legitimate argument for text protocols on
  internal tools.

## 6. Try it

1. **(easy)** Build and run `parser_test` and `framing`. Then run `parser_test` under
   `-fsanitize=address,undefined`. Done when: both pass clean.

2. **(easy)** Add a `KEYS <prefix>` request to the grammar (reply: `*<n>\r\n` followed by `n` bulk
   strings). Write the grammar lines and the formatting function. Done when: `fmt_keys({"a","bb"})`
   produces `*2\r\n$1\r\na\r\n$2\r\nbb\r\n`.

3. **(medium)** Write `ReplyParser`: an incremental parser for tcpkv **replies** (`+`, `-`, `:`,
   `$`), with the same `feed`/`next` shape, and test it with random splits. Done when: a 1000-reply
   stream fed byte-by-byte parses identically to being fed at once.

4. **(medium)** Make the parser tolerant of multiple spaces between tokens and of a bare `\n`
   terminator, without breaking any existing test. Done when: all tests plus three new ones pass,
   and you can state what ambiguity (if any) the relaxed grammar introduced.

5. **(hard)** Write a libFuzzer target `LLVMFuzzerTestOneInput` for the parser that splits the
   input at positions derived from the input bytes and feeds the rest. Run it for ten minutes with
   `clang++ -fsanitize=fuzzer,address,undefined`. Done when: either no crash, or you fixed one.

6. **(hard)** Design a binary version of tcpkv: header `{u32 len, u8 version, u8 opcode, u16 flags}`,
   request body for each opcode, reply body including a `u16 status`. Write the design as a one-page
   spec with every field's size and byte order, and list what becomes easier and harder compared
   to the text version. Done when: a colleague could implement a client from the spec alone.

## 7. Interview questions

**Q: How do you know where a message ends when reading from a TCP socket?**
A: TCP does not tell you; the protocol must. Either a length prefix (read the header, then exactly
that many bytes), a delimiter (scan for it, with a maximum length), or fixed-size messages. Most
real protocols combine a delimited or fixed header with length-prefixed bodies.

**Q: Length prefix or delimiter: when would you pick each?**
A: Length prefix for binary payloads, high throughput, and when the receiver should allocate once;
you must validate the length before using it. Delimiter for human-operable text protocols where
`nc` debugging matters; you must cap the line length and avoid needing to escape the delimiter.

**Q: Why must you not send a struct directly with `send(&s, sizeof s)`?**
A: Padding, alignment, field order and byte order depend on the compiler and architecture. It
works only when both ends are identical builds. Serialise fields explicitly with a fixed byte order
using `memcpy` and `htonl`/`htons`, never pointer casts.

**Q: What is network byte order and what do `htonl`/`ntohl` do?**
A: Big-endian: most significant byte first. `htonl` converts a 32-bit host value to that order
(byte swap on x86, no-op on big-endian hosts); `ntohl` converts back. Fix one byte order per
protocol and apply it to every multi-byte integer on the wire.

**Q: What does an incremental parser need that a normal one does not?**
A: Explicit state between calls (which part of a message it is in and how many bytes it still
needs), a buffer of unconsumed bytes, the rule "never consume a partial unit", and bounded limits
checked before allocation. It must produce identical output for any chunking of the same bytes.

**Q: How do you handle a malformed request without killing the connection?**
A: If you still know where the next message starts (bad verb on a complete line), reply with a
coded error and continue. If you do not (bad length, missing terminator after a payload, oversize
line), reply if you can and close, because guessing risks interpreting payload as headers.

**Q: Why does Redis RESP prefix bulk strings with their length if it is a text protocol?**
A: To be binary-safe without escaping while staying readable. The first byte gives the type, a
decimal length gives the size, then raw bytes. A parser is a few dozen lines in any language.

## 8. Recap

- Framing restores message boundaries TCP erased: length prefix, delimiter, fixed size, or hybrid.
- Binary-safe values need lengths, not delimiters. Avoid escaping by design.
- Serialise field by field with `memcpy` and `htonl`/`htons`. Never ship raw structs or cast pointers.
- A protocol spec covers grammar, error codes that classify retryability, request matching,
  versioning, idempotency, limits.
- tcpkv: `GET/SET/DEL/EXISTS/PING/QUIT`, replies typed by first byte (`+ $ : -`), SET value
  length-prefixed, all commands idempotent, line 1 KB and value 1 MiB limits.
- Incremental parser = state machine + unconsumed buffer + "never consume a partial unit" +
  limits checked before allocation.
- Recoverable error: skip one line and continue. Fatal error: lost sync, close the connection.
- Test with random splits (same bytes, any chunking, same result) and random garbage (no crash,
  bounded memory). Run under ASan/UBSan.
- RESP: type byte, decimal lengths, no escaping, trivial to implement; tcpkv borrows its replies.
