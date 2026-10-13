// m11_raft_election_sim.cpp - single-process Raft simulation: leader election, log replication
// and commit, with simulated message delays, crashes and network partitions. Assert-based checks
// of Raft's safety properties run on every step.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m11_raft_election_sim.cpp -o raft_sim
// Run:    ./raft_sim           (optional first argument: random seed for the scripted scenario)
//
// Nothing here touches a real network. Time is a 1 ms tick counter. "Sending" a message puts it
// in a queue with a delivery time a few ms in the future. Crashing a node stops its ticks and
// drops messages to it. A partition is a group number per node; messages only flow inside a group.
// This makes every run deterministic for a given seed, which is how real Raft implementations
// are tested too (etcd's raft package, TigerBeetle's VOPR, FoundationDB's simulator).
//
// Scope: elections (with the "up-to-date log" vote restriction), AppendEntries with consistency
// check and conflict truncation, commit index advance (current-term rule), leader no-op entry.
// Not included: snapshots, membership changes, pre-vote, persistence to disk, client sessions.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

enum class Role { Follower, Candidate, Leader };
static const char* role_name(Role r) {
    return r == Role::Leader ? "LEADER" : r == Role::Candidate ? "candidate" : "follower";
}

struct Entry {
    int term;
    std::string cmd;
    bool operator==(const Entry& o) const { return term == o.term && cmd == o.cmd; }
};

enum class MsgType { RequestVote, RequestVoteReply, AppendEntries, AppendEntriesReply };

struct Msg {
    Msg(MsgType t, int f, int dst, int tm) : type(t), from(f), to(dst), term(tm) {}
    MsgType type;
    int from, to, term;
    // RequestVote
    int last_log_index = 0, last_log_term = 0;
    // RequestVoteReply
    bool granted = false;
    // AppendEntries
    int prev_index = 0, prev_term = 0, leader_commit = 0;
    std::vector<Entry> entries;
    // AppendEntriesReply
    bool success = false;
    int match_index = 0;
};

class Cluster;

struct Node {
    int id = 0;
    bool alive = true;

    // Persistent state: a real implementation fsyncs these before answering any RPC.
    int term = 0;
    int voted_for = -1;
    std::vector<Entry> log{Entry{0, "<sentinel>"}};  // index 0 is a sentinel; real entries start at 1

    // Volatile state.
    Role role = Role::Follower;
    int commit_index = 0;
    int election_deadline = 0;
    int heartbeat_due = 0;
    int votes = 0;
    std::vector<int> next_index, match_index;

    int last_index() const { return static_cast<int>(log.size()) - 1; }
    int last_term() const { return log.back().term; }
};

class Cluster {
public:
    static constexpr int kElectionMin = 150, kElectionMax = 300;  // ms, as in the Raft paper
    static constexpr int kHeartbeat = 50;                         // ms, well below kElectionMin
    static constexpr int kDelayMin = 2, kDelayMax = 15;           // ms one-way, simulated

    Cluster(int n, unsigned seed) : rng_(seed), nodes_(n), group_(n, 0) {
        for (int i = 0; i < n; ++i) {
            nodes_[i].id = i;
            reset_election_timer(nodes_[i]);
        }
    }

    int size() const { return static_cast<int>(nodes_.size()); }
    int now() const { return now_; }
    const Node& node(int i) const { return nodes_[i]; }
    void set_drop_rate(double p) { drop_rate_ = p; }
    void set_verbose(bool v) { verbose_ = v; }

    // ---- fault injection ----
    void crash(int i) {
        nodes_[i].alive = false;
        log_event("node %d CRASHES", i);
    }
    void restart(int i) {
        Node& n = nodes_[i];
        n.alive = true;
        // Persistent state (term, voted_for, log) survives; volatile state is lost.
        n.role = Role::Follower;
        n.commit_index = 0;
        n.votes = 0;
        reset_election_timer(n);
        log_event("node %d RESTARTS (term %d, log length %d)", i, n.term, n.last_index());
    }
    void partition(const std::vector<int>& groups) {
        group_ = groups;
        log_event("network PARTITIONED");
    }
    void heal() {
        std::fill(group_.begin(), group_.end(), 0);
        log_event("network HEALED");
    }

