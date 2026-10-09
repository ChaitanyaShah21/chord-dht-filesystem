// In-process tests for the replicated tracker's core: the Raft state machine
// (raft.cpp), its log file (raft_log.cpp), its wire format (raft_wire.cpp), and the
// client's way of finding the leader (group.cpp). Decisions D-069 to D-082.
//
// Most of this file is a simulation (D-070). Several Raft cores run against a
// simulated network that delays, loses, duplicates and reorders messages and can
// partition the group, under a simulated clock, with replicas crashing and
// restarting from what their storage holds. Everything is driven from one seed,
// so a failing history replays exactly. After every step the simulation checks
// Raft's safety properties (the paper's Figure 3):
//
//   election safety       at most one leader in any term
//   log matching          two logs with an entry of the same index and term agree
//                         on every entry up to it
//   leader completeness   a new leader holds every entry committed before it won
//   state machine safety  no two replicas apply different entries at one index
//
// and at the end of each history, that every answer clients were given is
// linearizable (a checker for write-once registers, below), and that the group
// recovers -- elects a leader and commits -- once the network heals.

#include "group.h"
#include "lookup.h"
#include "names.h"
#include "raft.h"
#include "raft_log.h"
#include "raft_wire.h"
#include "sha1.h"
#include "store.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int checks   = 0;
int failures = 0;

void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

std::string key(int i) { return chunk_key("manifest " + std::to_string(i)); }

bool same(const Entry &a, const Entry &b) { return a.term == b.term && a.command == b.command; }

// ===========================================================================
// The simulation
// ===========================================================================

struct Op {                         // one client operation, as the client saw it
    bool write = false;
    std::string name, key;          // key: REGISTER only
    std::uint64_t inv = 0;          // when it was sent (the simulation's event counter)
    std::uint64_t resp = 0;         // when the answer came; 0: never (unknown outcome)
    bool rejected = false;          // refused before anything was appended or read
    std::string result;             // "OK", "EXISTS <k>", "FILE <k>", "MISSING"
};

struct SimOptions {
    int size = 3;
    Millis election = 100;
    Millis min_delay = 1, max_delay = 15;      // one-way message delay, uniform
    double loss = 0.0, duplicate = 0.0;        // per message
    bool crashes = false, partitions = false;
    Millis chaos_ms = 8000;                    // hostile phase
    Millis calm_ms = 6000;                     // healed: everything up, no loss
    Millis op_every = 15;                      // a client operation about this often
    std::size_t max_append_bytes = 64 * 1024;
};

class Sim {
public:
    Sim(const SimOptions &o, std::uint64_t seed) : opt_(o), rng_(seed), seed_(seed) {
        nodes_.resize(static_cast<std::size_t>(o.size));
        side_.assign(nodes_.size(), 0);
        for (int i = 0; i < o.size; ++i) {
            nodes_[i].disk = std::make_unique<MemoryStorage>();
            boot(i);
        }
    }

    // ---- what a test reads back ----
    std::string violation;                     // the first safety property broken, with when
    std::vector<Op> ops;
    std::map<Index, Entry> committed;          // every entry any replica has counted as committed
    std::map<Index, Entry> applied;            // every entry any replica has applied
    std::map<Term, int> leaders;               // term -> the replica that led it
    Millis now = 0;
    int elections_won = 0;

    Raft *raft(int i) { return nodes_[i].raft.get(); }
    bool up(int i) const { return nodes_[i].raft != nullptr; }
    int size() const { return opt_.size; }

    // The replica that believes it leads in the highest term, or -1.
    int leader() const {
        int best = -1;
        for (int i = 0; i < opt_.size; ++i)
            if (nodes_[i].raft && nodes_[i].raft->role() == Raft::Role::Leader &&
                (best < 0 || nodes_[i].raft->term() > nodes_[best].raft->term()))
                best = i;
        return best;
    }

    void crash(int i) {
        if (!nodes_[i].raft) return;
        for (auto &kv : nodes_[i].writes) end_unknown(kv.second.second);
        for (auto &kv : nodes_[i].reads) end_unknown(kv.second);
        nodes_[i].writes.clear();
        nodes_[i].reads.clear();
        nodes_[i].raft.reset();               // memory is gone; nodes_[i].disk is what survives
        ++nodes_[i].incarnation;
    }

    void restart(int i) {
        if (!nodes_[i].raft) boot(i);
    }

    void partition(const std::vector<int> &side) { side_ = side; }
    void heal() { std::fill(side_.begin(), side_.end(), 0); }
    void set_loss(double loss, double dup) { opt_.loss = loss; opt_.duplicate = dup; }

    // Run the clock forward `ms` milliseconds: deliver what is due, tick every
    // replica every 5 ms, and (if `clients`) issue operations.
    void run(Millis ms, bool clients, bool chaos) {
        const Millis end = now + ms;
        while (now < end && violation.empty()) {
            ++now;
            deliver_due();
            for (int i = 0; i < opt_.size; ++i)
                if (nodes_[i].raft && (now + i) % 5 == 0) {
                    nodes_[i].raft->tick(now);
                    after(i);
                }
            expire_ops();
            if (clients && chance(1.0 / static_cast<double>(opt_.op_every))) client_op();
            if (chaos) chaos_step();
            if (now % 25 == 0) check_log_matching();
        }
    }

    // A whole history: chaos, then calm. Returns false if a property broke.
    bool history() {
        run(opt_.chaos_ms, true, true);
        heal();
        set_loss(0, 0);
        for (int i = 0; i < opt_.size; ++i) restart(i);
        run(opt_.calm_ms - 1000, true, false);
        run(1000, false, false);                   // a quiet second: followers catch up with the last commit
        check_log_matching();
        return violation.empty();
    }

    // One REGISTER or RESOLVE sent to replica `to` now (D-075, D-076).
    void submit(int to, bool write, const std::string &name, const std::string &k) {
        Op op;
        op.write = write;
        op.name = name;
        op.key = k;
        op.inv = ++events_;
        ops.push_back(op);
        const std::size_t id = ops.size() - 1;
        Node &n = nodes_[to];
        if (!n.raft) { ops[id].rejected = true; ops[id].resp = ++events_; return; }
        if (write) {
            // Registered before proposing, as replica.cpp does, at the index and term
            // the entry will get.
            const Index expect = n.raft->last_index() + 1;
            const Term term = n.raft->term();
            Index index = 0;
            Term t = 0;
            if (n.raft->role() != Raft::Role::Leader) { ops[id].rejected = true; ops[id].resp = ++events_; return; }
            n.writes.emplace(expect, std::make_pair(term, id));
            if (!n.raft->propose(register_command(name, k), index, t, now) || index != expect || t != term) {
                fail("propose did not append where expected");
                return;
            }
        } else {
            const std::uint64_t rid = ++read_ids_;
            n.reads[rid] = id;
            if (!n.raft->read(rid, now)) {
                n.reads.erase(rid);
                ops[id].rejected = true;
                ops[id].resp = ++events_;
                return;
            }
        }
        after(to);
    }

