# 09. Client-server architecture and reliability

**Time:** ~2.5 hours   **Prerequisites:** 05, 07

## 1. In plain words

One server and one client is easy. Real systems have hundreds of branch clients, several
application servers, a load balancer in front, a database cluster behind, and a network that
sometimes drops, slows or lies. **Reliability engineering** is the set of habits that keep the
whole thing working when any one piece is misbehaving: give every call a deadline, retry carefully,
stop calling things that are failing, keep one bad tenant from hurting the others, say no early
when overloaded, and record enough to see what happened.

Think of a bank branch at month-end. Queues form (load), a teller falls ill (server failure), the
core system is slow (dependency latency). A well-run branch has a numbered-ticket system (queueing
with limits), tells people "come back in an hour" when the queue is too long (load shedding), keeps
a separate counter for pensioners so a long business transaction does not block them (bulkheads),
and writes everything in a ledger with ticket numbers (observability with correlation ids).

This module names each habit, gives the standard algorithm for it, and ends with the request path
of a banking server from branch to database, which is the architecture you already run.

## 2. The real thing

### 2.1 Stateless vs stateful servers

A **stateless** server keeps nothing between requests: every request carries what is needed (an
auth token, the account id), and any server instance can handle it. Add instances to scale; kill
one and nothing is lost. State lives in a database or cache. HTTP APIs are designed this way.

A **stateful** server remembers the client between requests: a login session, a cursor, a
half-built transaction, an open file. Scaling needs **sticky sessions** (the load balancer routes
the same client to the same server) or a shared session store (Redis). Killing an instance loses
its sessions. Your RPC server with a long-lived connection per branch and per-connection context is
stateful by design; the question to ask is what is lost when a node dies and how the client recovers.

Middle ground: connection state (authenticated identity, protocol version) is fine; business state
(the loan being built) should be persisted after each step so a reconnect can resume.

### 2.2 Sessions

Options: server-side session id in a cookie or header with the data in a shared store; a signed
**token** (JWT) that carries the claims so servers verify without a lookup (stateless, but
revocation is hard, so keep lifetimes short and use refresh tokens); or a per-connection
authenticated context as your RPC server does. Whatever you choose: sessions expire, re-auth is
needed, and the logs carry the session or user id for correlation.

### 2.3 Connection pooling

Establishing a connection costs a TCP handshake, possibly TLS, authentication, and TCP slow start
(modules 02, 08). A **pool** keeps N open connections to a backend and lends them out per request.
Rules for a good pool:
- Bounded size (the backend's capacity, not the client's wish). PostgreSQL and YugabyteDB handle a
  few hundred connections well; thousands need pgbouncer or the YSQL connection manager.
- Health-check or validate on borrow (`SELECT 1` or a protocol ping) when a connection has been idle
  longer than the server's idle timeout; discard on any error.
- Max lifetime (recycle every 30 to 60 minutes) so DNS changes and server restarts are picked up.
- Acquire timeout: waiting for a free connection is itself a bounded wait; failing fast beats
  queueing forever.
- Metrics: in-use, idle, wait time, wait count. Pool exhaustion is the most common "the app is
  slow but the DB is fine" cause.

### 2.4 Load balancers: L4 vs L7

```
 branches ---> [ LB ] ---> app1
                       \-> app2
                        \-> app3
```

**L4** (transport layer) balancers forward TCP connections based on IP and port: they see bytes,
not requests. Fast, protocol-agnostic, and the natural choice for your RPC-over-TCP: a connection
lands on one app server and stays there. Examples: Linux IPVS/keepalived, HAProxy in TCP mode, AWS
NLB, F5 in L4 mode. Balancing granularity is per connection, so long-lived connections can be
unevenly distributed after a server restarts (everyone reconnected to the survivors).

**L7** (application layer) balancers understand HTTP: route by path or header, retry failed
requests, terminate TLS, add headers (`X-Forwarded-For`, `X-Request-Id`), rate limit, do canary
routing by percentage. nginx, HAProxy in HTTP mode, Envoy, AWS ALB. Balancing is per request, so
load evens out. Costs: more CPU, HTTP only, a place where request smuggling matters.