    // A client sends a command to node i. Only a leader accepts it (a real server would reply
    // "not leader, try node X"). Returns the log index it was appended at.
    std::optional<int> submit(int i, const std::string& cmd) {
        Node& n = nodes_[i];
        if (!n.alive || n.role != Role::Leader) return std::nullopt;
        n.log.push_back(Entry{n.term, cmd});
        n.match_index[i] = n.last_index();
        log_event("client -> node %d: append '%s' at index %d term %d", i, cmd.c_str(), n.last_index(), n.term);
        return n.last_index();
    }

    std::vector<int> leaders() const {
        std::vector<int> out;
        for (const Node& n : nodes_)
            if (n.alive && n.role == Role::Leader) out.push_back(n.id);
        return out;
    }

    // The leader with the highest term among live nodes, if any.
    std::optional<int> current_leader() const {
        std::optional<int> best;
        for (int id : leaders())
            if (!best || nodes_[id].term > nodes_[*best].term) best = id;
        return best;
    }

    void step() {
        ++now_;
        deliver_due_messages();
        for (Node& n : nodes_) {
            if (!n.alive) continue;
            if (n.role == Role::Leader) {
                if (now_ >= n.heartbeat_due) broadcast_append_entries(n);
            } else if (now_ >= n.election_deadline) {
                start_election(n);
            }
        }
        check_safety();
    }

    void run(int ms) {
        for (int i = 0; i < ms; ++i) step();
    }

    template <class Pred>
    bool run_until(Pred pred, int max_ms) {
        for (int i = 0; i < max_ms; ++i) {
            if (pred()) return true;
            step();
        }
        return pred();
    }

    bool committed_everywhere_alive(int index) const {
        for (const Node& n : nodes_)
            if (n.alive && n.commit_index < index) return false;
        return true;
    }

    // Whether any node's committed prefix contains `cmd`.
    bool is_committed(const std::string& cmd) const {
        for (const auto& kv : committed_)
            if (kv.second.entry.cmd == cmd) return true;
        return false;
    }

    void dump() const {
        for (const Node& n : nodes_) {
            std::printf("    node %d %-9s term=%d commit=%d alive=%d log=[", n.id, role_name(n.role), n.term,
                        n.commit_index, n.alive ? 1 : 0);
            for (int i = 1; i <= n.last_index(); ++i)
                std::printf("%s%d:%s", i > 1 ? " " : "", n.log[i].term, n.log[i].cmd.c_str());
            std::printf("]\n");
        }
    }

private:
    template <class... Args>
    void log_event(const char* fmt, Args... args) {
        if (!verbose_) return;
        std::printf("t=%5d  ", now_);
        if constexpr (sizeof...(Args) == 0) std::fputs(fmt, stdout);
        else std::printf(fmt, args...);
        std::printf("\n");
    }

    void reset_election_timer(Node& n) {
        // Randomised timeouts are the whole trick that makes split votes rare: one node usually
        // times out first and collects a majority before anyone else wakes up.
        n.election_deadline = now_ + std::uniform_int_distribution<int>(kElectionMin, kElectionMax)(rng_);
    }

    bool connected(int a, int b) const { return group_[a] == group_[b]; }

    void send(Msg m) {
        if (!connected(m.from, m.to)) return;
        if (drop_rate_ > 0 && std::uniform_real_distribution<double>(0, 1)(rng_) < drop_rate_) return;
        int at = now_ + std::uniform_int_distribution<int>(kDelayMin, kDelayMax)(rng_);
        queue_.emplace(at, std::move(m));  // multimap keeps insertion order for equal times
    }

    void deliver_due_messages() {
        while (!queue_.empty() && queue_.begin()->first <= now_) {
            Msg m = std::move(queue_.begin()->second);
            queue_.erase(queue_.begin());
            Node& dst = nodes_[m.to];
            // A partition that appears while a message is in flight also eats it.
            if (!dst.alive || !connected(m.from, m.to)) continue;
            handle(dst, m);
        }
    }