    // The client's view, checked for linearizability. For one write-once register
    // (one name) a history is linearizable exactly when:
    //   - every OK went to a write of the winning key, every EXISTS names the winner
    //     and went to a write of another key, every FILE names the winner;
    //   - and the winning write can be placed at one instant after every MISSING
    //     read was sent and after some write of that key was sent, but before any
    //     answer that shows the name taken (OK, EXISTS or FILE) was received.
    // The winner is the first REGISTER of the name in the applied log. Names are
    // independent, and linearizability is local (Herlihy and Wing), so checking
    // each name alone checks the whole history.
    std::string check_linearizable() const {
        std::map<std::string, std::string> winner;
        for (const auto &kv : applied) {
            const std::vector<std::string> w = split(kv.second.command);
            if (w.size() == 3 && w[0] == "REGISTER" && !winner.count(w[1])) winner[w[1]] = w[2];
        }
        std::set<std::string> names;
        for (const Op &op : ops) names.insert(op.name);
        for (const std::string &name : names) {
            const bool taken = winner.count(name) > 0;
            const std::string win = taken ? winner.at(name) : "";
            std::uint64_t first_observed = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t last_missing_sent = 0, first_winning_write = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t i = 0; i < ops.size(); ++i) {
                const Op &op = ops[i];
                if (op.name != name) continue;
                if (op.write && !op.rejected && op.key == win) first_winning_write = std::min(first_winning_write, op.inv);
                if (op.resp == 0 || op.rejected) continue;
                const std::string where = "op " + std::to_string(i) + " on " + name + ": ";
                if (op.write && op.result == "OK") {
                    if (!taken || op.key != win) return where + "OK for a key that did not win";
                    first_observed = std::min(first_observed, op.resp);
                } else if (op.write && op.result.compare(0, 7, "EXISTS ") == 0) {
                    if (!taken || op.result.substr(7) != win || op.key == win) return where + "a wrong EXISTS";
                    first_observed = std::min(first_observed, op.resp);
                } else if (!op.write && op.result.compare(0, 5, "FILE ") == 0) {
                    if (!taken || op.result.substr(5) != win) return where + "FILE with a key that did not win";
                    first_observed = std::min(first_observed, op.resp);
                } else if (!op.write && op.result == "MISSING") {
                    last_missing_sent = std::max(last_missing_sent, op.inv);
                } else {
                    return where + "an answer that makes no sense: " + op.result;
                }
            }
            if (first_observed != std::numeric_limits<std::uint64_t>::max() &&
                (last_missing_sent > first_observed || first_winning_write > first_observed))
                return name + ": no instant for the write fits -- a stale MISSING, or an answer before any such write was sent";
        }
        return "";
    }

    // Every live replica has applied as far as the leader has committed, and the
    // leader has committed an entry of its own term (it is working, not just elected).
    bool settled() const {
        const int l = leader();
        if (l < 0) return false;
        const Raft &lr = *nodes_[l].raft;
        if (lr.term_at(lr.commit_index()) != lr.term()) return false;
        for (int i = 0; i < opt_.size; ++i)
            if (nodes_[i].raft && nodes_[i].raft->last_applied() < lr.commit_index()) return false;
        return true;
    }

    std::uint64_t seed() const { return seed_; }