Algorithms: round robin, least connections (good for long-lived), weighted (bigger boxes get
more), consistent hashing by key (cache affinity, module 12), random-two-choices (pick two at
random, send to the less loaded; surprisingly close to optimal with cheap state).

### 2.5 Health checks

A load balancer removes a backend that fails health checks and adds it back when they pass.
- **Liveness**: is the process alive and responding? (`GET /health` returns 200.) Fail it and the
  orchestrator restarts you.
- **Readiness**: can it serve traffic right now? (Database reachable, caches warm, not shutting
  down.) Fail it and the LB stops sending traffic without restarting you. This is how graceful
  shutdown begins behind a load balancer: fail readiness, wait for the LB to notice (its check
  interval times its failure threshold), then stop accepting (module 05).
- Depth: a shallow check (process up) is cheap but misses a broken DB connection; a deep check
  (query the DB) can take down every server at once when the DB hiccups. Common compromise: shallow
  readiness plus separate dependency metrics with alerts.
- For an L4 balancer in front of your RPC server, the check is a TCP connect or a protocol-level
  `PING`; make sure `PING` does not need a worker thread, or a busy server looks dead.

### 2.6 Timeouts everywhere

Every blocking operation has a deadline: connect, each read, the whole request, each database
query, the time waiting for a pool connection, the time in a queue. Rules:
- The caller's deadline bounds the callee's: if the branch gave the app server 5 s, the app server
  gives the DB at most 4 s and the audit log write 0.5 s. **Deadline propagation**: pass the
  remaining time (or an absolute deadline) in the request.
- Timeouts shorter than the dependency's normal p99 cause false failures; longer than the client's
  patience are useless. Measure, then set to about p99.9 of healthy latency with headroom.
- A timeout is an "unknown outcome" (module 07). Decide per operation what happens next.
- Idle timeouts on servers protect workers; connect timeouts on clients protect threads from black
  holes (default 2 minutes!).

### 2.7 Retries with exponential backoff and jitter

A retry turns a transient failure into a success. Done badly it turns a hiccup into an outage:
every client retries at once, tripling the load on a server that was already struggling.

Rules:
1. Retry only transient errors (`Unavailable`, `503`, connection reset before any response byte,
   timeouts **only** if idempotent or keyed).
2. Cap attempts (usually 3 total).
3. **Exponential backoff**: wait `base * 2^attempt` (100 ms, 200 ms, 400 ms), capped (say 5 s).
4. **Jitter**: randomise the wait so clients do not retry in lockstep. "Full jitter" picks
   `random(0, base * 2^attempt)`; AWS's analysis found it best for spreading load.
5. **Retry budget**: allow retries to be at most, say, 10% of total requests per client; when the
   budget is exhausted, fail fast. This caps the amplification during an outage.
6. Retry at one layer only. A client retrying three times, through a proxy that retries three times,
   onto a server that retries its DB three times, is 27x load.

```cpp
std::chrono::milliseconds backoff(int attempt, int base_ms = 100, int cap_ms = 5000, std::mt19937& rng) {
    int exp = std::min(cap_ms, base_ms * (1 << attempt));
    return std::chrono::milliseconds(std::uniform_int_distribution<int>(0, exp)(rng));   // full jitter
}
```

### 2.8 Circuit breakers

If a dependency is down, every call to it wastes a thread for the full timeout and the failures
cascade upward. A **circuit breaker** tracks recent failures per dependency and, when they cross a
threshold, **opens**: calls fail immediately without touching the dependency. After a cool-down it
goes **half-open**: one trial call; success closes the circuit, failure re-opens it.

```
   CLOSED --(failure rate > 50% over 20 calls)--> OPEN --(30 s)--> HALF-OPEN --success--> CLOSED
                                                   ^                   |
                                                   +------failure------+
```

The fast failure protects your own threads and gives the dependency room to recover. Pair it with
a fallback where one exists (cached value, degraded response, "try again later" message).
Libraries: Resilience4j (Java), Polly (.NET), Envoy outlier detection at the proxy level. In C++
you write it: a mutex, a sliding window of outcomes, a state enum and a timestamp.

