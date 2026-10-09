#include "raft.h"

#include <algorithm>

const char *role_name(Raft::Role role) {
    switch (role) {
        case Raft::Role::Follower:  return "follower";
        case Raft::Role::Candidate: return "candidate";
        case Raft::Role::Leader:    return "leader";
    }
    return "follower";
}

Raft::Raft(const RaftConfig &config, Storage &storage, Durable recovered, ApplyFn apply, Millis now)
    : cfg_(config), storage_(storage), apply_(std::move(apply)), rng_(config.seed) {
    term_      = recovered.term;
    voted_for_ = recovered.voted_for;
    log_       = std::move(recovered.log);

    const std::size_t n = static_cast<std::size_t>(cfg_.size);
    next_.assign(n, 1);
    match_.assign(n, 0);
    inflight_.assign(n, false);
    inflight_seq_.assign(n, 0);
    inflight_until_.assign(n, 0);
    heartbeat_due_.assign(n, 0);
    retry_after_.assign(n, 0);
    heard_.assign(n, 0);
    votes_.assign(n, false);

    // Every replica starts as a follower and waits out one election timeout, so a
    // replica that restarts does not depose a working leader the moment it is back.
    // A group of one has nobody to wait for (D-080).
    reset_election_timer(now);
    if (cfg_.size == 1) start_election(now);
    finish();
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

// A fresh timeout, uniform in [E, 2E), drawn at every reset (D-073): if every
// follower used the same timeout they would all stand at once after a leader died,
// split the vote, and time out together again.
void Raft::reset_election_timer(Millis now) {
    std::uniform_int_distribution<Millis> spread(0, cfg_.election - 1);
    election_deadline_ = now + cfg_.election + spread(rng_);
}

void Raft::tick(Millis now) {
    if (stopped_) return;
    if (role_ != Role::Leader) {
        if (now >= election_deadline_) start_election(now);
        finish();
        return;
    }

    // Check-quorum (D-077): a leader that has heard nothing in its term from a
    // majority for 2E is cut off, and stops claiming to lead. 2E is the longest
    // timeout a follower can draw, so by now the majority may have a new leader.
    int recent = 1;                                   // itself
    for (int p = 0; p < cfg_.size; ++p)
        if (p != cfg_.id && now - heard_[p] < 2 * cfg_.election) ++recent;
    if (!majority(recent)) {
        become_follower(term_, NOBODY, now);
        finish();
        return;
    }

    for (int p = 0; p < cfg_.size; ++p) {
        if (p == cfg_.id) continue;
        // A call unanswered for a whole deadline is presumed lost; APPEND is
        // idempotent, so sending again is safe even if it was only slow.
        if (inflight_[p] && now >= inflight_until_[p]) inflight_[p] = false;
        send_if_due(p, now, false);
    }
    finish();
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

// Adopt `term` if it is newer (forgetting any vote cast in an older term), and
// follow. The election timer restarts only when the role actually changes: a
// follower that merely learns of a higher term -- from a candidate it will not
// vote for, say -- keeps its deadline, as Figure 2 has it.
void Raft::become_follower(Term term, int leader, Millis now) {
    if (term > term_) {
        term_ = term;
        voted_for_ = NOBODY;
        changed_meta();
    }
    if (role_ != Role::Follower) {
        role_ = Role::Follower;
        fail_reads();                                 // a former leader can no longer answer them
        reset_election_timer(now);
    }
    leader_ = leader;
}

void Raft::start_election(Millis now) {
    ++term_;
    voted_for_ = cfg_.id;                             // vote for itself, durably, before asking anyone
    changed_meta();
    role_ = Role::Candidate;
    leader_ = NOBODY;
    fail_reads();
    reset_election_timer(now);
    std::fill(votes_.begin(), votes_.end(), false);
    votes_[cfg_.id] = true;
    if (majority(1)) {                                // a group of one
        become_leader(now);
        return;
    }
    for (int p = 0; p < cfg_.size; ++p) {
        if (p == cfg_.id) continue;
        Outgoing o;
        o.to = p;
        o.is_vote = true;
        o.vote = VoteRequest{term_, cfg_.id, last_index(), term_at(last_index())};
        out_.push_back(o);
    }
}

void Raft::become_leader(Millis now) {
    role_ = Role::Leader;
    leader_ = cfg_.id;
    for (int p = 0; p < cfg_.size; ++p) {
        next_[p] = last_index() + 1;
        match_[p] = 0;
        inflight_[p] = false;
        heartbeat_due_[p] = now;
        retry_after_[p] = now;
        heard_[p] = now;                              // a full 2E's grace before check-quorum
    }
    // A no-op in the new term. Entries of earlier terms are never committed by
    // counting copies (Figure 8); they commit when an entry of this term does. It
    // also gives ReadIndex a commit in this term to start from (D-076).
    log_.push_back(Entry{term_, "NOOP"});
    changed_from(last_index());
    for (int p = 0; p < cfg_.size; ++p)
        if (p != cfg_.id) send_append(p, now);
}

// ---------------------------------------------------------------------------
// Votes
// ---------------------------------------------------------------------------

// Section 5.4.1: a candidate's log is at least as up to date as mine if its last
// entry has a later term, or the same term and an index at least as high.
bool Raft::log_at_least_as_up_to_date(Term last_term, Index last_index_) const {
    const Term mine = term_at(last_index());
    return last_term > mine || (last_term == mine && last_index_ >= last_index());
}

VoteReply Raft::handle_vote(const VoteRequest &req, Millis now) {
    if (stopped_) return VoteReply{term_, false};
    if (req.term > term_) become_follower(req.term, NOBODY, now);

    // One vote per term, and only for a log at least as up to date as mine: that is
    // what makes every leader hold every committed entry (Leader Completeness).
    bool granted = false;
    if (req.term == term_ && (voted_for_ == NOBODY || voted_for_ == req.candidate) &&
        log_at_least_as_up_to_date(req.last_term, req.last_index)) {
        voted_for_ = req.candidate;
        changed_meta();
        granted = true;
        reset_election_timer(now);                    // a vote granted is a reason not to stand
    }
    finish();                                         // the vote is on disk before it is sent
    return VoteReply{term_, granted && !stopped_};
}

void Raft::handle_vote_reply(int from, const VoteRequest &sent, const VoteReply &reply, Millis now) {
    if (stopped_) return;
    if (reply.term > term_) {
        become_follower(reply.term, NOBODY, now);
    } else if (role_ == Role::Candidate && sent.term == term_ && reply.term == term_ && reply.granted) {
        votes_[from] = true;
        const int count = static_cast<int>(std::count(votes_.begin(), votes_.end(), true));
        if (majority(count)) become_leader(now);
    }
    finish();
}

// ---------------------------------------------------------------------------
// Appending
// ---------------------------------------------------------------------------

AppendReply Raft::handle_append(const AppendRequest &req, Millis now) {
    if (stopped_) return AppendReply{term_, false, 0};
    if (req.term < term_) {                           // a deposed leader: my term tells it so
        finish();
        return AppendReply{term_, false, 0};
    }
    // A current leader. A candidate of the same term steps down; a follower restarts
    // its election timer, because the leader is alive.
    become_follower(req.term, req.leader, now);
    reset_election_timer(now);

    AppendReply reply{term_, false, 0};
    if (req.prev_index > last_index()) {
        // My log is too short to hold the entry before these: send from my end.
        reply.index = last_index() + 1;
    } else if (term_at(req.prev_index) != req.prev_term) {
        // I hold a different entry there. Suggest the start of that entry's term, so
        // the leader skips the whole term in one step rather than one entry per call.
        const Term conflict = term_at(req.prev_index);
        Index i = req.prev_index;
        while (i > 1 && term_at(i - 1) == conflict) --i;
        reply.index = i;
    } else {
        // The logs agree up to prev_index (Log Matching). Add what is new. An entry I
        // already hold with the same term is the same entry, and is kept: a duplicate
        // or reordered call must never cut entries a later call delivered.
        Index i = req.prev_index;
        for (const Entry &e : req.entries) {
            ++i;
            if (i <= last_index()) {
                if (term_at(i) == e.term) continue;
                // A conflict: everything from i on was never committed, or the leader
                // would hold it too (Leader Completeness). Cut it. A committed entry
                // never reaches this line; if it did, the replica stops rather than
                // apply a different command at an index it has already applied.
                if (i <= commit_) {
                    stopped_ = true;
                    return AppendReply{term_, false, 0};
                }
                log_.resize(i - 1);
                changed_from(i);
            }
            log_.push_back(e);
            changed_from(i);
        }
        const Index last_new = req.prev_index + req.entries.size();
        // Commit no further than this call proved my log matches the leader's: an
        // entry after last_new may be a stale one this leader would replace.
        if (req.commit > commit_) commit_ = std::min(req.commit, last_new);
        reply.success = true;
        reply.index = last_new;
    }
    finish();                                         // entries on disk before "success" leaves
    if (stopped_) return AppendReply{term_, false, 0};
    return reply;
}

void Raft::handle_append_reply(int from, const AppendRequest &sent, const AppendReply &reply, Millis now) {
    if (stopped_) return;
    if (reply.term > term_) {
        become_follower(reply.term, NOBODY, now);
        finish();
        return;
    }
    // Only a reply to a call of this leadership counts; anything else is history.
    if (role_ != Role::Leader || sent.term != term_ || reply.term != term_) {
        finish();
        return;
    }
    heard_[from] = now;
    if (inflight_[from] && sent.seq == inflight_seq_[from]) inflight_[from] = false;

    if (reply.success) {
        const Index matched = sent.prev_index + sent.entries.size();
        if (matched > match_[from]) match_[from] = matched;
        if (next_[from] < matched + 1) next_[from] = matched + 1;
    } else {
        // Back up, never below what this follower is known to hold, and never past the
        // call's own prev_index (a stale failure reply must not undo later progress).
        Index hint = std::max<Index>(reply.index, 1);
        hint = std::min(hint, sent.prev_index);
        const Index back = std::max(match_[from] + 1, hint);
        if (back < next_[from]) next_[from] = back;
        retry_after_[from] = now;                     // try the earlier index at once
    }

    // A reply in this term to a call sent after a read arrived confirms, for that
    // read, that no newer leader existed when it arrived (D-076).
    for (PendingRead &r : reads_)
        if (sent.seq >= r.seq) r.acks[from] = true;

    send_if_due(from, now, false);                    // more to send? keep the pipe full
    finish();
}

void Raft::call_failed(int to, const Outgoing &sent, Millis now) {
    if (stopped_ || sent.is_vote || role_ != Role::Leader) return;
    if (inflight_[to] && sent.append.seq == inflight_seq_[to]) inflight_[to] = false;
    // Refused or timed out: wait a heartbeat before the next call, so a dead peer
    // is not called in a tight loop while entries are pending.
    retry_after_[to] = now + std::max<Millis>(cfg_.election / 5, 1);
}

// The leader's next call to `peer`: everything from next_[peer] on, up to about
// max_append_bytes of commands, or nothing (a heartbeat).
void Raft::send_append(int peer, Millis now) {
    AppendRequest r;
    r.term = term_;
    r.leader = cfg_.id;
    r.prev_index = next_[peer] - 1;
    r.prev_term = term_at(r.prev_index);
    r.commit = commit_;
    r.seq = ++seq_;
    std::size_t bytes = 0;
    for (Index i = next_[peer]; i <= last_index(); ++i) {
        const std::size_t cost = log_[i - 1].command.size() + 22;    // "<term> <command>\n"
        if (!r.entries.empty() && bytes + cost > cfg_.max_append_bytes) break;
        r.entries.push_back(log_[i - 1]);
        bytes += cost;
    }
    Outgoing o;
    o.to = peer;
    o.append = std::move(r);
    out_.push_back(std::move(o));
    inflight_[peer] = true;
    inflight_seq_[peer] = seq_;
    inflight_until_[peer] = now + cfg_.election;      // the call deadline is E (D-073)
    heartbeat_due_[peer] = now + std::max<Millis>(cfg_.election / 5, 1);
}

// Send to `peer` if nothing is in flight to it and either entries are waiting or
// a heartbeat is due (or `force`, for a read that needs a fresh acknowledgement).
void Raft::send_if_due(int peer, Millis now, bool force) {
    if (role_ != Role::Leader || inflight_[peer] || now < retry_after_[peer]) {
        if (force) heartbeat_due_[peer] = now;        // send as soon as the call in flight returns
        return;
    }
    if (force || now >= heartbeat_due_[peer] || next_[peer] <= last_index()) send_append(peer, now);
}

// The commit rule, Figure 2 with the restriction of section 5.4.2: the highest
// index held by a majority -- counting the leader, whose log finish() has just made
// durable -- AND whose entry is of the leader's own term. An entry of an earlier
// term is never committed by counting its copies: Figure 8 shows one held by a
// majority being overwritten. It commits indirectly, when a later entry does.
void Raft::advance_commit() {
    for (Index n = last_index(); n > commit_; --n) {
        if (term_at(n) != term_) break;               // terms only fall going back: nothing below is mine
        int copies = 1;
        for (int p = 0; p < cfg_.size; ++p)
            if (p != cfg_.id && match_[p] >= n) ++copies;
        if (majority(copies)) {
            commit_ = n;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Clients
// ---------------------------------------------------------------------------

bool Raft::propose(const std::string &command, Index &index, Term &term, Millis now) {
    if (stopped_ || role_ != Role::Leader) return false;
    log_.push_back(Entry{term_, command});
    changed_from(last_index());
    index = last_index();
    term = term_;
    for (int p = 0; p < cfg_.size; ++p)
        if (p != cfg_.id) send_if_due(p, now, false);
    finish();                                         // the leader's own copy is durable before it counts
    return !stopped_;
}

bool Raft::read(std::uint64_t id, Millis now) {
    if (stopped_ || role_ != Role::Leader) return false;
    PendingRead r;
    r.id = id;
    r.seq = seq_ + 1;                                 // the next call sent is the first that counts
    r.index = term_at(commit_) == term_ ? commit_ : 0;
    r.acks.assign(static_cast<std::size_t>(cfg_.size), false);
    r.acks[cfg_.id] = true;
    reads_.push_back(std::move(r));
    for (int p = 0; p < cfg_.size; ++p)
        if (p != cfg_.id) send_if_due(p, now, true);
    finish();
    return true;
}

// ---------------------------------------------------------------------------
// The end of every step
// ---------------------------------------------------------------------------

void Raft::changed_from(Index i) {
    dirty_ = true;
    if (dirty_from_ == 0 || i < dirty_from_) dirty_from_ = i;
}

void Raft::finish() {
    if (stopped_) {
        out_.clear();
        read_results_.clear();
        return;
    }
    // 1. Durable first. Everything this step changed -- term, vote, entries -- is
    // saved in one call (one flush, D-074) before any output can be taken.
    if (dirty_) {
        const Index from = dirty_from_ == 0 ? last_index() + 1 : dirty_from_;
        const bool ok = storage_.save(term_, voted_for_, from, log_);
        dirty_ = false;
        dirty_from_ = 0;
        if (!ok) {
            stopped_ = true;                          // fail-stop: never answer from state that may be lost
            out_.clear();
            read_results_.clear();
            return;
        }
    }
    // 2. Then commit (a leader counts its own entries only now that they are durable)
    // and apply, in order.
    if (role_ == Role::Leader) advance_commit();
    apply_committed();
    // 3. Then the reads this made answerable.
    resolve_reads();
}

void Raft::apply_committed() {
    while (applied_ < commit_) {
        ++applied_;
        apply_(applied_, log_[applied_ - 1]);
    }
}

// A read may be answered once (1) the leader has committed in its own term and set
// the read's index, (2) a majority has acknowledged it since the read arrived, and
// (3) everything up to the index is applied (D-076).
void Raft::resolve_reads() {
    if (reads_.empty()) return;
    if (role_ != Role::Leader) {
        fail_reads();
        return;
    }
    std::vector<PendingRead> waiting;
    for (PendingRead &r : reads_) {
        if (r.index == 0 && term_at(commit_) == term_) r.index = commit_;
        const int acks = static_cast<int>(std::count(r.acks.begin(), r.acks.end(), true));
        if (r.index != 0 && majority(acks) && applied_ >= r.index) read_results_.push_back(ReadResult{r.id, true});
        else waiting.push_back(std::move(r));
    }
    reads_ = std::move(waiting);
}

void Raft::fail_reads() {
    for (const PendingRead &r : reads_) read_results_.push_back(ReadResult{r.id, false});
    reads_.clear();
}

std::vector<Outgoing> Raft::take_outgoing() {
    std::vector<Outgoing> out;
    out.swap(out_);
    return out;
}

std::vector<Raft::ReadResult> Raft::take_reads() {
    std::vector<ReadResult> out;
    out.swap(read_results_);
    return out;
}
