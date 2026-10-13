# 07. RPC

**Time:** ~3 hours   **Prerequisites:** 03, 04, 05

## 1. In plain words

**RPC** (remote procedure call) is the idea that calling a function on another machine should look
like calling a function locally: `balance = get_balance("ACC-1")`. Behind that line, a **stub**
packs the arguments into bytes (**serialization**), sends them over a connection, the server
unpacks them, finds the right function in a table (**dispatch**), runs it, packs the result, sends
it back, and the stub unpacks it and returns. Every RPC system from 1984's Sun RPC to gRPC is this
sequence with different choices for each step.

The analogy: ordering from a catalogue by post. You fill in a form (serialize), put your order
number on it (request id), post it (transport), the warehouse looks up the item code (dispatch),
ships it with your order number on the box (correlation), and you match the box to the order.
The post can lose letters, boxes can arrive late, and the warehouse can burn down after charging
your card. RPC's real difficulty is that the function call illusion hides all of that: a local call
cannot "time out with unknown outcome", a remote one can.

You maintain an RPC-over-TCP server. This module gives you the standard words for what it does,
then builds a tiny RPC layer from scratch so each word points at code you wrote.

## 2. The real thing

### 2.1 Anatomy of an RPC

```
 client code     stub          transport        transport      dispatcher     handler
 ----------      ----          ---------        ---------      ----------     -------
 get_balance  -> encode args -> frame + id  ==>  read frame  -> method 3   ->  lookup account
   <- int64   <- decode      <- frame + id  <==  write frame <- status,ret <-  return 100000
```

- **Interface definition**: which methods exist, their ids, their argument and result types. In
  protobuf this is a `.proto` file; in the demo it is `namespace methods { constexpr uint16_t ... }`
  plus comments. Both client and server compile against it.
- **Stub** (client) and **skeleton** (server): generated or hand-written code that converts between
  typed calls and bytes. The demo's `BankClient` methods are stubs; the lambdas in
  `build_dispatcher` are the skeleton.
- **Serialization**: typed values to bytes and back, with a fixed byte order and explicit lengths.
- **Framing**: module 04. The RPC header is a length-prefixed frame.
- **Dispatch**: `method_id` to handler, a table lookup.
- **Correlation**: the response carries the request's id so the client can match them, which is
  what makes several outstanding calls on one connection possible.

### 2.2 Serialization choices

| Approach | Schema | Size | Speed | Evolution | Tooling | Where |
|----------|--------|------|-------|-----------|---------|-------|
| Hand-rolled fixed layout (demo) | in code and comments | smallest | fastest | manual, fragile | none | internal, stable protocols |
| Raw C structs on the wire | the struct | small | memcpy | none: any field change breaks it; padding/endianness tie you to one compiler | none | legacy; probably your server |
| JSON | none or JSON Schema | large (text) | slow (parse, allocate) | easy, unknown fields ignored | universal | HTTP APIs, configs |
| Protocol Buffers | `.proto`, codegen | compact (varints, tags) | fast | excellent: tag numbers, optional fields, unknown fields preserved | protoc, every language | gRPC, Google, most microservices |
| FlatBuffers / Cap'n Proto | schema, codegen | compact-ish | zero-copy read (no parse step) | good | less common | games, low-latency, mmap'd data |
| MessagePack / CBOR | self-describing | compact | fast | like JSON | many | when you want "binary JSON" |
| Apache Thrift / Avro | schema | compact | fast | Avro: reader/writer schema resolution | Hadoop, Kafka ecosystems | data pipelines |