### 2.9 Bulkheads

Ships have compartments so one leak does not sink everything. In a server: separate thread pools
or connection pools per dependency or per tenant, so a slow reporting query cannot consume every
worker that the payment path needs. Also per-client concurrency limits (max in-flight per branch)
and separate queues for interactive vs batch work. The cost is lower utilisation; the benefit is
that one failure stays in its compartment.

### 2.10 Rate limiting

Protects a server from a client that sends too much, intentionally or by bug. Algorithms:
- **Token bucket** (section 3): burst `capacity`, sustained `rate`. The standard. Used by Linux
  `tc`, AWS API Gateway, nginx `limit_req` (a variant: leaky bucket).
- **Leaky bucket**: a queue drained at a fixed rate; smooths output rather than policing input.
- **Fixed window** counter: "100 per minute" resets at :00; allows 200 in two seconds around the
  boundary. Simple, flawed.
- **Sliding window log/counter**: more accurate, more state.

Where: at the edge (per IP, per API key), per tenant inside the app (per branch code), per
dependency (how fast *we* may call *them*). Reply `429` or `ERR_BUSY` with `Retry-After` so good
clients back off. Distributed limiting (many servers, one global limit) uses a shared store (Redis
with Lua for atomicity) or per-server limits of `global / N` with some slack.

### 2.11 Load shedding and graceful degradation

When overloaded, serve some requests well rather than all requests badly. Bounded queues with fast
rejection (module 05), priority (health checks and payments before reports), adaptive concurrency
limits (lower the in-flight limit when latency rises), and **degradation**: show cached balances
when the live query fails, accept loan applications into a queue when the scoring service is down
and process them later, disable a non-essential feature. Each degradation is a product decision
made in advance, not at 3 AM.

### 2.12 Observability: metrics, logs, traces

- **Metrics**: numbers over time, cheap, aggregated. The RED set per endpoint: **R**ate (requests
  per second), **E**rrors (per second, by class), **D**uration (histogram; p50, p99, p99.9).
  Plus saturation: queue depth, pool in-use, threads busy, fds open, CLOSE_WAIT count. Prometheus
  format with Grafana is the de facto standard; on Windows, performance counters or the same
  Prometheus exporters.
- **Logs**: structured (JSON or key=value), one line per event, with a **request id** on every line
  so you can `grep` one request across servers. Log at the boundaries (received, dispatched to DB,
  replied) with durations. Never log secrets or full card numbers.
- **Traces**: a request id that travels with the call (header or RPC field) plus per-hop spans with
  start, end and parent, assembled into a timeline: "branch -> app (120 ms) -> DB query (95 ms) ->
  audit write (20 ms)". OpenTelemetry is the standard API; Jaeger and Tempo store traces. Even
  without tooling, propagating a request id and logging durations per hop gives you 80% of the value.

What to alert on: SLO burn (error rate and latency against the target, module 13), saturation
near limits, and nothing else at 3 AM. Everything else is a dashboard.

### 2.13 The banking server's request path

```
 Branch client (teller UI)
   | RPC over TCP, long-lived connection, request id + branch code + teller id, deadline 10 s
   v
 L4 load balancer / VIP (health check: TCP connect or PING)
   v
 Application server (one of N)
   | acceptor + worker pool (module 05), per-connection auth context
   | per-request: validate, authorise, call business logic
   | DB pool (bounded, validated on borrow, deadline 8 s), circuit breaker per DB cluster
   v
 YugabyteDB (YSQL, RF3): PL/pgSQL procedures, transaction, Raft replication (modules 11-12)
   ^
   | audit/event log write (separate pool: bulkhead), notification queue (async; module 14)
```