private:
    enum class Kind { VoteRequest, VoteReply, AppendRequest, AppendReply };
    struct Msg {
        int from = 0, to = 0;
        int from_life = 0;               // the sender's incarnation: a reply to a crashed sender is dropped
        Kind kind = Kind::VoteRequest;
        VoteRequest vq;
        VoteReply vr;
        AppendRequest aq;
        AppendReply ar;
    };
    struct Node {
        std::unique_ptr<MemoryStorage> disk;
        std::unique_ptr<Raft> raft;
        int incarnation = 0;
        Namespace sm;                                            // this life's state machine
        std::multimap<Index, std::pair<Term, std::size_t>> writes;   // index -> (term, op)
        std::map<std::uint64_t, std::size_t> reads;              // read id -> op
        Index seen_commit = 0;                                   // for the committed-entries check
        Raft::Role seen_role = Raft::Role::Follower;
    };

    static std::vector<std::string> split(const std::string &s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string w;
        while (in >> w) out.push_back(w);
        return out;
    }

    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng_) < p; }
    int pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng_); }
    Millis between(Millis a, Millis b) { return std::uniform_int_distribution<Millis>(a, b)(rng_); }

    void fail(const std::string &what) {
        if (violation.empty())
            violation = what + " (seed " + std::to_string(seed_) + ", t=" + std::to_string(now) + " ms)";
    }

    void boot(int i) {
        Node &n = nodes_[i];
        n.sm = Namespace{};
        n.seen_commit = 0;
        n.seen_role = Raft::Role::Follower;
        RaftConfig c;
        c.id = i;
        c.size = opt_.size;
        c.election = opt_.election;
        c.max_append_bytes = opt_.max_append_bytes;
        c.seed = seed_ * 7919 + static_cast<std::uint64_t>(i) * 104729 + static_cast<std::uint64_t>(n.incarnation);
        n.raft = std::make_unique<Raft>(c, *n.disk, n.disk->durable,
                                        [this, i](Index idx, const Entry &e) { on_apply(i, idx, e); }, now);
        after(i);
    }

    // A replica applied an entry: state machine safety, and the client waiting on it.
    void on_apply(int i, Index idx, const Entry &e) {
        const auto it = applied.find(idx);
        if (it == applied.end()) applied[idx] = e;
        else if (!same(it->second, e))
            fail("state machine safety: replica " + std::to_string(i) + " applied a different entry at " + std::to_string(idx));
        const std::string result = apply_command(nodes_[i].sm, e.command);
        Node &n = nodes_[i];
        const auto range = n.writes.equal_range(idx);
        for (auto w = range.first; w != range.second; ++w) {
            if (w->second.first == e.term) end_with(w->second.second, result);
            else end_unknown(w->second.second);            // overwritten: it never took effect
        }
        n.writes.erase(range.first, range.second);
    }

    void end_with(std::size_t op, const std::string &result) {
        if (ops[op].resp) return;
        ops[op].result = result;
        ops[op].resp = ++events_;
    }
    void end_unknown(std::size_t op) { (void)op; }       // the client never hears; resp stays 0

    // After every call into replica i: send what it wants sent, answer the reads it
    // resolved, and check the properties that can be checked at once.
    void after(int i) {
        Node &n = nodes_[i];
        if (!n.raft) return;
        Raft &r = *n.raft;
        for (Outgoing &o : r.take_outgoing()) {
            Msg m;
            m.from = i;
            m.to = o.to;
            m.from_life = n.incarnation;
            m.kind = o.is_vote ? Kind::VoteRequest : Kind::AppendRequest;
            m.vq = o.vote;
            m.aq = std::move(o.append);
            send(m);
        }
        for (const Raft::ReadResult &rr : r.take_reads()) {
            const auto it = n.reads.find(rr.id);
            if (it == n.reads.end()) continue;
            if (rr.ok) {
                NamesRequest q;
                q.kind = NamesRequest::Kind::Resolve;
                q.name = ops[it->second].name;
                end_with(it->second, answer_read(n.sm, q));
            }
            n.reads.erase(it);
        }
        // Election safety.
        if (r.role() == Raft::Role::Leader) {
            const auto it = leaders.find(r.term());
            if (it == leaders.end()) leaders[r.term()] = i;
            else if (it->second != i)
                fail("election safety: replicas " + std::to_string(it->second) + " and " + std::to_string(i) +
                     " both lead term " + std::to_string(r.term()));
        }
        // Committed entries never change, whoever counted them.
        for (Index idx = n.seen_commit + 1; idx <= r.commit_index(); ++idx) {
            const auto it = committed.find(idx);
            if (it == committed.end()) committed[idx] = r.log()[idx - 1];
            else if (!same(it->second, r.log()[idx - 1]))
                fail("a committed entry changed at index " + std::to_string(idx) + " (replica " + std::to_string(i) + ")");
        }
        if (r.commit_index() > n.seen_commit) n.seen_commit = r.commit_index();
        // Leader completeness: on winning, a leader holds everything committed so far.
        if (r.role() == Raft::Role::Leader && n.seen_role != Raft::Role::Leader) {
            ++elections_won;
            for (const auto &kv : committed)
                if (kv.first > r.last_index() || !same(r.log()[kv.first - 1], kv.second)) {
                    fail("leader completeness: replica " + std::to_string(i) + " won term " + std::to_string(r.term()) +
                         " without committed entry " + std::to_string(kv.first));
                    break;
                }
        }
        n.seen_role = r.role();
        if (r.stopped()) fail("replica " + std::to_string(i) + " stopped");
    }

    void check_log_matching() {
        for (int a = 0; a < opt_.size; ++a)
            for (int b = a + 1; b < opt_.size; ++b) {
                if (!nodes_[a].raft || !nodes_[b].raft) continue;
                const std::vector<Entry> &la = nodes_[a].raft->log(), &lb = nodes_[b].raft->log();
                const std::size_t m = std::min(la.size(), lb.size());
                std::size_t agree = 0;                     // the highest index with the same term
                for (std::size_t k = m; k > 0; --k)
                    if (la[k - 1].term == lb[k - 1].term) { agree = k; break; }
                for (std::size_t k = 0; k < agree; ++k)
                    if (!same(la[k], lb[k])) {
                        fail("log matching: replicas " + std::to_string(a) + " and " + std::to_string(b) +
                             " agree on the term at " + std::to_string(agree) + " but differ at " + std::to_string(k + 1));
                        return;
                    }
            }
    }

    // The network: a message is lost, delayed, or (sometimes) delivered twice.
    void send(const Msg &m) {
        if (chance(opt_.loss)) return;
        queue_.emplace(now + between(opt_.min_delay, opt_.max_delay), m);
        if (chance(opt_.duplicate)) queue_.emplace(now + between(opt_.min_delay, opt_.max_delay), m);
    }

    void deliver_due() {
        while (!queue_.empty() && queue_.begin()->first <= now && violation.empty()) {
            const Msg m = queue_.begin()->second;
            queue_.erase(queue_.begin());
            ++events_;
            if (side_[m.from] != side_[m.to]) continue;     // partitioned: lost
            Node &to = nodes_[m.to];
            if (!to.raft) continue;                         // crashed: lost
            switch (m.kind) {
                case Kind::VoteRequest: {
                    Msg reply = m;
                    reply.kind = Kind::VoteReply;
                    reply.vr = to.raft->handle_vote(m.vq, now);
                    after(m.to);
                    reply_back(reply);
                    break;
                }
                case Kind::AppendRequest: {
                    Msg reply = m;
                    reply.kind = Kind::AppendReply;
                    reply.ar = to.raft->handle_append(m.aq, now);
                    after(m.to);
                    reply_back(reply);
                    break;
                }
                case Kind::VoteReply:
                case Kind::AppendReply:
                    break;                                  // replies travel in replies_ (reply_back)
            }
        }
    }

    // A reply travels from the request's receiver back to its sender, through the
    // same lossy network. It is handed over with the request it answers, as a
    // fresh-connection call does (D-071).
    void reply_back(const Msg &reply) {
        if (chance(opt_.loss)) return;
        const Millis at = now + between(opt_.min_delay, opt_.max_delay);
        replies_.emplace(at, reply);
        if (chance(opt_.duplicate)) replies_.emplace(now + between(opt_.min_delay, opt_.max_delay), reply);
        deliver_replies_due();
    }

    void deliver_replies_due() {
        while (!replies_.empty() && replies_.begin()->first <= now && violation.empty()) {
            const Msg m = replies_.begin()->second;
            replies_.erase(replies_.begin());
            ++events_;
            if (side_[m.from] != side_[m.to]) continue;
            Node &back = nodes_[m.from];                   // the request's sender
            if (!back.raft || back.incarnation != m.from_life) continue;
            if (m.kind == Kind::VoteReply) back.raft->handle_vote_reply(m.to, m.vq, m.vr, now);
            else back.raft->handle_append_reply(m.to, m.aq, m.ar, now);
            after(m.from);
        }
    }

    void expire_ops() {
        deliver_replies_due();
        // A client gives up after 2 s: its operation's outcome stays unknown.
        for (Node &n : nodes_) {
            for (auto it = n.writes.begin(); it != n.writes.end();) {
                if (now - op_sent_ms(it->second.second) > 2000) it = n.writes.erase(it);
                else ++it;
            }
            for (auto it = n.reads.begin(); it != n.reads.end();) {
                if (now - op_sent_ms(it->second) > 2000) it = n.reads.erase(it);
                else ++it;
            }
        }
    }
    Millis op_sent_ms(std::size_t op) const { return op_ms_.count(op) ? op_ms_.at(op) : now; }

    void client_op() {
        static const char *const NAMES[] = {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"};
        const std::string name = NAMES[pick(5)];
        const bool write = chance(0.5);
        const std::string k = key(static_cast<int>(name[0]) * 10 + pick(3));    // three possible keys per name
        // Mostly to whoever claims to lead (a real client is redirected there);
        // sometimes to anyone, which must refuse or answer correctly.
        int to = leader();
        if (to < 0 || chance(0.2)) to = pick(opt_.size);
        op_ms_[ops.size()] = now;
        submit(to, write, name, k);
    }

    void chaos_step() {
        if (opt_.crashes && chance(1.0 / 400)) crash(pick(opt_.size));
        if (opt_.crashes && chance(1.0 / 300)) restart(pick(opt_.size));
        if (opt_.partitions && chance(1.0 / 700)) {
            std::vector<int> side(static_cast<std::size_t>(opt_.size));
            for (int &s : side) s = pick(2);
            partition(side);
        }
        if (opt_.partitions && chance(1.0 / 900)) heal();
    }

    SimOptions opt_;
    std::mt19937_64 rng_;
    std::uint64_t seed_;
    std::vector<Node> nodes_;
    std::vector<int> side_;
    std::multimap<Millis, Msg> queue_, replies_;
    std::uint64_t events_ = 0, read_ids_ = 0;
    std::map<std::size_t, Millis> op_ms_;
};

// ===========================================================================
// Directed tests on the core: each property built by hand
// ===========================================================================

// A small harness for hand-driven scenarios: cores whose clocks never move unless
// a test moves them, and calls delivered only when a test delivers them.
struct Hand {
    std::vector<std::unique_ptr<MemoryStorage>> disks;
    std::vector<std::unique_ptr<Raft>> nodes;
    std::vector<std::vector<Entry>> applied;
    int size;
    Millis election;