Protobuf's key idea for **evolution**: every field has a number; the wire holds `(number, type,
value)` triples; readers skip unknown numbers and treat missing ones as default. Add fields freely,
never reuse a number, never change a field's type. Your hand-rolled format gets evolution only if
you build the same discipline: a version byte, optional trailing fields, "ignore what you do not
understand".

### 2.3 Request ids and correlation

In a strict one-at-a-time protocol (tcpkv) the Nth reply matches the Nth request and no id is
needed. The moment you want **multiple outstanding calls** on one connection (multiplexing), or
replies that may be **out of order** (a slow and a fast call in flight), each request needs an id
and each reply must carry it. Rules:
- Ids are unique per connection (a counter is fine), not necessarily globally.
- The client keeps a map `id -> pending call (promise, deadline)`.
- A reply with an unknown id (already timed out and removed) is logged and dropped.
- After a timeout you must either keep the id reserved forever on that connection or **close the
  connection**: otherwise a late reply to id 7 could be matched to a new request that reused id 7.
  The demo client closes.

Correlation ids also appear in logs and traces: the request id (or a globally unique trace id)
printed on both client and server lines is how you follow one call through a system (module 09).

### 2.4 Timeouts, retries and the three outcomes

A remote call has **three** outcomes, not two: success, failure, and **unknown** (timeout, or the
connection broke after sending). "Unknown" means the server may have executed it. A retry after
"unknown" may execute it twice. This is the single most important fact about distributed systems
for someone writing a banking server, and it is why module 10 spends time on exactly-once.

Rules that follow:
- **Every call has a deadline.** No deadline means a thread waits forever on a dead peer (module 02:
  15 minutes of kernel retransmission, or forever if idle). Deadlines propagate: if the client gave
  you 2 s, give the database 1.5 s.
- **Classify errors** so the client knows whether to retry: permanent (bad request, unknown method:
  never retry), transient (unavailable, overloaded: retry with backoff), unknown (timeout: retry
  only if idempotent). The `Status` enum in the demo does this; gRPC's status codes do the same
  (`INVALID_ARGUMENT` vs `UNAVAILABLE` vs `DEADLINE_EXCEEDED`).
- **Idempotency** makes retries safe. Natural idempotency: `SET k v`, `GET`, `DELETE`. Forced
  idempotency: the client attaches an **idempotency key** (a unique id for the logical operation,
  such as a transaction id generated at the branch) and the server stores `key -> result`; a
  retried request with a known key returns the stored result without re-executing. The demo's
  `Transfer` does this. Stripe, AWS and every payment API work this way. The store of applied keys
  must be durable and bounded (expire after the client's maximum retry window).
- **Retries need backoff with jitter** (module 09) or a hiccup becomes a storm. Cap the attempts.
  Never retry non-idempotent calls after unknown outcomes without a key.

### 2.5 Backpressure

If clients can send requests faster than the server handles them, something must give:
- At the transport: a server that stops reading makes TCP's window close and the client's `send`
  blocks (module 05). Crude but automatic.
- At the protocol: a **max in-flight** per connection (HTTP/2 `SETTINGS_MAX_CONCURRENT_STREAMS`,
  gRPC flow-control windows). The client does not send request 65 until one of the 64 outstanding
  completes.
- At the application: explicit rejection with `Overloaded` status when queues are full, so the
  client backs off or tries another server (load shedding, module 05 and 09).

A server that buffers everything and never pushes back dies first and takes its clients down with it.

### 2.6 Versioning

Clients and servers are never upgraded at the same instant. Options:
- Version byte in every frame (demo). Server rejects unknown versions with a clear error.
- Version negotiated once at connect (`HELLO v2`), then implied.
- Method-level: new behaviour gets a new method id; old ids keep old semantics forever.
- Field-level: protobuf style; add optional fields, never change meaning.

Rule of thumb: servers must accept old clients (backward compatible) for at least one release;
deploy servers first, then clients. The error for "I do not understand this" must be
distinguishable from "something broke", or clients retry forever (module 04's war story).

### 2.7 gRPC in one page

gRPC = protobuf for serialization + HTTP/2 for transport + generated stubs in a dozen languages.
- **HTTP/2** gives it framing, multiplexing with stream ids (correlation for free), flow control
  per stream (backpressure for free), header compression, and TLS integration.
- Each RPC is one HTTP/2 stream; the method is the path (`/bank.Ledger/GetBalance`), the body is
  length-prefixed protobuf messages.
- Four call shapes: unary (one request, one reply), server streaming, client streaming,
  bidirectional streaming. Streaming is just many messages on one stream.
- Deadlines are first-class (`grpc-timeout` header, propagated to the server which can check
  `context.deadline`). Status codes are standard (`OK`, `INVALID_ARGUMENT`, `NOT_FOUND`,
  `DEADLINE_EXCEEDED`, `UNAVAILABLE`, `UNIMPLEMENTED`, ...). Metadata = headers.
- Costs: HTTP/2 complexity, no browser support without grpc-web, debugging needs `grpcurl` instead
  of `nc`, and head-of-line blocking of all streams on one TCP connection on lossy links.
- When to use: service-to-service calls where you want codegen, streaming, deadlines and
  interoperability. When not: a single Windows client and server you control where a 16-byte
  header does the job and you need no new dependencies.

### 2.8 Mapping your employer's RPC-over-TCP transport

Go through its code with this checklist and write the answers down; they are your interview answers.

| Concept | Question to answer about your server |
|---------|--------------------------------------|
| Framing | Header size? Which field is the length? Is the length validated against a maximum before allocation? |
| Serialization | Raw structs? If so, same compiler/arch both ends? Any byte order handling? Strings: fixed arrays or length-prefixed? |
| Dispatch | How does the server find the handler: switch on an opcode, table, name lookup? |
| Correlation | Is there a request id? Can a client have more than one call outstanding per connection? |
| Timeouts | Client side deadline? Server side per-request deadline? What happens on timeout: close or keep waiting? |
| Idempotency | Which operations are safe to retry? Is there an idempotency key / transaction id for financial postings? |
| Error model | How does a server exception reach the client: status code, error string, connection close? |
| Backpressure | Max outstanding per connection? What happens when workers are all busy: queue, reject, block the socket? |
| Versioning | How did the last protocol change roll out? Can an old client talk to a new server? |
| Concurrency | Who writes to a socket: one thread per connection or a shared mutex around whole-frame writes? |

Being able to say "our transport is a length-prefixed binary protocol with a 24-byte header, opcode
dispatch, one outstanding request per connection, host byte order because both ends are x86
Windows, and a transaction id that the server uses to deduplicate postings" is a senior answer.
Saying "it uses sockets" is not.

## 3. Code

### 3.1 The RPC layer (`code/m07_rpc.hpp`)

Header, 16 bytes, big-endian:

```
magic u16 | version u8 | type u8 | request_id u32 | method_id u16 | status u16 | payload_len u32
```

Serialization primitives:

```cpp
class Writer { void u8/u16/u32/u64/i64(...); void str(string_view) /* u32 len + bytes */; std::string take(); };
class Reader {                      // every read bounds-checked; throws BadPayload
    uint64_t u64() { uint64_t hi = u32(); return (hi << 32) | u32(); }
    std::string str() { uint32_t n = u32(); need(n); std::string s(p_, n); p_ += n; return s; }
    bool done() const;              // handlers check this to reject trailing bytes (schema skew)
};
```

Dispatch and error propagation:

```cpp
using Handler = std::function<Status(Reader& req, Writer& resp)>;