Reliability features to be able to point at in this picture: deadline propagation (10 s at the
branch, 8 s for the DB, so the branch always gets an answer before giving up); idempotency key =
the branch's transaction id stored with the posting; retries only at the branch client, only for
`Unavailable`, with jitter, never for a timed-out posting without the key; circuit breaker around
the DB so a cluster outage fails fast and the branch shows a clear message; a separate pool for
audit writes so audit slowness cannot block postings; rate limit per branch to contain a looping
client; `PING` answered off the worker pool so health checks work under load; request id in every
log line on both sides. Every one of those is a sentence you can say in an interview, and each maps
to something you can check in the real code this week.

## 3. Code

### 3.1 Token bucket (`code/m09_token_bucket.cpp`)

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m09_token_bucket.cpp -o token_bucket && ./token_bucket
```

Lazy refill: no timer thread, tokens are computed from elapsed time at each call.

```cpp
class TokenBucket {
public:
    TokenBucket(double capacity, double rate_per_sec, std::function<TimePoint()> now = Clock::now);
    bool try_acquire(double n = 1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        if (tokens_ >= n) { tokens_ -= n; return true; }
        return false;
    }
    std::chrono::microseconds time_until(double n = 1.0);   // for Retry-After or a sleep
private:
    void refill() {
        TimePoint t = now_();
        double elapsed = std::chrono::duration<double>(t - last_).count();
        if (elapsed > 0) { tokens_ = std::min(capacity_, tokens_ + elapsed * rate_); last_ = t; }
    }
    double capacity_, rate_, tokens_; std::function<TimePoint()> now_; TimePoint last_; std::mutex mu_;
};
```

The injectable clock makes the test deterministic (burst of exactly `capacity`, exactly `rate`
tokens after one second, never above capacity after an hour). The concurrent demo runs 8 threads
for one second against burst 50 and rate 100 and checks that about 150 requests were accepted no
matter how hard the threads hammer. `KeyedLimiter` keeps one bucket per key with idle eviction,
which is the per-branch limiter from the request path.

### 3.2 Backoff and circuit breaker sketches

```cpp
// Full-jitter backoff. attempt starts at 0.
std::chrono::milliseconds backoff_ms(int attempt, std::mt19937& rng, int base = 100, int cap = 5000) {
    int exp = std::min(cap, base * (1 << std::min(attempt, 10)));
    return std::chrono::milliseconds(std::uniform_int_distribution<int>(0, exp)(rng));
}