    Hand(int n, Millis e = 100, std::size_t max_bytes = 64 * 1024) : size(n), election(e) {
        for (int i = 0; i < n; ++i) disks.push_back(std::make_unique<MemoryStorage>());
        applied.resize(static_cast<std::size_t>(n));
        nodes.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) boot(i, max_bytes);
    }
    void boot(int i, std::size_t max_bytes = 64 * 1024) {
        RaftConfig c;
        c.id = i;
        c.size = size;
        c.election = election;
        c.max_append_bytes = max_bytes;
        c.seed = static_cast<std::uint64_t>(i) + 1;
        applied[i].clear();
        nodes[i] = std::make_unique<Raft>(c, *disks[i], disks[i]->durable,
                                          [this, i](Index, const Entry &e) { applied[i].push_back(e); }, 0);
    }
    Raft &operator[](int i) { return *nodes[i]; }

    // Make replica c a candidate (its timer fired) and return its vote requests.
    std::vector<Outgoing> stand(int c, Millis now) {
        nodes[c]->tick(now);
        return nodes[c]->take_outgoing();
    }
    // Deliver c's vote request to each voter in `voters` and the replies back.
    void votes(int c, const std::vector<Outgoing> &reqs, const std::vector<int> &voters, Millis now) {
        for (const Outgoing &o : reqs)
            if (std::find(voters.begin(), voters.end(), o.to) != voters.end()) {
                const VoteReply r = nodes[o.to]->handle_vote(o.vote, now);
                nodes[c]->handle_vote_reply(o.to, o.vote, r, now);
            }
    }
    // Deliver every pending call from `from` to the replicas in `to`, with replies,
    // until nothing more is sent to them (other calls are dropped).
    void pump(int from, const std::vector<int> &to, Millis now, int rounds = 20) {
        for (int k = 0; k < rounds; ++k) {
            std::vector<Outgoing> out = nodes[from]->take_outgoing();
            if (out.empty()) {
                nodes[from]->tick(now);           // a heartbeat, if one is due
                out = nodes[from]->take_outgoing();
                if (out.empty()) return;
            }
            for (const Outgoing &o : out) {
                if (std::find(to.begin(), to.end(), o.to) == to.end()) continue;
                if (o.is_vote) {
                    const VoteReply r = nodes[o.to]->handle_vote(o.vote, now);
                    nodes[from]->handle_vote_reply(o.to, o.vote, r, now);
                } else {
                    const AppendReply r = nodes[o.to]->handle_append(o.append, now);
                    nodes[from]->handle_append_reply(o.to, o.append, r, now);
                }
            }
        }
    }
};

void test_single_replica() {
    std::printf("-- a group of one\n");
    Hand h(1);
    check(h[0].role() == Raft::Role::Leader && h[0].term() == 1, "a group of one leads at once, in term 1");
    check(h[0].commit_index() == 1 && h.applied[0].size() == 1 && h.applied[0][0].command == "NOOP",
          "and its no-op is committed and applied on its own flush");
    Index i = 0;
    Term t = 0;
    check(h[0].propose("REGISTER a " + key(1), i, t, 0) && i == 2 && t == 1, "a proposal lands at index 2, term 1");
    check(h[0].commit_index() == 2 && h.applied[0].size() == 2, "and is committed and applied before propose returns");
    check(h[0].read(7, 0), "a read is accepted");
    const std::vector<Raft::ReadResult> rr = h[0].take_reads();
    check(rr.size() == 1 && rr[0].id == 7 && rr[0].ok, "and confirmed at once: a majority of one");
    // Restart: same disk, a higher term, the log replayed and recommitted.
    h.boot(0);
    check(h[0].role() == Raft::Role::Leader && h[0].term() == 2 && h[0].last_index() == 3,
          "restarted: term 2, the two old entries and a new no-op");
    check(h.applied[0].size() == 3 && h.applied[0][1].command == "REGISTER a " + key(1),
          "and every old entry is applied again, in order");
}

void test_votes() {
    std::printf("-- votes\n");
    Hand h(3);
    // Replica 0 stands in term 1 and asks 1; 1 grants.
    std::vector<Outgoing> r0 = h.stand(0, 1000);
    check(h[0].role() == Raft::Role::Candidate && h[0].term() == 1 && r0.size() == 2, "a timer firing makes a candidate in term 1, asking both");
    check(h.disks[0]->durable.term == 1 && h.disks[0]->durable.voted_for == 0,
          "its own vote is on disk before its requests are released");
    VoteReply v = h[1].handle_vote(r0[0].to == 1 ? r0[0].vote : r0[1].vote, 1000);
    check(v.granted && v.term == 1, "replica 1 grants its vote in term 1");
    check(h.disks[1]->durable.voted_for == 0, "and the vote is on disk before the reply leaves");
    // Replica 2 stands in term 1 too; 1 has voted already.
    VoteRequest other{1, 2, 0, 0};
    check(!h[1].handle_vote(other, 1000).granted, "one vote per term: a second candidate in term 1 is refused");
    check(h[1].handle_vote(r0[0].to == 1 ? r0[0].vote : r0[1].vote, 1000).granted,
          "the same candidate asking again is granted again (a duplicate request)");
    // Restart replica 1 from its disk: the vote must survive.
    h.boot(1);
    check(h[1].term() == 1 && h[1].voted_for() == 0, "restarted, replica 1 remembers term 1 and its vote");
    check(!h[1].handle_vote(other, 1000).granted, "and still refuses a second candidate in term 1");

    // A vote granted in a term the voter already held -- it learned the term first from
    // a candidate it refused -- must reach the disk too. Here no term change accompanies
    // the vote, so only the vote itself can make the step save. (Found by mutation
    // testing: the test above only covered a vote that raised the term, which saves
    // anyway.)
    Hand s(3);
    s.disks[1]->durable = Durable{2, NOBODY, {Entry{1, "NOOP"}, Entry{2, "NOOP"}}};
    s.boot(1);
    check(!s[1].handle_vote(VoteRequest{5, 2, 1, 1}, 0).granted && s[1].term() == 5,
          "a stale candidate is refused, and its term 5 adopted with no vote");
    check(s[1].handle_vote(VoteRequest{5, 0, 2, 2}, 0).granted, "then a candidate with a good log is granted, in the same term");
    s.boot(1);
    check(s[1].term() == 5 && s[1].voted_for() == 0, "restarted, the same-term vote is remembered");
    check(!s[1].handle_vote(VoteRequest{5, 2, 9, 2}, 0).granted, "and a third candidate in term 5 is refused");

    // A granted vote counts only in the term it was asked for. Replica 0 stands in a term,
    // its reply from replica 1 is delayed past the next timeout, and it arrives after 0 has
    // stood again: counting it would make 0 leader of the new term with a vote nobody
    // cast in that term. (Found by mutation testing: the simulation's delays rarely
    // outlast an election timeout; a vote call behind a slow flush can.)
    Hand d(3);
    const std::vector<Outgoing> first = d.stand(0, 1000);
    const Outgoing to1 = first[0].to == 1 ? first[0] : first[1];
    const VoteReply late = d[1].handle_vote(to1.vote, 1000);      // granted, then held up in the network
    check(late.granted && late.term == 1, "replica 1 grants replica 0's term-1 request");
    d.stand(0, 99000);                                           // 0 times out and stands in term 2
    check(d[0].term() == 2 && d[0].role() == Raft::Role::Candidate, "replica 0 stands again, in term 2");
    d[0].handle_vote_reply(1, to1.vote, late, 99001);
    check(d[0].role() == Raft::Role::Candidate, "the late term-1 grant arrives: it is not counted in term 2");

    // Up-to-date rule (section 5.4.1).
    Hand u(3);
    u.disks[1]->durable = Durable{3, NOBODY, {Entry{1, "NOOP"}, Entry{3, "NOOP"}}};
    u.boot(1);
    check(!u[1].handle_vote(VoteRequest{4, 0, 5, 2}, 0).granted, "refused: the candidate's last term (2) is older than mine (3), however long its log");
    check(!u[1].handle_vote(VoteRequest{5, 0, 1, 3}, 0).granted, "refused: the same last term, and a shorter log");
    check(u[1].term() == 5 && u[1].voted_for() == NOBODY, "a higher term is adopted even when the vote is refused");
    check(u[1].handle_vote(VoteRequest{6, 0, 2, 3}, 0).granted, "granted: the same last term and an equal log");
    check(u[1].handle_vote(VoteRequest{7, 2, 1, 4}, 0).granted, "granted: a newer last term, even with a shorter log");
    check(!u[1].handle_vote(VoteRequest{3, 2, 9, 9}, 0).granted && u[1].term() == 7, "a request from an old term is refused");
}

