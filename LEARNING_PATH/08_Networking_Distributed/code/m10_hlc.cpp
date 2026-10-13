// m10_hlc.cpp - hybrid logical clock (HLC) simulation across three nodes with clock skew and a
// backwards NTP step, with assert-based checks of the properties HLC promises.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m10_hlc.cpp -o hlc
// Run:    ./hlc            (optional first argument: random seed)
//
// The problem: each machine's wall clock is a little wrong (skew) and can jump (NTP step). If you
// stamp events with raw wall-clock time, a message can be "received before it was sent". Lamport
// clocks fix ordering but lose any relation to real time. A hybrid logical clock keeps a value
// that is (a) always >= the local physical clock, (b) never more than the worst skew ahead of it,
// and (c) consistent with causality: if event e happened-before f then hlc(e) < hlc(f).
//
// Algorithm (Kulkarni, Demirbas et al., 2014). State per node: l (max physical time seen) and c
// (a logical counter that breaks ties when l does not move).
//   local or send event:  l' = max(l, pt); c = (l' == l) ? c + 1 : 0; l = l'
//   receive (lm, cm):     l' = max(l, lm, pt);
//                         if l' == l == lm: c = max(c, cm) + 1
//                         else if l' == l:  c = c + 1
//                         else if l' == lm: c = cm + 1
//                         else:             c = 0
//                         l = l'
// YugabyteDB's "hybrid time" is this: physical microseconds in the high bits, a 12-bit logical
// counter in the low bits, packed into one uint64 so comparisons are a single integer compare.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
#include <vector>

struct Hlc {
    int64_t l = 0;   // microseconds
    uint32_t c = 0;  // logical counter

    // Packed like YugabyteDB's HybridTime: 52 bits of microseconds, 12 bits of logical.
    uint64_t packed() const {
        assert(c < (1u << 12) && "logical counter overflow: physical clock stalled far too long");
        return (static_cast<uint64_t>(l) << 12) | c;
    }
    friend bool operator<(const Hlc& a, const Hlc& b) { return std::tie(a.l, a.c) < std::tie(b.l, b.c); }
    friend bool operator==(const Hlc& a, const Hlc& b) { return a.l == b.l && a.c == b.c; }
};

class Node {
public:
    Node(std::string name, int64_t skew_us) : name_(std::move(name)), skew_us_(skew_us) {}

    // The machine's wall clock: true time plus its skew. In a real system this is
    // clock_gettime(CLOCK_REALTIME) and the skew is unknown; here we choose it.
    int64_t physical(int64_t true_us) const { return true_us + skew_us_; }

    // Simulates an NTP step: the clock is set backwards (or forwards) instantly.
    void step_clock(int64_t delta_us) { skew_us_ += delta_us; }

    Hlc local_or_send(int64_t true_us) {
        int64_t pt = physical(true_us);
        int64_t l_new = std::max(h_.l, pt);
        h_.c = (l_new == h_.l) ? h_.c + 1 : 0;
        h_.l = l_new;
        check_monotonic();
        return h_;
    }

    Hlc receive(const Hlc& m, int64_t true_us) {
        int64_t pt = physical(true_us);
        int64_t l_new = std::max({h_.l, m.l, pt});
        if (l_new == h_.l && l_new == m.l)      h_.c = std::max(h_.c, m.c) + 1;
        else if (l_new == h_.l)                 h_.c = h_.c + 1;
        else if (l_new == m.l)                  h_.c = m.c + 1;
        else                                    h_.c = 0;
        h_.l = l_new;
        check_monotonic();
        return h_;
    }

    const std::string& name() const { return name_; }
    const Hlc& clock() const { return h_; }

private:
    // Property: a node's HLC never goes backwards, even when its wall clock does.
    void check_monotonic() {
        assert(last_ < h_);
        last_ = h_;
    }
    std::string name_;
    int64_t skew_us_;
    Hlc h_;
    Hlc last_;
};

struct Message {
    int from, to;
    int64_t deliver_at_us;
    Hlc stamp;          // HLC at send
    int64_t send_pt;    // sender's raw physical clock at send, to compare against
};