class CircuitBreaker {
    enum class State { Closed, Open, HalfOpen } state_ = State::Closed;
    int failures_ = 0, window_ = 0; TimePoint opened_at_;
    const int min_calls_ = 20; const double threshold_ = 0.5; const std::chrono::seconds cooldown_{30};
    std::mutex mu_;
public:
    bool allow() {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == State::Open) {
            if (Clock::now() - opened_at_ < cooldown_) return false;
            state_ = State::HalfOpen; return true;             // one trial call
        }
        return true;
    }
    void record(bool ok) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == State::HalfOpen) { state_ = ok ? State::Closed : State::Open; opened_at_ = Clock::now(); failures_ = window_ = 0; return; }
        ++window_; if (!ok) ++failures_;
        if (window_ >= min_calls_) {
            if (static_cast<double>(failures_) / window_ > threshold_) { state_ = State::Open; opened_at_ = Clock::now(); }
            failures_ = window_ = 0;
        }
    }
};
```

Exercise 3 turns the sketch into a tested class.

## 4. Under the hood (senior-level)

**Why jitter works.** Without it, N clients that failed at the same instant retry at the same
instant: the server sees N requests in one burst, which is exactly what overloaded it, repeated at
100 ms, 200 ms, 400 ms. With full jitter the N retries spread uniformly over the window; the peak
load drops by roughly the ratio of window length to request service time. Marc Brooker's AWS
analysis (2015) showed full jitter needs the fewest total calls to get all clients through.

**Metastable failure.** A system can enter a state where load from retries alone keeps it
overloaded even after the original trigger is gone: the retries cause timeouts which cause more
retries. Recovery needs the clients to back off or the server to shed load below its capacity. Retry
budgets and circuit breakers exist to prevent this state; bounded queues and fast rejection end it.

**Queueing theory in one line.** At utilisation `rho`, mean wait grows like `rho / (1 - rho)` times
service time (M/M/1). At 50% utilisation you wait one service time; at 90%, nine; at 99%, ninety-nine.
This is why p99 latency explodes long before CPU hits 100%, why you provision for 60 to 70%
utilisation, and why admission control (reject at a queue depth) keeps latency sane under overload.

**Connection pool sizing.** Little's law: `connections = throughput * latency`. 500 queries per
second at 20 ms each need 10 connections on average; size for the peak with headroom (say 25), not
for the number of application threads (often hundreds). More connections than the database has
cores plus disks mostly adds contention. PostgreSQL's advice of "a small pool, around 2 to 4 times
the CPU count" comes from this.

**Where the token bucket lives.** In-process: a mutex and two doubles; contention is negligible
below millions of operations per second. Per-key maps need eviction (the demo sweeps on each call
after an interval) or a bounded LRU. Distributed: Redis `INCR` with expiry for fixed windows, or a
Lua script implementing the bucket atomically; or let each of N servers enforce `limit / N` and
accept the error.

**Health check propagation delay.** An LB with a 5 s interval and 3-failure threshold takes up to
15 s to stop sending traffic to a dead backend; during that time those requests fail. Shorter
intervals cost load; passive health checks (eject on real request failures, Envoy's outlier
detection) react in milliseconds. Graceful shutdown must wait at least the detection window after
failing readiness.

## 5. In production

- **Retry storm after a 2-second database stall.** All branch clients timed out together and
  retried immediately, three times. The DB, now healthy, saw 4x normal load and stalled again.
  The system took 20 minutes to recover from a 2-second event. Jittered backoff and a retry budget
  would have recovered in seconds. If your clients retry, find out how.
- **Pool exhaustion masquerading as a slow DB.** p99 latency jumped to 5 s; DB metrics were flat.
  The app's pool of 10 connections was fully in use because one slow report query held connections
  for 30 s each. Pool wait time was not measured. Bulkhead (separate pool for reports) and the
  wait-time metric fixed diagnosis and cause.
- **Deep health check takes everything down.** Readiness checked the database; a 10-second DB
  failover made every app server fail readiness; the LB had no backends; clients got connection
  refused for something the app could have partially served (cached reads). Shallow readiness, plus
  dependency alerting, plus degraded mode.
- **Timeout longer than the caller's.** App waited 30 s for the DB; the branch client gave up at
  10 s and retried; the app did the work twice and the branch saw a timeout anyway. Deadline
  propagation: the callee's timeout must be shorter than the caller's remaining time.
- **The looping branch client.** A bug made one branch's terminal resend a balance query in a tight
  loop: 2,000 requests per second from one connection, starving others. Per-connection token bucket
  (`ERR_BUSY`) contained it to that branch within seconds.
- **Logs without request ids.** An incident where "some" postings failed took two days to
  reconstruct from unlinked client and server logs. A request id generated at the branch and logged
  on every line turned the next one into a 20-minute `grep`.

## 6. Try it

1. **(easy)** Add a per-connection `TokenBucket` to the module 05 tcpkv server (burst 100, rate
   50/s). Excess commands get `-ERR_BUSY`. Done when: `m05_loadtest.py --pipeline 200` sees
   `ERR_BUSY` and a normal client does not.

2. **(easy)** Write `backoff_ms` and plot (print) the schedule for attempts 0 to 5 for ten
   different seeds. Done when: you can explain why no two clients retry at the same instant.

3. **(medium)** Implement `CircuitBreaker` as a tested class with an injectable clock: closed to
   open after 50% failures over 20 calls, half-open after 30 s, one trial. Done when: tests cover
   every transition.

4. **(medium)** Add `X-Request-Id` handling to the module 08 HTTP server: accept it from the client
   or generate one, echo it in the response, and log one JSON line per request with method, path,
   status, duration and the id. Done when: `curl -H 'X-Request-Id: abc'` produces a log line with `abc`.

5. **(hard)** Build a connection pool for the module 07 RPC client: fixed size, acquire with timeout,
   validate on borrow if idle more than 10 s (send `Echo`), discard on error, max lifetime 60 s,
   metrics for in-use/idle/wait. Done when: 32 threads share 8 connections correctly under the demo
   server and pool exhaustion produces a clear timeout error rather than a hang.

6. **(hard)** Write a one-page reliability review of your employer's request path using section
   2.13 as the template: for each hop list the timeout, retry policy, idempotency mechanism,
   pool bound, breaker, and what the client sees when that hop fails. Mark unknowns. Done when:
   every row is filled in or marked "find out", and you have three concrete improvements.

## 7. Interview questions

**Q: Stateless versus stateful servers?**
A: Stateless servers keep nothing between requests, so any instance can serve any request: easy
to scale and replace, state lives in a store. Stateful servers remember clients (sessions,
connections, cursors): need sticky routing or shared session stores, and lose state on failure.
Keep connection-level state small and persist business state after each step.

**Q: L4 versus L7 load balancing?**
A: L4 forwards TCP connections by IP and port: fast, protocol-agnostic, per-connection
granularity, right for RPC-over-TCP. L7 understands HTTP: routes by path and headers, retries,
terminates TLS, adds request ids, balances per request. Costs more CPU and only works for HTTP.

**Q: How should a client retry?**
A: Only transient errors, only idempotent (or keyed) operations after timeouts, capped attempts,
exponential backoff with full jitter, a retry budget, and at one layer only. Otherwise retries
amplify an outage.

**Q: What is a circuit breaker and why use one?**
A: A per-dependency state machine that fails calls fast when the dependency has been failing
(open), retries one trial after a cool-down (half-open), and resumes on success (closed). It saves
your threads from waiting on timeouts, stops cascading failure, and gives the dependency room to
recover. Pair it with a fallback.

**Q: Explain the token bucket.**
A: A bucket of capacity C refills at R tokens per second; each request takes a token; empty means
reject or wait. C is the allowed burst, R the sustained rate. Implemented lazily: on each call add
`elapsed * R` capped at C, then try to subtract. One mutex and two doubles.

**Q: Why timeouts everywhere, and how do you choose them?**
A: Any unbounded wait eventually holds a thread forever on a dead peer. Set each timeout from the
dependency's healthy p99.9 with headroom, make callee timeouts shorter than the caller's remaining
deadline (propagate deadlines), and decide the action for each timeout since the outcome is unknown.

**Q: What do you monitor on a server?**
A: Per endpoint: rate, errors by class, duration histogram (p50/p99/p99.9). Saturation: queue
depth, pool in-use and wait time, worker threads busy, fds, CLOSE_WAIT count. Structured logs with a
request id on every line; traces or at least per-hop durations. Alert on SLO burn and saturation.

**Q: How does graceful shutdown work behind a load balancer?**
A: Fail the readiness check first, wait for the LB's detection window so it stops sending new
connections, then stop accepting, drain in-flight requests with a deadline, force-close stragglers,
exit. Skipping the first step causes a burst of connection-refused errors.

## 8. Recap

- Stateless scales by adding instances; stateful needs stickiness or shared state; persist business
  state after each step.
- Pools: bounded, validated on borrow, max lifetime, acquire timeout, metrics. Size by Little's law.
- L4 for TCP protocols, L7 for HTTP features. Health checks: liveness restarts, readiness routes.
- Timeouts on every wait; propagate deadlines; callee shorter than caller.
- Retries: transient only, idempotent only after timeouts, capped, exponential backoff with full
  jitter, retry budget, one layer.
- Circuit breaker: closed, open, half-open; fail fast; fallback.
- Bulkheads: separate pools per dependency or tenant.
- Token bucket: burst C, rate R, lazy refill; per key with eviction; `429`/`ERR_BUSY` with Retry-After.
- Shed load early with bounded queues; degrade deliberately.
- Metrics (RED plus saturation), structured logs with request ids, traces across hops.
- Your request path: branch deadline 10 s, app to DB 8 s, idempotency key = branch txn id, breaker
  around DB, separate audit pool, per-branch rate limit, PING off the worker pool, ids in every log.