    // Rule for all servers: seeing a higher term means you are out of date. Adopt it and step
    // down. This single rule is what dethrones a stale leader after a partition heals.
    void maybe_step_down(Node& n, int term) {
        if (term > n.term) {
            if (n.role == Role::Leader) {
                log_event("node %d steps down: saw term %d > %d", n.id, term, n.term);
                // A leader does not run an election timer, so its deadline is long past. Without
                // this reset the ex-leader would time out instantly and disrupt the new leader.
                reset_election_timer(n);
            }
            n.term = term;
            n.voted_for = -1;
            n.role = Role::Follower;
        }
    }

    void start_election(Node& n) {
        n.role = Role::Candidate;
        ++n.term;
        n.voted_for = n.id;
        n.votes = 1;
        reset_election_timer(n);
        log_event("node %d times out, starts election for term %d", n.id, n.term);
        for (int peer = 0; peer < size(); ++peer) {
            if (peer == n.id) continue;
            Msg m{MsgType::RequestVote, n.id, peer, n.term};
            m.last_log_index = n.last_index();
            m.last_log_term = n.last_term();
            send(m);
        }
        if (size() == 1) become_leader(n);
    }

    void become_leader(Node& n) {
        n.role = Role::Leader;
        n.next_index.assign(size(), n.last_index() + 1);
        n.match_index.assign(size(), 0);
        // Leader appends a no-op in its own term. Raft only commits entries from the current term
        // by counting replicas (section 5.4.2 of the paper); the no-op lets older entries commit.
        n.log.push_back(Entry{n.term, "noop"});
        n.match_index[n.id] = n.last_index();
        log_event("node %d becomes LEADER for term %d", n.id, n.term);
        auto it = leader_of_term_.find(n.term);
        // Election Safety: at most one leader can be elected in a given term.
        assert(it == leader_of_term_.end() || it->second == n.id);
        leader_of_term_[n.term] = n.id;
        broadcast_append_entries(n);
    }

    void broadcast_append_entries(Node& n) {
        n.heartbeat_due = now_ + kHeartbeat;
        for (int peer = 0; peer < size(); ++peer) {
            if (peer == n.id) continue;
            Msg m{MsgType::AppendEntries, n.id, peer, n.term};
            m.prev_index = n.next_index[peer] - 1;
            m.prev_term = n.log[m.prev_index].term;
            m.leader_commit = n.commit_index;
            // Send everything the follower is missing. Real systems cap the batch size.
            m.entries.assign(n.log.begin() + n.next_index[peer], n.log.end());
            send(m);
        }
    }

    void handle(Node& n, const Msg& m) {
        maybe_step_down(n, m.term);
        switch (m.type) {
        case MsgType::RequestVote: {
            // Vote restriction: only vote for a candidate whose log is at least as up to date as
            // ours. Since a winner needs a majority of votes and every committed entry is on a
            // majority, the winner is guaranteed to hold every committed entry.
            bool log_ok = m.last_log_term > n.last_term() ||
                          (m.last_log_term == n.last_term() && m.last_log_index >= n.last_index());
            bool grant = m.term == n.term && (n.voted_for == -1 || n.voted_for == m.from) && log_ok;
            if (grant) {
                n.voted_for = m.from;
                reset_election_timer(n);  // granting a vote also defers our own candidacy
            }
            Msg r{MsgType::RequestVoteReply, n.id, m.from, n.term};
            r.granted = grant;
            send(r);
            break;
        }
        case MsgType::RequestVoteReply: {
            if (n.role != Role::Candidate || m.term != n.term || !m.granted) break;
            if (++n.votes > size() / 2) become_leader(n);
            break;
        }
        case MsgType::AppendEntries: {
            Msg r{MsgType::AppendEntriesReply, n.id, m.from, n.term};
            if (m.term < n.term) {  // stale leader: reject; our higher term in the reply dethrones it
                send(r);
                break;
            }
            // A valid leader for our term exists: candidates give up, followers stay put.
            n.role = Role::Follower;
            reset_election_timer(n);
            // Consistency check: our log must contain the leader's entry at prev_index.
            if (m.prev_index > n.last_index() || n.log[m.prev_index].term != m.prev_term) {
                send(r);
                break;
            }
            int idx = m.prev_index;
            for (const Entry& e : m.entries) {
                ++idx;
                if (idx <= n.last_index() && n.log[idx].term != e.term) {
                    // Conflict: delete this entry and everything after it. Never touches
                    // committed entries, which is what check_safety() verifies.
                    assert(idx > n.commit_index);
                    n.log.resize(idx);
                }
                if (idx > n.last_index()) n.log.push_back(e);
            }
            int new_commit = std::min(m.leader_commit, idx);
            if (new_commit > n.commit_index) n.commit_index = new_commit;
            r.success = true;
            r.match_index = idx;
            send(r);
            break;
        }
        case MsgType::AppendEntriesReply: {
            if (n.role != Role::Leader || m.term != n.term) break;
            if (m.success) {
                n.match_index[m.from] = std::max(n.match_index[m.from], m.match_index);
                n.next_index[m.from] = n.match_index[m.from] + 1;
                advance_commit(n);
            } else {
                // Walk back one entry and retry on the next heartbeat. Real implementations send
                // a hint (conflicting term and its first index) to skip faster.
                n.next_index[m.from] = std::max(1, n.next_index[m.from] - 1);
            }
            break;
        }
        }
    }