Frame Dispatcher::handle(const Frame& req) const {
    Frame resp; resp.type = Type::Response; resp.request_id = req.request_id;   // correlation
    auto it = table_.find(req.method_id);
    if (it == table_.end()) { resp.status = Status::UnknownMethod; return resp; }  // permanent
    Reader r(req.payload); Writer w;
    try { resp.status = it->second(r, w); resp.payload = w.take(); }
    catch (const BadPayload& e) { resp.status = Status::BadRequest; resp.payload = e.what(); }
    catch (const std::exception& e) { resp.status = Status::Internal; resp.payload = e.what(); }
    return resp;                    // the server never dies because a handler threw
}
```

Client call with a deadline and the "unknown outcome" rule:

```cpp
Result Client::call(uint16_t method_id, std::string payload, int timeout_ms) {
    Frame req{Type::Request, next_id_++, method_id, Status::Ok, std::move(payload)};
    write_frame(fd_.get(), req);
    if (!net::wait_readable(fd_.get(), timeout_ms)) {
        fd_.reset();                                  // a late reply must never match a future id
        return {Status::Timeout, "no reply within deadline; outcome unknown"};
    }
    Frame resp; read_frame(fd_.get(), resp);
    return {resp.status, std::move(resp.payload)};
}
```

### 3.2 The demo (`code/m07_rpc_demo.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread m07_rpc_demo.cpp -o rpc_demo && ./rpc_demo
```

It starts a server on an ephemeral loopback port, registers five methods, and runs a client
through: a normal call, negative integers surviving the wire, a server exception becoming
`INTERNAL` while the server keeps running, an idempotent `Transfer` retried with the same key that
debits once, a malformed payload becoming `BAD_REQUEST`, an unknown method, and a timeout that
leaves the connection closed so the next call returns `UNAVAILABLE`.

The idempotency key in the handler:

```cpp
if (auto done = bank.applied.find(key); done != bank.applied.end()) {
    resp.i64(done->second); return Status::Ok;          // replay: same answer, money moves once
}
... debit, credit ...
bank.applied[key] = bank.balances[from];
```

In a real system `applied` is a table in the same database transaction as the balance update, so
"money moved" and "key recorded" are atomic. That is how a lending server makes a disbursement safe
to retry.

## 4. Under the hood (senior-level)

**Syscalls per call.** Client: one `send` (header and payload in one buffer: no Nagle stall, no
interleaving), one `poll`, one or two `recv` (header then payload; `read_exact` loops). Server: two
`recv`, one `send`. Six to eight syscalls and two round trips on the wire per RPC, which on loopback
is around 20 to 50 microseconds and on a LAN is dominated by the network RTT. Batching several
requests per `send` (pipelining) or `readv`/`writev` reduces syscalls; the request id is what makes
pipelining safe.

**Where time goes in a real RPC stack.** Serialization of small messages: tens to hundreds of
nanoseconds hand-rolled, about a microsecond for protobuf, tens of microseconds for JSON with
allocation. The syscalls and the copy to and from kernel buffers: a few microseconds. The network:
tens of microseconds on a LAN, milliseconds across a WAN. Thread hand-off to a worker: a few
microseconds. For a banking RPC that then does a database transaction (hundreds of microseconds to
milliseconds), the RPC layer is noise. Optimise it only when it is not.

**`std::function` dispatch.** The dispatcher's `unordered_map<uint16_t, std::function>` costs a
hash lookup and an indirect call: tens of nanoseconds. A `switch` or a flat array indexed by method
id is faster and what generated code does. It does not matter until millions of calls per second.

**Exceptions across the boundary.** C++ exceptions cannot cross the wire; the dispatcher converts
them into a status and text. On the client, you can choose to throw on non-Ok status to keep the
"local call" illusion, but the three-outcome problem means callers must still think about
`Timeout`. Many teams return a result type instead of throwing for exactly this reason.

**Thread safety of a connection.** One `Client` object is single-threaded. Sharing a connection
across threads needs: a mutex around `write_frame`, a reader thread that demultiplexes replies by
id into per-call promises, and a map of pending calls. That is 100 more lines and is what every
production RPC client contains; it is the natural next exercise.

**Windows parity.** Nothing in the frame format is OS-specific. The transport helpers map to
Winsock calls one for one (module 03 table). Your server's equivalent of `Dispatcher::handle` is
probably a `switch` on an opcode inside the worker thread's loop.

## 5. In production

- **The double disbursement.** A branch client timed out on a loan disbursement RPC, retried, and
  the customer received the amount twice. The first call had succeeded; the reply was lost when a
  firewall reset the connection. Fix: a transaction id from the client stored with the posting, and
  the server returning the stored result for a repeated id. Every money-moving RPC needs this.
- **The frozen client.** A client with no deadline waited on a server whose worker deadlocked.
  The branch terminal hung until someone killed it. Every call needs a deadline; every deadline
  needs an action (close the connection, report, maybe retry).
- **The mismatched reply.** A client reused a connection after a timeout. The late reply for the
  old request was read as the reply to the new one: a balance query returned the result of a
  different account's query. Close after timeout, or carry ids and discard unknown ones.
- **Struct layout drift.** Server rebuilt with a different packing pragma; client not. Every field
  after the third was misread. Explicit serialization with fixed widths and byte order removes this
  class of incident entirely; a version byte catches it with a clear error when it happens anyway.
- **Retry storm.** A dependency slowed down; clients retried immediately three times; load tripled;
  the dependency fell over completely. Exponential backoff with jitter and a retry budget
  (module 09) turn this into graceful degradation.
- **gRPC's `UNAVAILABLE` after deploys.** Rolling restarts caused in-flight calls to fail with
  `UNAVAILABLE`; clients without retry logic surfaced errors to users. gRPC's built-in retry policy
  (for idempotent methods, configured in the service config) fixed it. The same applies to your
  transport: define which methods are retryable and have the client library honour it.

## 6. Try it

1. **(easy)** Add a `Ping` method (id 6) to the demo with no arguments returning the server's
   monotonic time in milliseconds. Done when: the client prints it.

2. **(easy)** Change `kVersion` to 2 on the server only. Done when: the client fails with a clear
   "unsupported version" error rather than garbage, and you can explain which side detected it.

3. **(medium)** Make `Client` support pipelining: `send_async(method, payload) -> id`, then
   `wait(id, deadline) -> Result`, with a map of pending ids. Send 100 `Add` requests before reading
   any reply. Done when: all 100 results are correct and arrive in one burst.

4. **(medium)** Make the server bound in-flight requests per connection: more than 8 unanswered
   requests gets `Overloaded`. Done when: the pipelined client from exercise 3 sees `OVERLOADED`
   for the excess and succeeds after backing off.

5. **(hard)** Build the multi-threaded client: one writer mutex, one reader thread demultiplexing
   replies into `std::promise`s keyed by id, per-call deadlines enforced by a timer thread that
   fails the promise and marks the connection poisoned. Done when: 8 threads each making 1000 calls
   over one connection get correct results, and a `Sleep(5000)` call times out without disturbing
   the others.

6. **(hard)** Write the same `GetBalance` service with gRPC (`sudo apt install libgrpc++-dev
   protobuf-compiler-grpc`), measure p50 latency against the hand-rolled version on loopback, and
   write a half-page on where the difference comes from and when it would matter.

## 7. Interview questions

**Q: What is RPC and what are its parts?**
A: Making a remote function call look local. Parts: an interface definition, client stub and server
skeleton, serialization of arguments and results, framing over a transport, a request id for
correlation, a dispatch table on the server, and an error/status model that distinguishes permanent
from transient from unknown outcomes.

**Q: What are the three outcomes of a remote call and why does it matter?**
A: Success, failure, and unknown (timeout or broken connection after sending). Unknown means the
server may have executed it, so a blind retry can execute it twice. Money-moving calls need
idempotency keys; the server stores the result per key and replays it on retry.

**Q: Why do you need request ids?**
A: To match replies to requests when more than one call is outstanding on a connection or replies
can be reordered. Also for logs and tracing. After a timeout, either never reuse the id on that
connection or close it, or a late reply can be matched to the wrong call.

**Q: How do you propagate a server exception to the client?**
A: Catch it at the dispatch boundary, convert it to a status code the client can classify (bad
request vs internal) plus a message in the payload, and keep the server alive. The client maps the
status to its own error type or exception. Exceptions never cross the wire.

**Q: Protobuf versus hand-rolled structs?**
A: Protobuf gives schema evolution (numbered fields, unknown fields skipped), codegen in many
languages, compact varint encoding, and tooling. Hand-rolled is smallest and fastest and needs no
dependency, but every change is manual and risky. Raw C structs on the wire are the worst option:
compiler, padding and endianness dependent.

**Q: What does gRPC add over your own TCP protocol?**
A: HTTP/2 transport (multiplexed streams with ids, per-stream flow control, TLS), protobuf codegen
in many languages, streaming calls, standard status codes, first-class deadlines and metadata, and
an ecosystem (load balancing, retries, tracing). Cost: complexity, HTTP/2 head-of-line blocking,
harder manual debugging.

**Q: What is backpressure in an RPC system?**
A: A mechanism that stops a fast producer from overwhelming a slower consumer: TCP windows at the
transport, max-in-flight limits at the protocol, explicit "overloaded" rejections at the
application. Without it, queues grow until memory or latency fails everything.

**Q: Describe your RPC-over-TCP transport.**
A: (Your answer, from the section 2.8 table.) Framing, serialization, dispatch, correlation,
timeouts, idempotency, error model, backpressure, versioning, who writes to the socket.

## 8. Recap

- RPC = stub, serialization, framing, transport, request id, dispatch, status, skeleton.
- Serialization: fix byte order and widths; never ship raw structs; protobuf for evolution and
  codegen; hand-rolled only for small stable internal protocols.
- Request ids enable multiplexing and correlation; after a timeout close the connection or never
  reuse the id.
- Three outcomes: success, failure, unknown. Deadlines everywhere. Classify errors as permanent,
  transient, unknown.
- Idempotency keys make retries safe; store key and result atomically with the effect.
- Backpressure: TCP window, max in-flight, explicit overload rejection.
- Versioning: version byte or handshake; servers accept old clients; clear "unknown" errors.
- gRPC = protobuf + HTTP/2 + codegen + deadlines + standard status codes.
- Describe your own transport with these words; that is the interview answer.
