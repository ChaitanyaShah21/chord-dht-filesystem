#ifndef REPLICA_H
#define REPLICA_H

#include "group.h"
#include "metrics.h"
#include "names.h"
#include "net.h"
#include "raft.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// One tracker replica: the Raft core (raft.h) and the namespace (names.h) behind
// one lock, with the threads that drive them (decision D-072).
//
//   - Request threads (one per connection, in tracker.cpp) call handle() with each
//     request line. A client's REGISTER, RESOLVE or LIST waits on a condition
//     variable until its entry is applied (D-075) or its read confirmed (D-076).
//   - A ticker calls the core every TICK_MS: elections, heartbeats, check-quorum.
//   - One sender per peer takes the newest call pending for that peer, makes it
//     WITHOUT the lock, and feeds the reply back under it.
//
// The lock is never held across a network call. It is held across the flush,
// which happens inside the core's methods (D-072, D-074).
//
// The network is passed in as `call`, so a test can run a whole group in one
// process with real threads and a transport it can cut.
// ---------------------------------------------------------------------------

// One call to replica `to`: send `line` and `body`, read one reply line. Every step
// bounded by `timeout_ms`.
using CallFn = std::function<Delivery(int to, const std::string &line, const std::string &body, std::string &reply,
                                      int timeout_ms)>;

struct ReplicaConfig {
    std::vector<Address> group;          // every replica, in the same order everywhere (D-079)
    int id = 0;                          // this replica's position in `group`
    Millis election = 150;               // E (D-073; chosen by its rule, BENCHMARKS.md §7)
    int commit_wait_ms = 2000;           // how long a client request waits before UNAVAILABLE (D-075)
    std::uint64_t seed = 0;              // the election timer's seed; 0: from the clock and the id
    bool log_transitions = true;         // a line on standard error at every change of role or term
};

class Replica {
public:
    static constexpr int TICK_MS = 5;

    // `recovered` is what `storage` replayed. The core is built here, and a group
    // of one is leader before this returns.
    Replica(const ReplicaConfig &config, Storage &storage, Durable recovered, CallFn call);
    ~Replica();
    Replica(const Replica &) = delete;
    Replica &operator=(const Replica &) = delete;

    void start();                        // the ticker and the senders
    void stop();                         // joins them; waiting requests answer UNAVAILABLE

    // Answer one request line (with its body, for APPEND). Never throws on what a
    // peer sends (I5).
    std::string handle(const std::string &line, const std::string &body);

    // Called (with no lock held) if storage fails: the replica must not go on
    // (fail-stop). By default it prints why and ends the process.
    std::function<void(const std::string &)> on_fatal;

    std::string fingerprint() const { return fingerprint_; }

    // What the replica is now, copied under the lock, for the metrics (D-084).
    struct View {
        Raft::Role role = Raft::Role::Follower;
        Term term = 0;
        int leader = NOBODY;
        Index commit = 0, applied = 0, last = 0;
        std::size_t names = 0;
    };
    View view() const;

    // Events counted for the metrics. Incremented under the lock, read without it:
    // each Counter is an atomic.
    Counter elections;                   // elections this replica started
    Counter leader_changes;              // times it learned of a different leader
    std::unique_ptr<Counter[]> calls_failed;   // calls to each peer that got no usable reply

private:
    struct WriteWaiter {
        Term term = 0;
        bool done = false;
        std::string result;
    };
    struct ReadWaiter {
        bool done = false;
        bool ok = false;
    };
    struct Slot {                        // the newest call pending for one peer
        bool full = false;
        Outgoing call;
    };

    Millis now_ms() const;
    void dispatch();                     // after every core call, under the lock
    void on_apply(Index index, const Entry &entry);
    std::string not_leader() const;
    std::string status() const;
    std::string client_request(const std::string &line, std::unique_lock<std::mutex> &lock);
    void ticker();
    void sender(int peer);
    void check_fatal(std::unique_lock<std::mutex> &lock);

    ReplicaConfig cfg_;
    CallFn call_;
    std::string fingerprint_;

    mutable std::mutex mu_;              // guards everything below (D-072)
    std::condition_variable done_cv_;    // a write applied, a read resolved, or stopping
    std::condition_variable send_cv_;    // a slot filled, or stopping
    Namespace ns_;
    std::multimap<Index, WriteWaiter *> writes_;
    std::map<std::uint64_t, ReadWaiter *> reads_;
    std::uint64_t next_read_ = 0;
    std::vector<Slot> slots_;
    std::unique_ptr<Raft> raft_;         // built last: it applies entries into ns_ as it is built
    bool stopping_ = false;
    bool fatal_ = false;
    Raft::Role shown_role_ = Raft::Role::Follower;
    Term shown_term_ = 0;
    int known_leader_ = NOBODY;
    std::vector<bool> mismatch_logged_;

    std::vector<std::thread> threads_;
};

#endif