    void advance_commit(Node& n) {
        for (int N = n.last_index(); N > n.commit_index; --N) {
            if (n.log[N].term != n.term) break;  // current-term rule
            int count = 0;
            for (int peer = 0; peer < size(); ++peer)
                if (n.match_index[peer] >= N) ++count;
            if (count > size() / 2) {
                log_event("node %d commits up to index %d (replicated on %d/%d)", n.id, N, count, size());
                n.commit_index = N;
                break;
            }
        }
    }

    // Checked after every tick.
    void check_safety() {
        for (const Node& n : nodes_) {
            // State Machine Safety: once any node has committed index i with entry e, no node
            // ever commits a different entry at index i.
            for (int i = 1; i <= n.commit_index; ++i) {
                auto it = committed_.find(i);
                if (it == committed_.end())
                    // The first node to see a commit is the leader that made it, so n.term here
                    // is the term in which the entry became committed.
                    committed_.emplace(i, Committed{n.log[i], n.term});
                else
                    assert(it->second.entry == n.log[i] && "two nodes committed different entries at one index");
            }
            // Leader Completeness: an entry committed in term T is in the log of every leader of
            // a term greater than T. (A stale leader of an older term may legitimately lack it.)
            if (n.alive && n.role == Role::Leader) {
                for (const auto& kv : committed_) {
                    if (kv.second.commit_term < n.term) {
                        assert(kv.first <= n.last_index() && n.log[kv.first] == kv.second.entry);
                    }
                }
            }
        }
    }

    int now_ = 0;
    std::mt19937 rng_;
    std::vector<Node> nodes_;
    std::vector<int> group_;
    std::multimap<int, Msg> queue_;
    double drop_rate_ = 0.0;
    bool verbose_ = true;
    struct Committed {
        Entry entry;
        int commit_term;
    };
    std::map<int, int> leader_of_term_;
    std::map<int, Committed> committed_;
};

