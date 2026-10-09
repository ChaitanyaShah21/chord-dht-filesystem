#include "replica.h"

#include "lookup.h"     // split_single_spaces
#include "raft_wire.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace {

Millis steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *const UNKNOWN_OUTCOME = "UNAVAILABLE the request was not committed in time; its outcome is unknown, and retrying is safe";

}  // namespace

Replica::Replica(const ReplicaConfig &config, Storage &storage, Durable recovered, CallFn call)
    : cfg_(config), call_(std::move(call)) {
    std::vector<std::string> members;
    for (const Address &a : cfg_.group) members.push_back(a.text());
    fingerprint_ = group_fingerprint(members);
    slots_.resize(cfg_.group.size());
    mismatch_logged_.assign(cfg_.group.size(), false);
    on_fatal = [](const std::string &why) {
        std::fprintf(stderr, "[tracker] FATAL: %s; stopping so that nothing is answered from state that may be lost\n",
                     why.c_str());
        _exit(1);
    };

    RaftConfig rc;
    rc.id = cfg_.id;
    rc.size = static_cast<int>(cfg_.group.size());
    rc.election = cfg_.election;
    rc.seed = cfg_.seed != 0 ? cfg_.seed : static_cast<std::uint64_t>(steady_ms()) * 1000003u + static_cast<std::uint64_t>(cfg_.id);
    std::unique_lock<std::mutex> lock(mu_);
    raft_ = std::make_unique<Raft>(rc, storage, std::move(recovered),
                                   [this](Index i, const Entry &e) { on_apply(i, e); }, now_ms());
    dispatch();
}

Replica::~Replica() { stop(); }

Millis Replica::now_ms() const { return steady_ms(); }

void Replica::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!threads_.empty()) return;
    threads_.emplace_back(&Replica::ticker, this);
    for (int p = 0; p < static_cast<int>(cfg_.group.size()); ++p)
        if (p != cfg_.id) threads_.emplace_back(&Replica::sender, this, p);
}

void Replica::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    send_cv_.notify_all();
    done_cv_.notify_all();
    for (std::thread &t : threads_) t.join();
    threads_.clear();
}

// ---------------------------------------------------------------------------
// Under the lock: moving the core's outputs to where they go
// ---------------------------------------------------------------------------

// The core applies a committed entry. Apply it to the namespace, then wake the
// client whose request this index was -- if the entry is still that request's,
// which the term says (D-075).
void Replica::on_apply(Index index, const Entry &entry) {
    const std::string result = apply_command(ns_, entry.command);
    const auto range = writes_.equal_range(index);
    for (auto it = range.first; it != range.second; ++it) {
        WriteWaiter *w = it->second;
        w->done = true;
        w->result = w->term == entry.term
                        ? result
                        : "UNAVAILABLE a new leader replaced this request before it was committed; retrying is safe";
    }
    if (range.first != range.second) {
        writes_.erase(range.first, range.second);
        done_cv_.notify_all();
    }
}

// After every call into the core: calls go to their peer's slot, replacing any
// older one not yet sent (D-072); resolved reads wake their clients.
void Replica::dispatch() {
    bool any = false;
    for (Outgoing &o : raft_->take_outgoing()) {
        slots_[o.to].full = true;
        slots_[o.to].call = std::move(o);
        any = true;
    }
    if (any) send_cv_.notify_all();

    bool woke = false;
    for (const Raft::ReadResult &r : raft_->take_reads()) {
        const auto it = reads_.find(r.id);
        if (it == reads_.end()) continue;
        it->second->done = true;
        it->second->ok = r.ok;
        reads_.erase(it);
        woke = true;
    }
    if (woke) done_cv_.notify_all();

    if (raft_->stopped()) fatal_ = true;
    if (cfg_.log_transitions && (raft_->role() != shown_role_ || raft_->term() != shown_term_)) {
        shown_role_ = raft_->role();
        shown_term_ = raft_->term();
        std::fprintf(stderr, "[tracker %d] term %llu: %s\n", cfg_.id, static_cast<unsigned long long>(shown_term_),
                     role_name(shown_role_));
    }
}

void Replica::check_fatal(std::unique_lock<std::mutex> &lock) {
    if (!fatal_) return;
    lock.unlock();
    on_fatal("the replica's log could not be written");
    lock.lock();
}

std::string Replica::not_leader() const {
    const int l = raft_->leader();
    if (l != NOBODY && l != cfg_.id) return "REDIRECT " + cfg_.group[l].text();
    return "UNAVAILABLE no leader is known; an election may be in progress";
}