void test_append_rules() {
    std::printf("-- appending\n");
    Hand h(3);
    // A follower given a log with an uncommitted tail from term 2.
    h.disks[1]->durable = Durable{2, NOBODY, {Entry{1, "NOOP"}, Entry{2, "NOOP"}, Entry{2, "REGISTER x " + key(9)}}};
    h.boot(1);
    // A leader of term 3 whose log is [1:t1, 2:t3]: prev (2, 3) does not match.
    AppendRequest a;
    a.term = 3; a.leader = 0; a.prev_index = 2; a.prev_term = 3; a.commit = 0; a.seq = 1;
    AppendReply r = h[1].handle_append(a, 0);
    check(!r.success && r.index == 2, "a mismatch at index 2 suggests trying from the start of term 2 there (index 2)");
    a.prev_index = 5; a.prev_term = 3;
    r = h[1].handle_append(a, 0);
    check(!r.success && r.index == 4, "a gap: prev_index past my end suggests my end + 1");
    a.prev_index = 1; a.prev_term = 1; a.entries = {Entry{3, "NOOP"}}; a.commit = 2;
    r = h[1].handle_append(a, 0);
    check(r.success && r.index == 2 && h[1].last_index() == 2 && h[1].term_at(2) == 3,
          "a conflict at index 2 cuts my uncommitted tail and takes the leader's entry");
    check(h[1].commit_index() == 2, "commit follows the leader's, up to what this call proved");
    check(h.disks[1]->durable.log.size() == 2 && h.disks[1]->durable.log[1].term == 3, "and the cut is on disk before the reply");

    // A stale, shorter call must not cut entries a later call delivered.
    AppendRequest longer;
    longer.term = 3; longer.leader = 0; longer.prev_index = 2; longer.prev_term = 3; longer.seq = 2;
    longer.entries = {Entry{3, "REGISTER a " + key(1)}, Entry{3, "REGISTER b " + key(2)}};
    check(h[1].handle_append(longer, 0).success && h[1].last_index() == 4, "a call adds two entries");
    AppendRequest older = longer;
    older.seq = 1;
    older.entries = {Entry{3, "REGISTER a " + key(1)}};
    r = h[1].handle_append(older, 0);
    check(r.success && r.index == 3 && h[1].last_index() == 4, "an older, shorter copy arriving late changes nothing and reports 3");

    // Commit no further than the call proved.
    Hand c(3);
    c.disks[1]->durable = Durable{2, NOBODY, {Entry{1, "NOOP"}, Entry{2, "NOOP"}, Entry{2, "NOOP"}}};
    c.boot(1);
    AppendRequest hb;
    hb.term = 3; hb.leader = 0; hb.prev_index = 1; hb.prev_term = 1; hb.commit = 3; hb.seq = 1;
    check(c[1].handle_append(hb, 0).success && c[1].commit_index() == 1,
          "a heartbeat matching only index 1 commits only to 1, though the leader's commit is 3 and I hold 3 entries");

    AppendRequest stale;
    stale.term = 1; stale.leader = 2;
    r = c[1].handle_append(stale, 0);
    check(!r.success && r.term == 3, "a call from an old term is refused with my term");
}

// Figure 8 of the Raft paper: an entry from an earlier term, stored on a majority,
// is still not safe to commit by counting -- a later leader can overwrite it. Five
// replicas, one entry per call so replication stops exactly where the figure needs
// it to, driven through the core's inputs so every step is exact. The paper's
// servers S1..S5 are replicas 0..4.
void test_figure_8_exact() {
    std::printf("-- Figure 8, exact\n");
    // Start from durable states that are the figure's picture (c): S1 is about to
    // lead term 4; index 2 holds a term-2 entry on S1 and S2; S5 holds a term-3 entry
    // at index 2; everyone holds the committed index 1 (term 1).
    Hand h(5, 100, 1);
    const Entry e1{1, "NOOP"}, e2{2, "REGISTER a " + key(2)}, e3{3, "REGISTER a " + key(3)};
    h.disks[0]->durable = Durable{3, NOBODY, {e1, e2}};       // S1
    h.disks[1]->durable = Durable{3, NOBODY, {e1, e2}};       // S2
    h.disks[2]->durable = Durable{3, NOBODY, {e1}};           // S3
    h.disks[3]->durable = Durable{3, NOBODY, {e1}};           // S4
    h.disks[4]->durable = Durable{3, 4, {e1, e3}};            // S5, which led term 3
    for (int i = 0; i < 5; ++i) h.boot(i, 1);
    // (c) S1 wins term 4 with S2 and S3. It appends its term-4 no-op at index 3.
    h.votes(0, h.stand(0, 1000), {1, 2}, 1000);
    check(h[0].role() == Raft::Role::Leader && h[0].term() == 4 && h[0].last_index() == 3, "(c) S1 leads term 4; its no-op is index 3");
    // S2 already holds index 2. A restarted S1 does not know that (what each follower
    // holds is volatile), so S2 acknowledges a call that carries index 2 alone.
    std::vector<Outgoing> out = h[0].take_outgoing();
    AppendRequest only2;
    only2.term = 4; only2.leader = 0; only2.prev_index = 1; only2.prev_term = 1; only2.entries = {e2}; only2.seq = 100;
    const AppendReply ack2 = h[1].handle_append(only2, 1000);
    h[0].handle_append_reply(1, only2, ack2, 1000);
    // Replicate index 2 (term 2) to S3 only -- one entry per call -- and stop there.
    for (const Outgoing &o : out) {
        if (o.to != 2) continue;
        // S3 is missing index 2: the first call (prev 2) fails, the retry (prev 1) carries index 2.
        AppendReply r = h[2].handle_append(o.append, 1000);
        h[0].handle_append_reply(2, o.append, r, 1000);
        for (const Outgoing &o2 : h[0].take_outgoing())
            if (o2.to == 2 && o2.append.prev_index == 1 && o2.append.entries.size() == 1) {
                AppendReply r2 = h[2].handle_append(o2.append, 1000);
                h[0].handle_append_reply(2, o2.append, r2, 1000);
            }
    }
    check(h[1].last_index() == 2 && h[2].last_index() == 2 && h[2].term_at(2) == 2,
          "index 2 (term 2) is now on S1, S2 and S3 -- a majority -- and S1 knows it");
    check(h[0].commit_index() < 2, "and S1 still does NOT commit it: it is not of S1's term");
    // (d) S1 crashes before index 3 replicates. S5 stands in term 5 with S2, S3, S4:
    // its last term (3) is newer than theirs (2), so they vote for it.
    h.nodes[0].reset();
    h.stand(4, 99000);                                    // term 4: S2 and S3 already voted in it
    h.votes(4, h.stand(4, 199000), {1, 2, 3}, 2000);
    check(h[4].role() == Raft::Role::Leader && h[4].term() == 5, "(d) S5 wins term 5 with S2, S3 and S4");
    h.pump(4, {1, 2, 3}, 2000);
    check(h[1].term_at(2) == 3 && h[2].term_at(2) == 3, "and overwrites index 2 on S2 and S3 with its term-3 entry");
    check(h[4].commit_index() >= 2, "which it commits, with its own term-5 no-op");
    // Had S1 counted copies, it would have committed index 2 (term 2) in (c), told a
    // client OK, and S5 would now have overwritten a committed entry. The planted bug
    // "commit a previous term's entry by counting" is caught exactly here.
    h.pump(4, {1, 2, 3}, 2050);                           // a heartbeat later, the followers learn the commit
    check(h.applied[1].size() >= 2 && h.applied[1][1].term == 3, "S2 applies the term-3 entry at index 2, never the term-2 one");
}

