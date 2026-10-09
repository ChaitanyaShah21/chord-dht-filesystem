#ifndef RAFT_H
#define RAFT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The Raft core (decisions D-069 to D-082; docs/consensus.md).
//
// A deterministic state machine, and nothing else (D-070): no sockets, no
// threads, no clock. Everything that happens to a replica is a method call --
// a tick carrying the time, a call received from another replica, a reply
// together with the call it answers, a client's proposal or read -- and
// everything the replica wants done comes back out: calls to send
// (take_outgoing), entries to apply (the apply function), reads that may now
// be answered (take_reads).
//
// Every input method ends the same way (finish() in raft.cpp): first whatever
// the step changed is made durable through Storage, then the commit index is
// advanced and committed entries applied, and only then can its outputs be
// taken. So nothing leaves a replica -- a vote, an append's reply, a call, a
// leader counting its own entry -- before the state behind it is on disk:
// the paper's "respond after updating stable storage", made structural (D-074).
//
// The names follow Figure 2 of the Raft paper (Ongaro and Ousterhout, 2014).
// Indexes start at 1; index 0 with term 0 is "before the first entry".
// ---------------------------------------------------------------------------

using Term   = std::uint64_t;
using Index  = std::uint64_t;
using Millis = std::int64_t;        // a point in time in milliseconds, on whatever clock the caller uses

constexpr int NOBODY = -1;          // no vote, no known leader

struct Entry {
    Term term = 0;
    std::string command;            // opaque to the core: "NOOP" or "REGISTER <name> <key>" (names.h)
};

// RequestVote (Figure 2).
struct VoteRequest {
    Term term = 0;
    int candidate = NOBODY;
    Index last_index = 0;           // the candidate's last entry, so a voter can refuse a log
    Term last_term = 0;             // less up to date than its own
};
struct VoteReply {
    Term term = 0;
    bool granted = false;
};

// AppendEntries (Figure 2), which is also the heartbeat.
struct AppendRequest {
    Term term = 0;
    int leader = NOBODY;
    Index prev_index = 0;           // the entry just before `entries`, which the follower must hold
    Term prev_term = 0;
    std::vector<Entry> entries;     // empty for a pure heartbeat
    Index commit = 0;               // the leader's commit index
    std::uint64_t seq = 0;          // numbers the leader's calls; a read counts only replies to calls
};                                  //   sent after it arrived (ReadIndex, D-076)
struct AppendReply {
    Term term = 0;
    bool success = false;
    Index index = 0;                // success: the last index the follower now matches through;
};                                  //   failure: the index the leader should try from next

// One call the core wants made. Exactly one of the two requests is meaningful.
struct Outgoing {
    int to = NOBODY;
    bool is_vote = false;
    VoteRequest vote;
    AppendRequest append;
};

// What a replica must keep through a crash: Figure 2's persistent state.
struct Durable {
    Term term = 0;
    int voted_for = NOBODY;
    std::vector<Entry> log;         // log[0] is index 1
};

// Where the durable state goes (D-074). save() is called with the whole current
// state and the first log index that changed since the last save (log.size() + 1
// if no entry changed); entries from `from` on replace whatever was stored there.
// It returns only once everything is durable. False: it may not be, and the
// replica stops (fail-stop) rather than answer from state it may forget.
class Storage {
public:
    virtual ~Storage() = default;
    virtual bool save(Term term, int voted_for, Index from, const std::vector<Entry> &log) = 0;
};

// Storage in memory, for tests: `durable` is what a crash leaves behind.
class MemoryStorage : public Storage {
public:
    Durable durable;
    int saves = 0;                  // how many times save() was called
    std::atomic<bool> fail{false};  // make every save fail, as a full disk would

    bool save(Term term, int voted_for, Index from, const std::vector<Entry> &log) override {
        ++saves;
        if (fail) return false;
        durable.term = term;
        durable.voted_for = voted_for;
        durable.log.resize(from - 1 < durable.log.size() ? from - 1 : durable.log.size());
        for (Index i = from; i <= log.size(); ++i) durable.log.push_back(log[i - 1]);
        return true;
    }
};

struct RaftConfig {
    int id = 0;                     // this replica's position in the group, 0 .. size-1
    int size = 1;                   // replicas in the group (D-069)
    Millis election = 150;          // E (D-073; chosen by its rule, BENCHMARKS.md §7): timeouts in [E, 2E), heartbeat E/5, call deadline E,
                                    //   a leader steps down after 2E without a majority (D-077)
    std::size_t max_append_bytes = 64 * 1024;   // commands per APPEND, roughly (D-071)
    std::uint64_t seed = 1;         // the election-timeout generator's seed
};