// STATUS <id> <role> <term> <leader|-> <commit> <applied> <last-index>: read-only,
// for the test and benchmark harnesses, as FINGERS is for the ring.
std::string Replica::status() const {
    const int l = raft_->leader();
    return "STATUS " + std::to_string(cfg_.id) + " " + role_name(raft_->role()) + " " + std::to_string(raft_->term()) +
           " " + (l == NOBODY ? std::string("-") : std::to_string(l)) + " " + std::to_string(raft_->commit_index()) + " " +
           std::to_string(raft_->last_applied()) + " " + std::to_string(raft_->last_index());
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

std::string Replica::handle(const std::string &line, const std::string &body) {
    if (line.empty()) return "ERR empty request";
    const std::vector<std::string> w = split_single_spaces(line);
    const int n = static_cast<int>(cfg_.group.size());
    std::unique_lock<std::mutex> lock(mu_);
    if (fatal_ || stopping_) return "UNAVAILABLE this replica is stopping";

    // ---- calls from other replicas ----
    if (w[0] == "VOTE" || w[0] == "APPEND") {
        std::string group;
        VoteRequest vq;
        AppendRequest aq;
        const bool vote = w[0] == "VOTE";
        if (vote ? !parse_vote(w, group, vq) : !parse_append(w, body, valid_command, group, aq))
            return "ERR malformed " + w[0];
        // A replica started with a different member list computes different
        // majorities: refuse it rather than count it (D-079).
        const int from = vote ? vq.candidate : aq.leader;
        if (group != fingerprint_ || from < 0 || from >= n || from == cfg_.id) {
            if (from >= 0 && from < n && !mismatch_logged_[from]) {
                mismatch_logged_[from] = true;
                std::fprintf(stderr, "[tracker %d] refusing calls from a replica with group %s (mine is %s)\n", cfg_.id,
                             group.c_str(), fingerprint_.c_str());
            }
            return "ERR group mismatch";
        }
        std::string reply = vote ? encode_vote_reply(raft_->handle_vote(vq, now_ms()))
                                 : encode_append_reply(raft_->handle_append(aq, now_ms()));
        dispatch();
        if (fatal_) {
            check_fatal(lock);
            return "UNAVAILABLE this replica is stopping";
        }
        return reply;
    }
    if (w[0] == "STATUS" && w.size() == 1) return status();
    return client_request(line, lock);
}

std::string Replica::client_request(const std::string &line, std::unique_lock<std::mutex> &lock) {
    const NamesRequest r = parse_names_request(line);
    if (r.kind == NamesRequest::Kind::Invalid) return r.error;
    if (r.kind == NamesRequest::Kind::Ping) return "PONG";
    if (raft_->role() != Raft::Role::Leader) return not_leader();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.commit_wait_ms);

    if (r.kind == NamesRequest::Kind::Register) {
        // The waiter is registered BEFORE proposing, at the index and term the entry
        // will get: in a group of one the entry is committed and applied inside
        // propose() itself, and the result must not be missed.
        WriteWaiter waiter;
        waiter.term = raft_->term();
        const Index expect = raft_->last_index() + 1;
        const auto it = writes_.emplace(expect, &waiter);
        Index index = 0;
        Term term = 0;
        if (!raft_->propose(register_command(r.name, r.key), index, term, now_ms())) {
            if (!waiter.done) writes_.erase(it);
            dispatch();
            check_fatal(lock);
            return not_leader();
        }
        dispatch();
        check_fatal(lock);
        done_cv_.wait_until(lock, deadline, [&] { return waiter.done || stopping_ || fatal_; });
        if (waiter.done) return waiter.result;
        writes_.erase(it);                            // still there: on_apply erases only what it completes
        return UNKNOWN_OUTCOME;
    }

    // RESOLVE and LIST: ReadIndex (D-076). Like a write, the waiter is in place
    // before the core can resolve it.
    ReadWaiter waiter;
    const std::uint64_t id = ++next_read_;
    reads_[id] = &waiter;
    if (!raft_->read(id, now_ms())) {
        reads_.erase(id);
        return not_leader();
    }
    dispatch();
    check_fatal(lock);
    done_cv_.wait_until(lock, deadline, [&] { return waiter.done || stopping_ || fatal_; });
    if (!waiter.done) {
        reads_.erase(id);
        return "UNAVAILABLE the read could not be confirmed by a majority in time; retrying is safe";
    }
    if (!waiter.ok) return not_leader();
    return answer_read(ns_, r);                       // the state now covers the read's index
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

void Replica::ticker() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stopping_) {
        raft_->tick(now_ms());
        dispatch();
        check_fatal(lock);
        done_cv_.wait_for(lock, std::chrono::milliseconds(TICK_MS), [&] { return stopping_; });
    }
}

// One peer's calls, one at a time. The lock is released for the network call
// and taken again to hand the reply to the core (D-030, D-072).
void Replica::sender(int peer) {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        send_cv_.wait(lock, [&] { return stopping_ || slots_[peer].full; });
        if (stopping_) return;
        const Outgoing sent = slots_[peer].call;
        slots_[peer].full = false;
        lock.unlock();

        std::string line, body, reply;
        if (sent.is_vote) line = encode_vote(fingerprint_, sent.vote);
        else encode_append(fingerprint_, sent.append, line, body);
        const Delivery d = call_(peer, line, body, reply, static_cast<int>(cfg_.election));

        lock.lock();
        if (stopping_) return;
        VoteReply vr;
        AppendReply ar;
        if (d == Delivery::Ok && sent.is_vote && parse_vote_reply(reply, vr))
            raft_->handle_vote_reply(peer, sent.vote, vr, now_ms());
        else if (d == Delivery::Ok && !sent.is_vote && parse_append_reply(reply, ar))
            raft_->handle_append_reply(peer, sent.append, ar, now_ms());
        else
            raft_->call_failed(peer, sent, now_ms());
        dispatch();
        check_fatal(lock);
    }
}