void test_commit_rule_and_reads() {
    std::printf("-- commit, ReadIndex and check-quorum\n");
    Hand h(3);
    h.votes(0, h.stand(0, 1000), {1}, 1000);
    check(h[0].role() == Raft::Role::Leader, "replica 0 leads with replica 1's vote");
    check(h[0].commit_index() == 0, "its no-op is not committed before any follower has it");
    check(h[0].read(1, 1000), "a read is accepted by the leader");
    std::vector<Outgoing> out = h[0].take_outgoing();
    check(h[0].take_reads().empty(), "but not answered: nothing of this term is committed, and no majority has answered");
    // Deliver only to replica 1. These calls were made when the leader won, BEFORE the
    // read arrived.
    for (const Outgoing &o : out)
        if (o.to == 1) {
            const AppendReply r = h[1].handle_append(o.append, 1000);
            h[0].handle_append_reply(1, o.append, r, 1000);
        }
    check(h[0].commit_index() == 1, "replica 1's acknowledgement commits the no-op (2 of 3)");
    std::vector<Raft::ReadResult> rr = h[0].take_reads();
    check(rr.empty(), "but does not confirm the read: those calls were sent before it arrived");
    h.pump(0, {1}, 1000);
    rr = h[0].take_reads();
    check(rr.size() == 1 && rr[0].ok, "a reply to the next call does: the no-op committed, a majority acknowledged since");

    // A read arriving now must wait for calls sent AFTER it: a reply to an earlier
    // call proves nothing about leadership at the read's arrival.
    h[0].take_outgoing();
    Index i = 0;
    Term t = 0;
    h[0].propose("REGISTER a " + key(1), i, t, 1000);
    std::vector<Outgoing> before = h[0].take_outgoing();       // sent before the read
    check(h[0].read(2, 1001), "a second read");
    for (const Outgoing &o : before)
        if (o.to == 1) {
            const AppendReply r = h[1].handle_append(o.append, 1001);
            h[0].handle_append_reply(1, o.append, r, 1001);
        }
    rr = h[0].take_reads();
    check(rr.empty(), "a reply to a call sent before the read does not confirm it");
    h.pump(0, {1}, 1002);
    rr = h[0].take_reads();
    check(rr.size() == 1 && rr[0].ok && h[0].commit_index() == 2, "a reply to a later call does, once index 2 is committed and applied");

    // A leader cut off from everyone: reads never confirm, and check-quorum deposes it.
    check(h[0].read(3, 1100), "a read at a leader about to be cut off");
    h[0].take_outgoing();
    for (Millis now = 1100; now <= 1100 + 2 * h.election + 10; now += 5) {
        h[0].tick(now);
        h[0].take_outgoing();
    }
    check(h[0].role() == Raft::Role::Follower, "after 2E with no reply from a majority, the leader steps down (check-quorum)");
    rr = h[0].take_reads();
    check(rr.size() == 1 && !rr[0].ok, "and its pending read fails rather than answer");
    check(!h[0].read(4, 1400), "a follower refuses reads");
    check(!h[0].propose("NOOP", i, t, 1400), "and proposals");
}

void test_storage_failure() {
    std::printf("-- a disk that fails\n");
    Hand h(3);
    h.disks[1]->fail = true;
    const VoteReply v = h[1].handle_vote(VoteRequest{1, 0, 0, 0}, 0);
    check(!v.granted && h[1].stopped(), "a vote that cannot be flushed is not granted, and the replica stops");
    check(h[1].take_outgoing().empty(), "a stopped replica sends nothing");
    AppendRequest a;
    a.term = 1; a.leader = 0;
    check(!h[1].handle_append(a, 0).success, "and acknowledges nothing");
}

// Crash the leader at every step of one replication, and check that whatever the
// client was told survives, and nothing else is invented (one history per step).
void test_crash_at_every_step() {
    std::printf("-- the leader crashed at every step of a replication\n");
    int histories = 0, broken = 0, acknowledged = 0;
    for (int step = 0; step < 120; ++step) {
        SimOptions o;
        o.size = 3;
        o.election = 100;
        Sim sim(o, 5000 + static_cast<std::uint64_t>(step));
        sim.run(1500, false, false);                         // a leader, settled
        const int l = sim.leader();
        if (l < 0) { ++broken; continue; }
        sim.submit(l, true, "x.txt", key(77));
        // Let `step` milliseconds pass -- the entry part way through replication --
        // then crash the leader, wait, restart it, and let the group settle.
        sim.run(step / 4, false, false);
        sim.crash(l);
        sim.run(800, false, false);
        sim.restart(l);
        sim.run(1500, false, false);
        // Ask the group now.
        const int l2 = sim.leader();
        if (l2 >= 0) sim.submit(l2, false, "x.txt", "");
        sim.run(500, false, false);
        ++histories;
        if (!sim.violation.empty() || !sim.check_linearizable().empty()) {
            ++broken;
            std::printf("    step %d: %s %s\n", step, sim.violation.c_str(), sim.check_linearizable().c_str());
        }
        if (sim.ops[0].resp != 0 && sim.ops[0].result == "OK") {
            ++acknowledged;
            check(sim.ops.size() > 1 && sim.ops[1].result == "FILE " + key(77),
                  "step " + std::to_string(step) + ": an acknowledged registration is found after the crash");
        }
    }
    check(broken == 0, std::to_string(histories) + " crash points: every property held (" + std::to_string(broken) + " broken)");
    check(acknowledged > 10 && acknowledged < histories,
          "some crashes came before the acknowledgement and some after (" + std::to_string(acknowledged) + " of " +
              std::to_string(histories) + " acknowledged)");
}

// Randomised histories from fixed seeds: the safety properties after every step,
// linearizability at the end, and recovery once healed.
void test_random_histories() {
    const char *env = std::getenv("RAFT_SEEDS");
    const int seeds = env ? std::atoi(env) : 60;
    struct Mix {
        const char *what;
        SimOptions o;
    };
    std::vector<Mix> mixes;
    {
        SimOptions o; o.size = 3; o.loss = 0.05; o.duplicate = 0.05; o.crashes = true; o.partitions = true;
        mixes.push_back({"3 replicas: loss, duplication, reordering, crashes, partitions", o});
    }
    {
        SimOptions o; o.size = 5; o.loss = 0.05; o.duplicate = 0.05; o.crashes = true; o.partitions = true;
        mixes.push_back({"5 replicas: the same", o});
    }
    {
        SimOptions o; o.size = 3; o.loss = 0.30; o.duplicate = 0.20; o.max_delay = 60; o.crashes = true; o.partitions = true;
        o.max_append_bytes = 300;                         // a few entries per call: long catch-ups
        mixes.push_back({"3 replicas: 30% loss, 20% duplication, slow, small batches", o});
    }
    for (const Mix &m : mixes) {
        std::printf("-- %d histories: %s\n", seeds, m.what);
        int bad = 0, unsettled = 0, linear_bad = 0;
        long elections = 0, ops_answered = 0, ops_total = 0;
        for (int s = 0; s < seeds; ++s) {
            Sim sim(m.o, 100000 + static_cast<std::uint64_t>(s) * 31 + static_cast<std::uint64_t>(m.o.size));
            const bool ok = sim.history();
            const std::string lin = sim.check_linearizable();
            if (!ok) { ++bad; if (bad <= 3) std::printf("    %s\n", sim.violation.c_str()); }
            if (!lin.empty()) { ++linear_bad; if (linear_bad <= 3) std::printf("    seed %llu: %s\n", static_cast<unsigned long long>(sim.seed()), lin.c_str()); }
            if (!sim.settled()) { ++unsettled; if (unsettled <= 3) std::printf("    seed %llu: did not settle after healing\n", static_cast<unsigned long long>(sim.seed())); }
            elections += sim.elections_won;
            for (const Op &op : sim.ops) {
                ++ops_total;
                if (op.resp && !op.rejected) ++ops_answered;
            }
        }
        check(bad == 0, "election safety, log matching, leader completeness, state machine safety: " + std::to_string(bad) + " histories broke one");
        check(linear_bad == 0, "every client-visible answer linearizable: " + std::to_string(linear_bad) + " histories were not");
        check(unsettled == 0, "every history elects a leader and commits once healed: " + std::to_string(unsettled) + " did not");
        check(ops_answered > ops_total / 4, "and the histories did real work: " + std::to_string(ops_answered) + " of " +
                                                std::to_string(ops_total) + " operations answered, " +
                                                std::to_string(elections) + " elections won");
    }
}