static void scripted_scenario(unsigned seed) {
    std::printf("=== scripted scenario, 5 nodes, seed %u ===\n", seed);
    Cluster c(5, seed);

    // 1. Initial election.
    bool ok = c.run_until([&] { return c.current_leader().has_value(); }, 3000);
    assert(ok && "no leader elected within 3 s");
    int l1 = *c.current_leader();
    std::printf("--> leader %d elected at t=%d ms, term %d\n\n", l1, c.now(), c.node(l1).term);

    // 2. Replicate three commands.
    int last = 0;
    for (const char* cmd : {"set A=1", "set B=2", "set C=3"}) last = *c.submit(l1, cmd);
    ok = c.run_until([&] { return c.committed_everywhere_alive(last); }, 1000);
    assert(ok && "commands did not commit on all nodes");
    std::printf("--> three commands committed on all 5 nodes by t=%d\n\n", c.now());

    // 3. Leader crashes; the remaining four elect a new one with a higher term.
    int old_term = c.node(l1).term;
    c.crash(l1);
    ok = c.run_until([&] { auto l = c.current_leader(); return l && *l != l1; }, 3000);
    assert(ok && "no new leader after crash");
    int l2 = *c.current_leader();
    assert(c.node(l2).term > old_term);
    std::printf("--> node %d took over at t=%d, term %d\n\n", l2, c.now(), c.node(l2).term);
    last = *c.submit(l2, "set D=4");
    ok = c.run_until([&] { return c.committed_everywhere_alive(last); }, 1000);
    assert(ok);

    // 4. Old leader restarts as a follower and catches up.
    c.restart(l1);
    ok = c.run_until([&] { return c.node(l1).commit_index >= last; }, 1000);
    assert(ok && "restarted node did not catch up");
    std::printf("--> node %d caught up to index %d\n\n", l1, last);

    // 5. Partition the current leader with one follower (minority of 2) away from the other 3.
    int lead = *c.current_leader();
    int buddy = (lead + 1) % 5;
    std::vector<int> groups(5, 1);
    groups[lead] = 0;
    groups[buddy] = 0;
    c.partition(groups);
    auto lost_idx = c.submit(lead, "set X=LOST");  // the minority leader still accepts writes
    assert(lost_idx.has_value());
    c.run(1500);
    std::vector<int> ls = c.leaders();
    std::printf("--> after 1.5 s of partition, nodes claiming leadership:");
    for (int id : ls) std::printf(" %d(term %d)", id, c.node(id).term);
    std::printf("\n");
    // Two "leaders" can coexist, but in different terms, and only one can commit.
    assert(ls.size() == 2);
    assert(!c.is_committed("set X=LOST") && "minority leader must not commit");
    int majority_leader = *c.current_leader();
    assert(majority_leader != lead && majority_leader != buddy);
    last = *c.submit(majority_leader, "set Y=KEPT");
    ok = c.run_until([&] { return c.node(majority_leader).commit_index >= last; }, 1000);
    assert(ok && "majority side failed to commit");
    std::printf("--> majority leader %d committed 'set Y=KEPT'; 'set X=LOST' is still uncommitted\n\n",
                majority_leader);
    c.dump();

    // 6. Heal. The stale leader sees a higher term, steps down, and its uncommitted entry is
    // overwritten by the majority's log.
    c.heal();
    ok = c.run_until([&] { return c.leaders().size() == 1 && c.committed_everywhere_alive(last); }, 3000);
    assert(ok && "cluster did not converge after heal");
    assert(!c.is_committed("set X=LOST"));
    for (int i = 0; i < c.size(); ++i)
        for (int j = 1; j <= c.node(i).last_index(); ++j)
            assert(c.node(i).log[j].cmd != "set X=LOST");
    std::printf("\n--> converged at t=%d: one leader, 'set X=LOST' gone from every log\n", c.now());
    c.dump();
}

// Random crashes, restarts, partitions and 5% message loss. Liveness is not asserted here, only
// safety: the asserts inside Cluster fire if any property is ever violated.
static void fuzz(unsigned seeds) {
    std::printf("\n=== safety fuzz: %u seeds x 20 s simulated, random faults, 5%% loss ===\n", seeds);
    int total_commits = 0;
    for (unsigned s = 1; s <= seeds; ++s) {
        Cluster c(5, s * 7919u);
        c.set_verbose(false);
        c.set_drop_rate(0.05);
        std::mt19937 rng(s);
        int cmd = 0;
        for (int t = 0; t < 20000; ++t) {
            c.step();
            unsigned r = rng() % 1000;
            if (r < 2) c.crash(static_cast<int>(rng() % 5));
            else if (r < 6) c.restart(static_cast<int>(rng() % 5));
            else if (r < 7) {
                std::vector<int> g(5);
                for (int& x : g) x = static_cast<int>(rng() % 2);
                c.partition(g);
            } else if (r < 9) c.heal();
            else if (r < 60) {
                if (auto l = c.current_leader()) c.submit(*l, "cmd" + std::to_string(++cmd));
            }
            // Keep at least a majority able to run most of the time so commits actually happen.
            if (t % 3000 == 0) { for (int i = 0; i < 5; ++i) if (!c.node(i).alive) c.restart(i); c.heal(); }
        }
        int best = 0;
        for (int i = 0; i < 5; ++i) best = std::max(best, c.node(i).commit_index);
        total_commits += best;
    }
    std::printf("fuzz finished: no safety violation; %d entries committed across all runs\n", total_commits);
    assert(total_commits > 0);
}

int main(int argc, char** argv) {
    unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 42u;
    scripted_scenario(seed);
    fuzz(30);
    std::printf("all Raft checks passed\n");
    return 0;
}
