# 08. HTTP and REST

**Time:** ~3 hours   **Prerequisites:** 03, 04

## 1. In plain words

HTTP is a text protocol over TCP where a client sends a **request** ("GET me the resource at
`/accounts/1001`") and the server sends one **response** ("200 OK, here are 212 bytes of JSON").
It was built for web pages but became the universal transport for APIs because every language,
proxy, firewall and debugging tool understands it. If you can write the tcpkv parser, you can
write an HTTP/1.1 parser: a request line, header lines, a blank line, and an optional body whose
length is announced by a header.

**REST** is not a protocol but a style for designing HTTP APIs: things are **resources** with URLs
(`/loans/4471`), you act on them with the standard methods (`GET` read, `PUT` replace, `DELETE`
remove, `POST` create or do something), the server tells you what happened with standard status
codes, and each request carries everything needed to process it (no server-side session state).

This module reads HTTP/1.1 at the byte level, writes a small server by hand in C++17, and then
explains what HTTP/2, HTTP/3 and TLS change, and when to stop writing it yourself.

## 2. The real thing

### 2.1 HTTP/1.1 message format

Request:

```
GET /kv/greeting HTTP/1.1\r\n            request line: METHOD SP request-target SP version
Host: 127.0.0.1:8080\r\n                 headers: name ":" value, names case-insensitive
User-Agent: curl/8.5.0\r\n
Accept: */*\r\n
\r\n                                     blank line ends the head
                                         body (none for GET)
```

Response:

```
HTTP/1.1 200 OK\r\n                      status line: version SP code SP reason
Date: Fri, 03 Oct 2026 10:00:00 GMT\r\n
Content-Type: application/json\r\n
Content-Length: 35\r\n                   body length in bytes: this is the framing
Connection: keep-alive\r\n
\r\n
{"key":"greeting","value":"hello"}
```

Framing of the body, in order of precedence: `Transfer-Encoding: chunked` (section 2.5), else
`Content-Length: N`, else (responses only) read until the server closes. A request with a body and
neither header is invalid (`411 Length Required`). `Content-Length` must be validated like any
length off the wire: digits only, bounded.

### 2.2 Methods

| Method | Meaning | Safe | Idempotent | Body |
|--------|---------|------|------------|------|
| GET | read a resource | yes | yes | no |
| HEAD | GET without the body (headers only) | yes | yes | no |
| PUT | replace the resource at this URL with the body | no | yes | yes |
| DELETE | remove the resource | no | yes | no |
| POST | create a subordinate resource or trigger an action | no | no | yes |
| PATCH | partial update | no | not necessarily | yes |
| OPTIONS | what methods are allowed (used by CORS preflight) | yes | yes | no |

**Safe** means no side effects (caches and crawlers may call it freely). **Idempotent** means
repeating has the same effect as doing it once: `PUT /kv/a` with `hello` twice leaves one `hello`;
`DELETE` of a missing resource is still "gone" (return 204 or 404, both acceptable). `POST` is the
non-idempotent one, which is why payment APIs add an `Idempotency-Key` header to POST (module 07).

### 2.3 Status codes

- **1xx** informational: `100 Continue` (client asked `Expect: 100-continue` before sending a big body).
- **2xx** success: `200 OK`, `201 Created` (with a `Location` header), `202 Accepted` (async, not
  done yet), `204 No Content` (success, nothing to return, no body allowed).
- **3xx** redirection: `301` permanent, `302`/`307` temporary, `304 Not Modified` (conditional GET hit).
- **4xx** client error (do not retry as-is): `400 Bad Request`, `401 Unauthorized` (not
  authenticated), `403 Forbidden` (authenticated, not allowed), `404 Not Found`, `405 Method Not
  Allowed`, `409 Conflict` (version mismatch, duplicate), `413 Payload Too Large`, `422
  Unprocessable Entity` (valid syntax, invalid semantics), `429 Too Many Requests` (rate limited;
  send `Retry-After`).
- **5xx** server error (may retry with backoff): `500 Internal Server Error`, `502 Bad Gateway`
  (proxy got a bad upstream response), `503 Service Unavailable` (overloaded or down; `Retry-After`),
  `504 Gateway Timeout`.

The 4xx/5xx split is the retry classification from module 07 built into the protocol.

### 2.4 Headers you must know

`Host` (required in 1.1; virtual hosting), `Content-Type` (`application/json; charset=utf-8`),
`Content-Length`, `Transfer-Encoding`, `Connection` (`keep-alive`/`close`), `Accept` and
`Accept-Encoding` (content negotiation), `Content-Encoding` (`gzip`), `Cache-Control`, `ETag` and
`If-None-Match` (conditional requests, optimistic concurrency: `If-Match: "v7"` on PUT returns 412
if the resource changed), `Authorization` (`Bearer <token>`), `Location`, `Retry-After`,
`X-Request-Id` (correlation; module 09), `Date`.

Header rules: names are case-insensitive, values may have leading and trailing whitespace trimmed,
repeated headers combine with commas (except `Set-Cookie`), no whitespace between name and colon
(a smuggling vector), and you must cap total header size (8 to 64 KB is common).

### 2.5 Keep-alive, pipelining and chunked encoding

**Keep-alive**: HTTP/1.0 closed the connection after every response. HTTP/1.1 keeps it open by
default; `Connection: close` ends it. One TCP connection then carries many sequential requests,
saving a handshake and slow start each time. Servers apply an idle timeout (5 to 60 s) and clients
must handle the server closing an idle connection (retry the request on a new connection if it was
idempotent and no bytes of a response were received).

**Pipelining**: sending the next request before the previous response arrives. Legal in 1.1,
responses must come back in order; in practice browsers disabled it (head-of-line blocking,
buggy proxies). HTTP/2 multiplexing replaced it. Your server gets it for free if its read loop keeps
unconsumed bytes, as the code does.

**Chunked transfer encoding**: for bodies whose length is unknown when the headers are sent
(streaming a generated report). Each chunk is `hex-size\r\n data\r\n`, terminated by `0\r\n\r\n`
(optionally with trailers):

```
HTTP/1.1 200 OK\r\n
Transfer-Encoding: chunked\r\n
\r\n
7\r\n
{"a":1}\r\n
8\r\n
,{"b":2}\r\n
0\r\n
\r\n
```

A chunked parser is another small state machine (size line, data, CRLF, repeat, trailers). The
demo server does not implement it for requests and returns 411; for responses it always knows the
length.

### 2.6 Content negotiation

The client says what it can accept (`Accept: application/json`, `Accept-Encoding: gzip, br`,
`Accept-Language: en`), the server picks and states its choice (`Content-Type`,
`Content-Encoding`, `Content-Language`, plus `Vary: Accept-Encoding` so caches key on it). If
nothing matches: `406 Not Acceptable`. Most internal APIs skip this and always speak JSON; public
APIs should honour `Accept-Encoding: gzip` because JSON compresses 5 to 10 times.

### 2.7 HTTP/2 in a paragraph

HTTP/2 (2015) keeps HTTP semantics (methods, status codes, headers) and replaces the text framing
with a binary framing layer over one TCP connection: **streams** identified by ids carry
**frames** (HEADERS, DATA, SETTINGS, WINDOW_UPDATE, PING, GOAWAY), many streams are **multiplexed**
on one connection so requests and responses interleave without ordering constraints, headers are
compressed with HPACK (a shared dynamic table, so repeated `Authorization` headers cost two bytes),
each stream has flow control (per-stream backpressure), and priorities hint at ordering. It
eliminates HTTP/1's six-connections-per-host workaround and head-of-line blocking at the HTTP layer,
but inherits TCP's: one lost packet stalls every stream. It is almost always run over TLS
(negotiated via ALPN), and gRPC is built on it. Debug it with `curl --http2 -v` or Wireshark.

### 2.8 HTTP/3 in a paragraph

HTTP/3 (2022) moves the same HTTP/2 concepts onto **QUIC**, a transport over UDP that implements
its own reliability, congestion control, and **per-stream** ordering, so a lost packet stalls only
the stream it belonged to. QUIC integrates TLS 1.3 into its handshake, giving a 1-RTT (or 0-RTT on
resumption) connection setup instead of TCP's handshake plus TLS's. It supports connection
migration (a phone switching from Wi-Fi to mobile keeps the connection). Costs: user-space
implementations use more CPU than kernel TCP, UDP is sometimes blocked or rate-limited by
middleboxes (clients fall back to HTTP/2), and operational tooling is younger. Header compression
is QPACK, adapted for out-of-order delivery. Browsers and CDNs use it heavily; internal
service-to-service traffic mostly still uses HTTP/2.

### 2.9 TLS handshake basics

TLS sits between TCP and HTTP (HTTPS = HTTP over TLS, port 443). TLS 1.3 handshake:

```
Client -> ClientHello: supported versions, cipher suites, key share (ECDHE public key), SNI (host name), ALPN (h2, http/1.1)
Server -> ServerHello: chosen suite, its key share; then (encrypted) Certificate, CertificateVerify, Finished
Client -> Finished; application data begins          (1 round trip; 0-RTT possible on resumption)
```

Both sides derive the same symmetric keys from the ECDHE exchange (forward secrecy: a stolen
server private key later cannot decrypt recorded traffic). The certificate, signed by a CA the
client trusts, proves the server owns the name in SNI. Mutual TLS adds a client certificate: this
is how services authenticate each other inside a data centre. TLS 1.2 took two round trips and
allowed weaker options; disable everything below 1.2. In C++ you use OpenSSL (`SSL_CTX`, `SSL_read`,
`SSL_write` replace `recv`/`send` and handle the record layer), or let a library (Beast, httplib
with `CPPHTTPLIB_OPENSSL_SUPPORT`) do it. Common production failures: expired certificates,
missing intermediate certificates in the chain, hostname mismatch, clock skew on the client.

### 2.10 REST resource design

- Nouns, not verbs, in URLs: `/loans/4471`, not `/getLoan?id=4471`. Collections are plural:
  `/customers/88/loans`.
- Use the methods for the verbs: `GET /loans/4471`, `POST /loans` (create; reply `201` with
  `Location: /loans/4472`), `PUT /loans/4472` (full replace), `PATCH` (partial), `DELETE`.
- Actions that do not map to CRUD become sub-resources: `POST /loans/4471/disbursements` creates a
  disbursement; its state is `GET /loans/4471/disbursements/9`. Avoid `POST /loans/4471/disburse`
  unless you accept the RPC-ish style (many good APIs do; be consistent).
- Status codes carry meaning; the body carries detail: `{"error":{"code":"INSUFFICIENT_FUNDS",
  "message":"...","request_id":"..."}}`. RFC 9457 "Problem Details" standardises this shape.
- Idempotency: `PUT` and `DELETE` naturally; `POST` with an `Idempotency-Key` header.
- Versioning: `/v1/loans` in the path (simple, visible) or `Accept: application/vnd.bank.v2+json`
  (purer, invisible). Pick path versioning unless you have a reason.
- Pagination: `GET /loans?limit=100&cursor=abc`; return `next_cursor`. Avoid `offset` for large
  tables (slow and inconsistent under inserts).
- Filtering and sorting as query parameters; never put secrets in URLs (they land in logs).
- Concurrency control: `ETag` on GET, `If-Match` on PUT; `412 Precondition Failed` on mismatch.
  This maps directly to an optimistic version column in your database rows.
- Long operations: `202 Accepted` with a `Location` of a status resource the client polls.

### 2.11 When to use a library

Writing the parser by hand teaches you the protocol and is fine for an internal tool. For
anything that faces other teams or the internet, use a library: the edge cases (chunked requests,
`Expect: 100-continue`, header folding, percent-decoding, request smuggling defences, TLS, HTTP/2)
are where hand-written servers get exploited.

- **cpp-httplib**: single header, blocking, thread pool, client and server, optional OpenSSL. Ten
  lines to a working JSON API. Good for internal services and tools. Not for 10K connections.
- **Boost.Beast**: HTTP/1 and WebSocket on top of Asio, asynchronous, you control the event loop.
  More code, more scalability. Pairs with module 06.
- **Drogon**, **userver**, **Pistache**, **Crow**: fuller frameworks (routing, JSON, ORM).
- **nghttp2**, **libcurl** (client side; `curl` is libcurl), **gRPC** for HTTP/2.
- Behind a reverse proxy (nginx, Envoy) your server can speak plain HTTP/1.1 and let the proxy do
  TLS, HTTP/2, compression and rate limiting. This is the common production layout.

## 3. Code

### 3.1 The server (`code/m08_http_server.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m08_http_server.cpp -o http_server
./http_server 8080 &
curl -i http://127.0.0.1:8080/health
curl -i -X PUT --data-binary 'hello "world"' http://127.0.0.1:8080/kv/greeting      # 201
curl -i -X PUT --data-binary 'hello again'   http://127.0.0.1:8080/kv/greeting      # 204 (idempotent)
curl -s http://127.0.0.1:8080/kv/greeting | python3 -m json.tool
curl -i -X DELETE http://127.0.0.1:8080/kv/greeting                                 # 204
curl -i http://127.0.0.1:8080/kv/greeting                                           # 404
curl -v http://127.0.0.1:8080/health http://127.0.0.1:8080/stats 2>&1 | grep -E 'Re-using|Connected'  # keep-alive
curl -i --http1.0 http://127.0.0.1:8080/health                                      # Connection: close
printf 'GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET /stats HTTP/1.1\r\nHost: x\r\n\r\n' | nc -q1 127.0.0.1 8080  # pipelined
printf 'GARBAGE\r\n\r\n' | nc -q1 127.0.0.1 8080                                    # 400, closed
```

Reading the head, then the body, keeping leftovers for the next request:

```cpp
while ((head_end = buf.find("\r\n\r\n")) == std::string::npos) {   // head = up to the blank line
    if (buf.size() > kMaxHeaderBytes) { reply 400; return; }         // bounded, like every parser
    size_t n = net::read_some(fd, tmp, sizeof tmp);
    if (n == 0) return;
    buf.append(tmp, n);
}
parse_head(std::string_view(buf).substr(0, head_end), req);
buf.erase(0, head_end + 4);
... validate Content-Length (digits, <= 1 MiB) ...
while (buf.size() < content_length) { read more }
req.body = buf.substr(0, content_length); buf.erase(0, content_length);   // leftovers = next request
```

Persistence rule, straight from RFC 7230:

```cpp
if (req.version == "HTTP/1.1") req.keep_alive = conn_v.find("close") == npos;        // default on
else                            req.keep_alive = conn_v.find("keep-alive") != npos;   // 1.0: default off
```

The response always carries `Content-Length` (or is 204), so the client knows exactly where it
ends and the connection can be reused. On any parse error the server replies and closes, because
it no longer knows where the next request starts: the same rule as the tcpkv parser's `fatal`.

### 3.2 Testing with curl

`-i` show response headers; `-v` show request and response headers and connection reuse; `-X PUT`
method; `--data-binary @file` body from a file; `-H 'Content-Type: application/json'` header;
`-s` silent; `-w '%{http_code} %{time_total}\n'` print code and timing; `--http1.0`; `--http2`;
`-k` ignore TLS errors (debugging only); `--resolve host:443:10.1.2.3` to test a server by IP with
the right `Host`/SNI. `curl` plus `python3 -m json.tool` is the REST developer's `nc`.

## 4. Under the hood (senior-level)

**Why HTTP/1.1 is slow for many small requests.** Each request waits for the previous response
(no multiplexing), so latency is RTT times the number of sequential requests. Browsers open six
connections per host to work around it, which multiplies handshakes and slow-start phases.
Request and response headers are repeated verbatim every time (cookies, user agents: hundreds of
bytes). HTTP/2 fixes both with multiplexed streams and HPACK; the TCP-level head-of-line blocking
remains and HTTP/3 removes it.

**Where the server's time goes.** For a small JSON response: `recv` (a few microseconds), parsing
(a microsecond with `string_view`, more if you allocate per header), the handler, formatting
(string concatenation: allocation), `send`. A hand-written server like the demo does roughly
50,000 to 100,000 requests per second per core on loopback with keep-alive (typical for this
class of code; measure with `wrk` or `ab`), and perhaps 5,000 per second without keep-alive because
each request then pays a handshake, a thread creation and a TIME_WAIT. That ratio is the whole
argument for keep-alive and connection pooling.

**`Date` header and `strftime`.** `gmtime_r` plus `strftime` costs about a microsecond; nginx
caches the formatted date and refreshes it once per second. Small things like this are where
"fast HTTP server" work goes.

**Request smuggling.** When a proxy and a backend disagree on where a request ends (one honours
`Content-Length`, the other `Transfer-Encoding`; or a header with whitespace before the colon is
parsed differently), an attacker can hide a second request inside the first. Defences: reject
requests with both length headers, reject malformed header names, normalise before forwarding,
close the connection on any ambiguity. The demo's strictness (411 on chunked, digits-only
`Content-Length`, no space before colon) is this defence in miniature.

**TLS cost.** The 1.3 handshake costs one RTT plus an ECDHE operation (tens of microseconds of CPU)
and a signature verification; session resumption skips most of it. Bulk encryption with AES-GCM
runs at gigabytes per second with AES-NI. TLS is not a performance excuse in 2026; terminating it
at a proxy is about operational convenience (certificates in one place), not speed.

**Windows.** The same server compiles with Winsock changes from module 03. Windows also has
`http.sys`, a kernel-mode HTTP listener used by IIS and ASP.NET that parses requests in the kernel
and can share port 80 among processes by URL prefix; nothing like it exists on Linux.

## 5. In production

- **Missing `Content-Length` on a keep-alive response.** A hand-written server forgot the header on
  one code path; the client waited for the connection to close to know the body had ended, and with
  keep-alive it never did: every request on that path "hung" for the idle timeout. Framing is not
  optional, in either direction.
- **The 15-second idle timeout nobody knew about.** A connection pool kept HTTP connections idle
  longer than the server's keep-alive timeout. The server closed them; the next request hit a
  closed connection and failed with `ECONNRESET`. Fix: client idle timeout shorter than the server's,
  and retry idempotent requests once on a fresh connection when the failure happens before any
  response byte.
- **`GET` with side effects.** An endpoint `GET /loans/4471/approve` existed for convenience. A
  link-prefetching browser extension approved loans. Safe methods must be safe.
- **Retrying `POST` without a key.** A client retried `POST /payments` on 504 from a gateway; the
  backend had processed the first one. Idempotency keys on every non-idempotent write, stored with
  the result, and `409`/replay on repeat.
- **Header size attack.** A fuzzer sent 10 MB of headers; the server buffered until OOM. Cap head
  size (the demo: 64 KB) and body size, and reply `431`/`413` and close.
- **Chunked bodies from a new client library.** A rewritten client began sending
  `Transfer-Encoding: chunked` for small bodies; the hand-written backend did not support it and
  silently misparsed. Returning `411` loudly (as the demo does) is better than guessing; supporting
  chunked is better still, and a library does it for you.
- **Your context.** If the lending server ever exposes HTTP (a REST facade for mobile or partner
  integration), it will sit behind a reverse proxy that terminates TLS. Design the resources (loans,
  customers, disbursements, repayments), use `Idempotency-Key` on postings, return Problem Details
  errors with the request id, and use `ETag`/`If-Match` to map onto your optimistic row versions.

## 6. Try it

1. **(easy)** Run the server and every `curl` line in section 3.1. With `tcpdump -A -i lo port
   8080` running, confirm the keep-alive test reused one connection. Done when: the capture shows
   one handshake and two requests.

2. **(easy)** Add `HEAD` support check: `curl -I http://127.0.0.1:8080/kv/x` must return the same
   headers as GET with no body. Add `ETag` (a hash of the value) to `GET /kv/<key>` and `304 Not
   Modified` when `If-None-Match` matches. Done when: the second `curl` with the ETag returns 304.

3. **(medium)** Implement chunked **request** body decoding (state machine: size line, data, CRLF,
   repeat, terminating `0`, optional trailers). Test with `curl -H 'Transfer-Encoding: chunked'
   --data-binary @bigfile -X PUT`. Done when: the stored value equals the file.

4. **(medium)** Implement `If-Match` on `PUT /kv/<key>`: `412` if the current ETag differs.
   Write a two-client race test in Python that shows one writer winning and one getting 412. Done
   when: the test is deterministic.

5. **(hard)** Port the server onto the module 06 epoll loop: per-connection `Request` parse state,
   output buffer, keep-alive. Benchmark with `wrk -c 1000 -t 4 -d 10s http://127.0.0.1:8080/health`
   against the thread-per-connection version. Done when: both survive 1000 connections and you can
   explain the throughput and memory difference.

6. **(hard)** Add TLS with OpenSSL: `SSL_CTX_new(TLS_server_method())`, load a self-signed cert
   (`openssl req -x509 -newkey rsa:2048 -nodes ...`), `SSL_accept`, `SSL_read`/`SSL_write` replacing
   `recv`/`send`. Done when: `curl -k https://127.0.0.1:8443/health` works and `openssl s_client`
   shows TLS 1.3.

## 7. Interview questions

**Q: How does an HTTP/1.1 receiver know where the body ends?**
A: `Transfer-Encoding: chunked` (size-prefixed chunks ending with a zero chunk), else
`Content-Length`, else for responses until the connection closes. A request with a body and neither
header is a 411. Both headers present is a smuggling risk and should be rejected.

**Q: What is keep-alive and why does it matter?**
A: Reusing one TCP connection for sequential requests, the default in HTTP/1.1. It avoids a
handshake, slow start and a TIME_WAIT per request, which is the difference between thousands and
tens of thousands of requests per second for small responses. Both sides need idle timeouts and
the client must handle the server closing an idle connection.

**Q: Idempotent versus safe methods?**
A: Safe: no side effects (GET, HEAD, OPTIONS). Idempotent: repeating has the same effect as once
(GET, PUT, DELETE; also HEAD, OPTIONS). POST is neither; make it retry-safe with an idempotency key.
Caches, crawlers and retry logic all depend on this classification being honoured.

**Q: What does HTTP/2 change and what does it not fix?**
A: Binary framing, multiplexed streams on one connection, header compression (HPACK), per-stream
flow control, server push (deprecated in practice). It keeps HTTP semantics. It does not fix
TCP-level head-of-line blocking: one lost segment stalls all streams, which HTTP/3 over QUIC fixes.

**Q: Walk through a TLS 1.3 handshake.**
A: ClientHello with supported suites, an ECDHE key share, SNI and ALPN; ServerHello with its key
share and chosen suite, then encrypted Certificate, CertificateVerify and Finished; client Finished.
One round trip; both derive symmetric keys from ECDHE (forward secrecy); the certificate chain
proves identity. Resumption allows 0-RTT data.

**Q: How would you design the REST API for loan disbursement?**
A: `POST /loans/{id}/disbursements` with an `Idempotency-Key` header, returning `201` and a
`Location` for the disbursement resource (or `202` if asynchronous, with a status URL). Errors as
Problem Details with a machine-readable code and request id. `GET` for status. Never a `GET` that
disburses.

**Q: When would you not write the HTTP server yourself?**
A: Whenever it faces other teams or the internet: chunked encoding, `Expect`, smuggling defences,
TLS, HTTP/2 and compression are where hand-written servers fail or get exploited. Use cpp-httplib
for simple blocking services, Beast on Asio for asynchronous scale, or sit behind nginx/Envoy and
speak plain HTTP/1.1.

## 8. Recap

- Request: request line, headers, blank line, body. Response: status line, headers, blank line,
  body. Body framed by chunked encoding, then `Content-Length`, else close.
- Methods: GET/HEAD safe and idempotent; PUT/DELETE idempotent; POST neither (use idempotency keys).
- 4xx: your fault, do not retry as-is. 5xx: server's fault, retry with backoff. 429/503 carry `Retry-After`.
- Keep-alive is default in 1.1; idle timeouts on both sides; keep leftover bytes for pipelining.
- Chunked: hex size, data, CRLF, repeat, `0\r\n\r\n`.
- HTTP/2: binary frames, multiplexed streams, HPACK, per-stream flow control, TCP HOL remains.
- HTTP/3: same over QUIC on UDP, per-stream loss recovery, 1-RTT setup with TLS 1.3 built in.
- TLS 1.3: one-RTT ECDHE handshake, certificate proves identity, forward secrecy; terminate at a
  proxy for convenience, not speed.
- REST: resources as nouns, methods as verbs, status codes for outcomes, Problem Details for
  errors, ETag/If-Match for concurrency, idempotency keys for POST, path versioning, cursors.
- Hand-write for learning and internal tools; use a library or a proxy for everything else.