// The checker itself, against histories built by hand: it must reject the stale
// read and the lost write it exists to catch, and accept the legal overlaps.
void test_linearizability_checker() {
    std::printf("-- the linearizability checker\n");
    SimOptions o;
    Sim sim(o, 1);
    sim.applied[1] = Entry{1, "REGISTER a " + key(1)};
    auto op = [&](bool w, const std::string &k, std::uint64_t inv, std::uint64_t resp, const std::string &result) {
        Op x;
        x.write = w; x.name = "a"; x.key = k; x.inv = inv; x.resp = resp; x.result = result;
        sim.ops.push_back(x);
    };
    op(true, key(1), 1, 5, "OK");
    op(false, "", 6, 8, "MISSING");                                   // after the OK: stale
    check(!sim.check_linearizable().empty(), "a MISSING sent after an OK was received is rejected");
    sim.ops.pop_back();
    op(false, "", 3, 8, "MISSING");                                   // overlapping the write: legal
    check(sim.check_linearizable().empty(), "a MISSING overlapping the write is accepted");
    op(false, "", 2, 4, "FILE " + key(1));                            // FILE before the OK came back: legal
    check(sim.check_linearizable().empty(), "a FILE overlapping the write is accepted");
    op(false, "", 0, 0, "");
    sim.ops.back().inv = 7; sim.ops.back().resp = 9; sim.ops.back().result = "FILE " + key(2);
    check(!sim.check_linearizable().empty(), "a FILE naming a key that did not win is rejected");
    sim.ops.pop_back();
    op(true, key(2), 10, 12, "OK");
    check(!sim.check_linearizable().empty(), "a second key told OK is rejected");
    sim.ops.pop_back();
    op(true, key(2), 10, 12, "EXISTS " + key(1));
    check(sim.check_linearizable().empty(), "a second key told EXISTS with the winner is accepted");
    sim.ops.clear();
    op(false, "", 1, 3, "FILE " + key(1));
    op(true, key(1), 4, 6, "OK");
    check(!sim.check_linearizable().empty(), "a FILE answered before any write of that key was sent is rejected");
}

// ===========================================================================
// The log file (raft_log.cpp)
// ===========================================================================

std::vector<std::string> g_tmpdirs;

std::string make_tmpdir() {
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/test-raft-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    g_tmpdirs.emplace_back(buf.data());
    return g_tmpdirs.back();
}

std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void append_bytes(const std::string &path, const std::string &bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f << bytes;
}

void test_log_file() {
    std::printf("-- the log file\n");
    const std::string dir = make_tmpdir();
    const std::string path = dir + "/raft_7000.log";
    std::string report, err;
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err) && d.term == 0 && d.log.empty(), "a missing file is an empty state");
        std::vector<Entry> log = {Entry{1, "NOOP"}, Entry{1, "REGISTER a " + key(1)}};
        check(fs.save(1, 0, 1, log), "a save: term 1, a vote, two entries");
        log.resize(1);
        log.push_back(Entry{2, "NOOP"});
        check(fs.save(2, NOBODY, 2, log), "a save that cuts index 2 and adds a term-2 entry");
        check(fs.save(2, 1, 3, log), "a save of a vote alone");
        check(fs.batches() == 3, "three saves, three flushes");
    }
    const std::string text = slurp(path);
    check(text.find("CUT 2 ") != std::string::npos && text.find("TERM 2 1 ") != std::string::npos, "the file holds a CUT and the later vote");
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err), "reopened: " + report);
        check(d.term == 2 && d.voted_for == 1 && d.log.size() == 2 && d.log[1].term == 2 && d.log[0].command == "NOOP",
              "term 2, the vote for 1, and the log [NOOP t1, NOOP t2]: the cut entry is gone");
    }
    // A crash's tail: a torn last record, and one that fails its check.
    const std::string good = slurp(path);
    append_bytes(path, log_record("ENTRY 3 2 REGISTER b " + key(2)).substr(0, 30));   // torn: no newline
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err) && d.log.size() == 2, "a torn final record is cut off");
        check(report.find("torn") != std::string::npos, "and reported: " + report);
        check(slurp(path) == good, "the file is back to its last good record");
    }
    std::string altered = log_record("ENTRY 3 2 REGISTER b " + key(2));
    altered[20] = altered[20] == 'b' ? 'c' : 'b';
    append_bytes(path, altered + "\n" + log_record("ENTRY 4 2 NOOP") + "\n");
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err) && d.log.size() == 2, "a record failing its check, with an intact one after it, is cut with it");
        check(report.find("1 intact-looking") != std::string::npos, "and the intact record after the damage is counted: " + report);
    }
    append_bytes(path, log_record("ENTRY 9 2 NOOP") + "\n");
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err) && d.log.size() == 2 && report.find("out of sequence") != std::string::npos,
              "an entry out of sequence is damage too");
    }
    append_bytes(path, log_record("TERM 2 0") + "\n");
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err) && d.voted_for == 1, "a second vote in one term is refused as damage");
    }
    // Damage further back than any batch: refuse to start.
    {
        FileStorage fs;
        Durable d;
        check(fs.open(path, d, report, err), "reopen before the large write");
        std::vector<Entry> log = d.log;
        for (int i = 0; i < 5000; ++i) {
            log.push_back(Entry{2, "REGISTER n" + std::to_string(i) + " " + key(i)});
            fs.save(2, 1, log.size(), log);
        }
    }
    std::string big = slurp(path);
    const std::size_t at = big.find("ENTRY 5 ");
    big[at + 10] = big[at + 10] == '2' ? '3' : '2';
    { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << big; }
    {
        FileStorage fs;
        Durable d;
        err.clear();
        check(!fs.open(path, d, report, err), "damage far from the end (" + std::to_string(big.size() - at) + " bytes) refuses to start");
        check(err.find("refusing to start") != std::string::npos, "and says why: " + err.substr(0, 90));
    }
    // A write that fails: the file-size limit makes write() fail with EFBIG.
    {
        const std::string p2 = dir + "/raft_7001.log";
        FileStorage fs;
        Durable d;
        fs.open(p2, d, report, err);
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit old{};
        getrlimit(RLIMIT_FSIZE, &old);
        rlimit tiny = old;
        tiny.rlim_cur = 64;
        setrlimit(RLIMIT_FSIZE, &tiny);
        std::vector<Entry> log = {Entry{1, "REGISTER " + std::string(120, 'z') + " " + key(1)}};
        const bool saved = fs.save(1, 0, 1, log);
        setrlimit(RLIMIT_FSIZE, &old);
        check(!saved, "a save the disk refuses returns false (the core then stops)");
    }
}

// ===========================================================================
// The wire (raft_wire.cpp)
// ===========================================================================