int main(int argc, char** argv) {
    unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 7u;
    std::mt19937 rng(seed);

    // Skews chosen to make trouble: B runs 5 ms fast, C runs 8 ms slow. A message from B to C
    // that takes 1 ms arrives at a wall-clock time 12 ms *earlier* than it was sent.
    std::vector<Node> nodes = {Node("A", 0), Node("B", 5000), Node("C", -8000)};

    std::uniform_int_distribution<int> pick(0, 2);
    std::uniform_int_distribution<int> delay_us(200, 2000);  // typical LAN-ish one-way delay
    std::uniform_int_distribution<int> step_us(50, 400);     // time between simulated events

    std::vector<Message> in_flight;
    int messages = 0, physical_violations = 0, local_events = 0;
    // Simulated true time in microseconds since the Unix epoch (a date in 2023), so every
    // physical clock reading is a realistic positive number. Printed relative to `base`.
    const int64_t base = 1700000000000000LL;
    int64_t t = base;
    const int64_t end = base + 200000;  // 200 ms of simulated time
    bool stepped = false;

    while (t < end) {
        t += step_us(rng);

        // Halfway through, A's NTP daemon steps its clock back by 20 ms.
        if (!stepped && t > base + 100000) {
            nodes[0].step_clock(-20000);
            stepped = true;
            std::printf("t=%6lld us  A's wall clock stepped back 20 ms (NTP step)\n", static_cast<long long>(t - base));
        }

        // Deliver every message that is due, in delivery order.
        std::sort(in_flight.begin(), in_flight.end(),
                  [](const Message& a, const Message& b) { return a.deliver_at_us < b.deliver_at_us; });
        while (!in_flight.empty() && in_flight.front().deliver_at_us <= t) {
            Message m = in_flight.front();
            in_flight.erase(in_flight.begin());
            Node& dst = nodes[m.to];
            int64_t recv_pt = dst.physical(m.deliver_at_us);
            Hlc r = dst.receive(m.stamp, m.deliver_at_us);

            // Property: causality. send happened-before receive, so the HLC must order them.
            assert(m.stamp < r);
            if (recv_pt <= m.send_pt) ++physical_violations;  // raw clocks get this wrong
            ++messages;
            if (messages <= 6 || recv_pt <= m.send_pt) {
                if (messages <= 6 || physical_violations <= 3)
                    std::printf("t=%6lld us  %s->%s  send pt=%lld recv pt=%lld %s hlc send=(%lld,%u) recv=(%lld,%u)\n",
                                static_cast<long long>(m.deliver_at_us - base), nodes[m.from].name().c_str(),
                                dst.name().c_str(), static_cast<long long>(m.send_pt - base),
                                static_cast<long long>(recv_pt - base), recv_pt <= m.send_pt ? "[RAW CLOCK WRONG]" : "",
                                static_cast<long long>(m.stamp.l - base), m.stamp.c,
                                static_cast<long long>(r.l - base), r.c);
            }
        }

        // One random action: a local event (e.g. a write) or a send.
        int who = pick(rng);
        if (rng() % 3 == 0) {
            nodes[who].local_or_send(t);
            ++local_events;
        } else {
            int to = (who + 1 + static_cast<int>(rng() % 2)) % 3;
            Message m;
            m.from = who;
            m.to = to;
            m.send_pt = nodes[who].physical(t);
            m.stamp = nodes[who].local_or_send(t);
            m.deliver_at_us = t + delay_us(rng);
            in_flight.push_back(m);
        }

        // Property: bounded divergence. HLC's l never exceeds the largest physical clock in the
        // cluster at this instant, so it stays within max skew of real time. Lamport clocks
        // have no such bound; this is why HLC values can be used as timestamps for snapshots.
        int64_t max_pt = 0;
        for (const Node& n : nodes) max_pt = std::max(max_pt, n.physical(t));
        for (const Node& n : nodes) assert(n.clock().l <= max_pt);
    }

    std::printf("\nmessages delivered: %d, local events: %d\n", messages, local_events);
    std::printf("raw physical clocks ordered receive <= send %d times; HLC ordered every one correctly\n",
                physical_violations);
    for (const Node& n : nodes)
        std::printf("node %s final hlc=(%lld,%u) packed=0x%016llx\n", n.name().c_str(),
                    static_cast<long long>(n.clock().l - base), n.clock().c,
                    static_cast<unsigned long long>(n.clock().packed()));

    // The skews guarantee B->C messages break raw physical ordering; if this fires, the demo
    // did not demonstrate anything (change the seed).
    assert(physical_violations > 0);
    std::printf("all HLC checks passed\n");
    return 0;
}