class Raft {
public:
    enum class Role { Follower, Candidate, Leader };
    using ApplyFn = std::function<void(Index, const Entry &)>;

    // `recovered` is what Storage held at start-up. A group of one becomes leader
    // here, at once: no other replica can lead (D-080).
    Raft(const RaftConfig &config, Storage &storage, Durable recovered, ApplyFn apply, Millis now);

    // ---- inputs ----
    void tick(Millis now);                                        // timers: elections, heartbeats, check-quorum
    VoteReply handle_vote(const VoteRequest &req, Millis now);    // a call from a candidate
    AppendReply handle_append(const AppendRequest &req, Millis now);   // a call from a leader
    void handle_vote_reply(int from, const VoteRequest &sent, const VoteReply &reply, Millis now);
    void handle_append_reply(int from, const AppendRequest &sent, const AppendReply &reply, Millis now);
    void call_failed(int to, const Outgoing &sent, Millis now);   // no reply: refused, timed out or closed

    // A client's command: appended at `index` in `term` if this replica is the
    // leader. Whether it takes effect is known only when `index` is applied: if the
    // entry applied there has `term`, it is this one (D-075).
    bool propose(const std::string &command, Index &index, Term &term, Millis now);

    // A linearizable read (ReadIndex, D-076): false at once if not the leader;
    // otherwise its result arrives through take_reads(), ok once it may be answered
    // from the applied state, not ok if leadership was lost first.
    bool read(std::uint64_t id, Millis now);

    // ---- outputs ----
    std::vector<Outgoing> take_outgoing();
    struct ReadResult {
        std::uint64_t id = 0;
        bool ok = false;
    };
    std::vector<ReadResult> take_reads();

    // ---- inspection, for the server and the tests ----
    Role role() const { return role_; }
    Term term() const { return term_; }
    int leader() const { return leader_; }
    int voted_for() const { return voted_for_; }
    Index commit_index() const { return commit_; }
    Index last_applied() const { return applied_; }
    Index last_index() const { return log_.size(); }
    Term term_at(Index i) const { return i == 0 || i > log_.size() ? 0 : log_[i - 1].term; }
    const std::vector<Entry> &log() const { return log_; }
    bool stopped() const { return stopped_; }
    const RaftConfig &config() const { return cfg_; }

private:
    struct PendingRead {
        std::uint64_t id;
        std::uint64_t seq;              // replies to calls numbered from here on count
        Index index;                    // the commit index to wait for; 0 until known
        std::vector<bool> acks;         // who has acknowledged this leader since the read arrived
    };

    // Transitions.
    void become_follower(Term term, int leader, Millis now);
    void start_election(Millis now);
    void become_leader(Millis now);

    // The leader's side of replication.
    void send_append(int peer, Millis now);
    void send_if_due(int peer, Millis now, bool force);
    void advance_commit();

    // The end of every input: persist, then commit and apply, then resolve reads.
    void finish();
    void apply_committed();
    void resolve_reads();
    void fail_reads();

    void reset_election_timer(Millis now);
    void changed_from(Index i);         // the log changed from index i on
    void changed_meta() { dirty_ = true; }
    bool majority(int count) const { return 2 * count > cfg_.size; }
    bool log_at_least_as_up_to_date(Term last_term, Index last_index) const;

    RaftConfig cfg_;
    Storage &storage_;
    ApplyFn apply_;

    // Persistent state (Figure 2), kept here and written through storage_.
    Term term_ = 0;
    int voted_for_ = NOBODY;
    std::vector<Entry> log_;
    bool dirty_ = false;                // something above changed since the last save
    Index dirty_from_ = 0;              // the first log index that changed; 0 if none

    // Volatile state.
    Role role_ = Role::Follower;
    int leader_ = NOBODY;
    Index commit_ = 0;
    Index applied_ = 0;
    Millis election_deadline_ = 0;
    std::mt19937_64 rng_;
    bool stopped_ = false;

    // A candidate's votes.
    std::vector<bool> votes_;

    // A leader's view of each peer (indexed by replica id; its own slot unused).
    std::vector<Index> next_, match_;
    std::vector<bool> inflight_;        // one APPEND at a time per follower (D-071)
    std::vector<std::uint64_t> inflight_seq_;
    std::vector<Millis> inflight_until_;    // presumed lost after this
    std::vector<Millis> heartbeat_due_;
    std::vector<Millis> retry_after_;   // after a failed call, wait a heartbeat before the next
    std::vector<Millis> heard_;         // the last reply in this term, for check-quorum (D-077)
    std::uint64_t seq_ = 0;

    std::vector<PendingRead> reads_;
    std::vector<ReadResult> read_results_;
    std::vector<Outgoing> out_;
};

const char *role_name(Raft::Role role);

#endif