void test_wire() {
    std::printf("-- the wire\n");
    const std::string g = group_fingerprint({"127.0.0.1:7000", "127.0.0.1:7001", "127.0.0.1:7002"});
    check(g.size() == 8 && g != group_fingerprint({"127.0.0.1:7001", "127.0.0.1:7000", "127.0.0.1:7002"}),
          "the fingerprint is 8 characters and depends on the order");
    VoteRequest v{12, 2, 40, 11};
    VoteRequest v2;
    std::string g2;
    check(parse_vote(split_single_spaces(encode_vote(g, v)), g2, v2) && g2 == g && v2.term == 12 && v2.candidate == 2 &&
              v2.last_index == 40 && v2.last_term == 11,
          "VOTE round-trips");
    AppendRequest a;
    a.term = 5; a.leader = 1; a.prev_index = 7; a.prev_term = 4; a.commit = 6; a.seq = 99;
    a.entries = {Entry{4, "REGISTER a " + key(1)}, Entry{5, "NOOP"}};
    std::string line, body;
    encode_append(g, a, line, body);
    std::size_t len = 0;
    check(body_length(line, len) == Body::Present && len == body.size(), "APPEND's last word is its body length, as net.cpp reads it");
    AppendRequest a2;
    check(parse_append(split_single_spaces(line), body, valid_command, g2, a2) && a2.entries.size() == 2 &&
              a2.entries[0].command == a.entries[0].command && a2.seq == 99 && a2.prev_term == 4,
          "APPEND round-trips with its entries");
    const auto bad = [&](const std::string &l, const std::string &b) {
        AppendRequest x;
        std::string gg;
        return !parse_append(split_single_spaces(l), b, valid_command, gg, x);
    };
    check(bad(line, body.substr(0, body.size() - 1)), "a body shorter than declared is refused");
    std::string l3 = line;
    l3.replace(l3.rfind(' ') - 1, 1, "3");                         // count 3, two lines
    check(bad(l3, body), "a body with fewer entries than its count is refused");
    check(bad(line, "4 REGISTER a " + key(1) + "\n5 BOGUS\n"), "an entry with an unknown command is refused");
    AppendRequest future = a;
    future.entries = {Entry{9, "NOOP"}};
    encode_append(g, future, line, body);
    check(bad(line, body), "an entry newer than the leader's term is refused");
    AppendRequest falling = a;
    falling.entries = {Entry{5, "NOOP"}, Entry{4, "NOOP"}};
    encode_append(g, falling, line, body);
    check(bad(line, body), "entries whose terms fall are refused");
    check(bad("APPEND " + g + " 05 1 7 4 6 99 0 0", ""), "a number with a leading zero is refused");
    check(bad("APPEND " + g + " 5 1 7 4 6 99 0 0 extra", ""), "a wrong word count is refused");
    VoteReply vr;
    AppendReply ar;
    check(parse_vote_reply(encode_vote_reply(VoteReply{8, true}), vr) && vr.term == 8 && vr.granted, "VOTED round-trips");
    check(parse_append_reply(encode_append_reply(AppendReply{8, false, 3}), ar) && ar.term == 8 && !ar.success && ar.index == 3,
          "APPENDED round-trips");
    check(!parse_vote_reply("VOTED 8 2", vr) && !parse_append_reply("APPENDED 8 1", ar) && !parse_vote_reply("ERR group mismatch", vr),
          "malformed replies are refused");
    std::uint64_t n = 0;
    check(!parse_u64("1234567890123456789", n) && parse_u64("123456789012345678", n), "numbers are at most 18 digits");
}

// ===========================================================================
// Finding the leader (group.cpp)
// ===========================================================================

void test_group_client() {
    std::printf("-- the client finding the leader\n");
    std::vector<Address> list;
    std::string err;
    check(parse_address_list("127.0.0.1:7000,127.0.0.1:7001,127.0.0.1:7002", list, err) && list.size() == 3,
          "a list of three addresses");
    check(!parse_address_list("127.0.0.1:7000,127.0.0.1:7000", list, err), "a repeated address is refused");
    check(!parse_address_list("127.0.0.1:7000,", list, err) && !parse_address_list("", list, err) &&
              !parse_address_list("localhost:7000", list, err),
          "an empty item, an empty list and a host name are refused");

    // Scripted replicas: what each answers, and a clock that sleeping advances.
    std::map<int, std::function<std::string()>> script;
    std::map<int, int> asked;
    Millis clock = 0;
    GroupClient g;
    parse_address_list("127.0.0.1:7000,127.0.0.1:7001,127.0.0.1:7002", g.replicas, err);
    g.wait_ms = 1000;
    g.now = [&] { return clock; };
    g.sleep = [&](int ms) { clock += ms; };
    g.exchange = [&](const Address &to, const std::string &, std::string &reply) {
        ++asked[to.port];
        clock += 1;
        if (!script.count(to.port)) return Delivery::Refused;
        reply = script[to.port]();
        return Delivery::Ok;
    };
    script[7000] = [] { return std::string("REDIRECT 127.0.0.1:7002"); };
    script[7001] = [] { return std::string("UNAVAILABLE no leader"); };
    script[7002] = [] { return std::string("OK"); };
    std::string reply;
    check(ask_group(g, "REGISTER a " + key(1), reply) && reply == "OK" && asked[7002] == 1 && asked[7001] == 0,
          "a redirect is followed at once, straight to the leader");
    check(g.preferred == 2, "and the next request starts at the leader");
    asked.clear();
    check(ask_group(g, "RESOLVE a", reply) && asked[7002] == 1 && asked[7000] == 0, "which it does");

    // An election: nobody answers for a while, then replica 1 leads.
    int calls = 0;
    script[7000] = [] { return std::string("UNAVAILABLE no leader is known"); };
    script[7001] = [&] { return ++calls > 6 ? std::string("FILE ") + key(1) : std::string("UNAVAILABLE no leader is known"); };
    script.erase(7002);                                      // the old leader is dead
    g.preferred = 2;
    clock = 0;
    check(ask_group(g, "RESOLVE a", reply) && reply == "FILE " + key(1), "through an election, the client keeps asking until a leader answers");
    check(clock >= 200 && clock < 1000, "pausing between rounds (" + std::to_string(clock) + " ms)");

    // Two replicas naming each other: bounded, then a failure that says what happened.
    script[7000] = [] { return std::string("REDIRECT 127.0.0.1:7001"); };
    script[7001] = [] { return std::string("REDIRECT 127.0.0.1:7000"); };
    clock = 0;
    asked.clear();
    check(!ask_group(g, "RESOLVE a", reply), "two replicas redirecting to each other never answer");
    check(clock >= 1000 && asked[7000] + asked[7001] < 200, "the client gives up at its deadline, and the redirects were bounded");
    check(reply.find("7002 refused") != std::string::npos && reply.find("redirected") != std::string::npos,
          "the failure says what each replica did: " + reply.substr(0, 80));
    // A reply that is an answer, even an error, is returned as is.
    script[7000] = [] { return std::string("ERR a name is 1 to 200 visible ASCII characters"); };
    g.preferred = 0;
    check(ask_group(g, "RESOLVE " + std::string(300, 'x'), reply) && reply.compare(0, 4, "ERR ") == 0, "an ERR is an answer, not a retry");
}

}  // namespace

int main() {
    std::printf("test-raft: the replicated tracker's core, log, wire and client\n");
    test_single_replica();
    test_votes();
    test_append_rules();
    test_figure_8_exact();
    test_commit_rule_and_reads();
    test_storage_failure();
    test_linearizability_checker();
    test_crash_at_every_step();
    test_random_histories();
    test_log_file();
    test_wire();
    test_group_client();
    for (const std::string &d : g_tmpdirs) {
        const std::string cmd = "rm -rf '" + d + "'";
        if (std::system(cmd.c_str()) != 0) std::printf("  (could not remove %s)\n", d.c_str());
    }
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
