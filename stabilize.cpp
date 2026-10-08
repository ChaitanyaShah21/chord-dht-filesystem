#include "stabilize.h"
#include "net.h"

#include <chrono>
#include <cstddef>
#include <iterator>
#include <sstream>
#include <utility>

namespace {

using Events = std::vector<std::string>;

std::string address(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

// The failure detector's key for one ring member (D-047): its address and its
// identifier, because two members of one process share an address.
std::string member_key(const Peer &p) { return address(p) + "/" + id_to_hex(p.id); }

// Hand collected events to the log, AFTER the lock has been released (D-030).
void emit(const NodeState &s, const Events &events) {
    if (!s.log) return;
    for (const std::string &e : events) s.log(e);
}

bool is_pong(const std::string &reply) { return reply.compare(0, 5, "PONG ") == 0; }
bool is_ok(const std::string &reply)   { return reply == "OK"; }

// ---------------------------------------------------------------------------
// Eviction and strikes (D-035). Every *_locked function requires s.mu to be
// held by the caller, and none of them makes a call to another node.
// ---------------------------------------------------------------------------

// The nearest node clockwise that we still know of, other than ourselves and
// `dead` -- the fallback when every successor-list entry has gone. Falling back
// to ourselves would mean claiming the whole ring, (n, n], so it is the very
// last resort.
Peer nearest_known_locked(const NodeState &s, const std::function<bool(const Peer &)> &gone) {
    Peer best = s.self;
    Id best_distance = 0;                              // 0 means "nothing found yet"
    auto consider = [&](const Peer &p) {
        if (same_node(p, s.self) || gone(p) || p.id == s.self.id) return;
        const Id distance = p.id - s.self.id;          // clockwise, wraps modulo 2^64
        if (best_distance == 0 || distance < best_distance) { best = p; best_distance = distance; }
    };
    for (const Peer &f : s.fingers) consider(f);
    if (s.has_pred) consider(s.pred);
    return best;
}

// Remember an address for rejoining (D-040): newest first, no repeats, bounded.
void remember_locked(NodeState &s, const Peer &p) {
    if (same_process(p, s.self)) return;               // not an independent way back in
    for (std::size_t i = 0; i < s.contacts.size(); ++i) {
        if (same_node(s.contacts[i], p)) { s.contacts.erase(s.contacts.begin() + i); break; }
    }
    s.contacts.insert(s.contacts.begin(), p);
    if (s.contacts.size() > static_cast<std::size_t>(MAX_CONTACTS)) s.contacts.pop_back();
}

// Remove `dead` from every structure: successor list, predecessor, fingers.
// With `whole_process`, every member at dead's ADDRESS goes: a refused connection
// proves nothing is listening there, which is true of all its members at once
// (D-047). Otherwise only the member itself, the subject of a timeout or a bad reply.
// A sibling is never evicted on evidence about the process -- if a sibling looks
// dead, so are we -- except with `sibling_said_so`: the sibling itself answered that
// it is orphaned, out of the ring (D-067).
void evict_locked(NodeState &s, const Peer &dead, const std::string &why, Events &events,
                  bool whole_process, bool sibling_said_so = false) {
    if (same_node(dead, s.self)) return;               // never evict ourselves
    if (same_process(dead, s.self) && !sibling_said_so) return;   // nor our siblings, on suspicion
    auto gone = [&](const Peer &p) { return whole_process ? same_process(p, dead) : same_node(p, dead); };

    // The entry after `dead` in our list now owns everything `dead` owned, so it
    // is the exact replacement for any finger that pointed at `dead`.
    Peer replacement;
    bool have_replacement = false;
    // With a whole process gone, the replacement is the first entry after it that
    // is not also one of its members.
    for (std::size_t i = 0; i < s.succs.size() && !have_replacement; ++i) {
        if (!gone(s.succs[i])) continue;
        for (std::size_t j = i + 1; j < s.succs.size(); ++j) {
            if (!gone(s.succs[j])) { replacement = s.succs[j]; have_replacement = true; break; }
        }
        break;
    }

    bool was_known = false;
    std::vector<Peer> kept;
    for (const Peer &p : s.succs) {
        if (gone(p)) was_known = true;
        else kept.push_back(p);
    }
    if (kept.empty()) {
        const Peer fallback = nearest_known_locked(s, gone);
        kept.push_back(fallback);
        if (same_node(fallback, s.self) && !same_node(s.succs[0], s.self)) {
            // Nobody left that we know of. Claiming the ring would be (n, n]:
            // every key, confidently. Refuse to route and rejoin instead (D-040).
            s.orphaned = true;
            events.push_back("orphaned: every known node is gone; refusing to route until rejoined");
        } else {
            events.push_back("successor list exhausted; falling back to " + address(fallback));
        }
    }
    s.succs = std::move(kept);

    if (s.has_pred && gone(s.pred)) {
        s.has_pred = false;
        was_known = true;
    }
    for (Peer &f : s.fingers) {
        if (gone(f)) {
            f = have_replacement ? replacement : s.succs[0];
            was_known = true;
        }
    }
    s.fingers[0] = s.succs[0];

    // Forget the strikes of everything evicted. The log line names the address
    // first, so tools that read "evict <ip:port> (" keep working; a single member's
    // eviction also names the member.
    for (auto it = s.strikes.begin(); it != s.strikes.end();) {
        const bool match = whole_process ? it->first.compare(0, address(dead).size() + 1, address(dead) + "/") == 0
                                         : it->first == member_key(dead);
        it = match ? s.strikes.erase(it) : std::next(it);
    }
    if (was_known)
        events.push_back("evict " + address(dead) + " (" + why + ")" +
                         (whole_process ? "" : " member " + id_to_hex(dead.id)));
}

// A failure that is not a refusal: suspicion, not proof (D-013). It counts
// against any peer this node holds -- successor, predecessor or finger -- and
// the second in a row evicts. Fingers matter as much as neighbours here: a
// finger lookup routes through the node's own fingers, so a frozen finger
// target would otherwise make its own repair time out for ever.
//
// `detail` is appended to the strike's log line only (not to the eviction's), for
// evidence such as how long the failed call actually took.
void strike_locked(NodeState &s, const Peer &p, const std::string &why, Events &events,
                   const std::string &detail = "") {
    if (same_process(p, s.self)) return;               // we are running: our siblings are alive
    bool held = s.has_pred && same_node(p, s.pred);
    for (const Peer &q : s.succs) held = held || same_node(p, q);
    for (const Peer &f : s.fingers) held = held || same_node(p, f);
    if (!held) return;                         // nothing to evict it from

    const int n = ++s.strikes[member_key(p)];
    events.push_back("strike " + std::to_string(n) + " against " + address(p) + " (" + why + ")" + detail);
    if (n >= STRIKES_TO_EVICT) evict_locked(s, p, why + ", second strike", events, false);
}

void clear_strikes_locked(NodeState &s, const Peer &p) { s.strikes.erase(member_key(p)); }

// ---------------------------------------------------------------------------
// The one wrapper every maintenance call goes through (D-013, D-035), so that
// no call site can forget to report what happened.
// ---------------------------------------------------------------------------

enum class Call { Good, Refused, Failed };

// Send `request` to `to`. A reply is Good only if it arrived, is not ERR, and
// passes `valid` (which usually parses it). The outcome is reported here:
// Good clears strikes, a refusal evicts, anything else is a strike.
Call call_peer(NodeState &s, const Transport &transport, const Peer &to, const std::string &request,
               std::string &reply, const std::function<bool(const std::string &)> &valid) {
    // How long the call took, by the monotonic clock. A timeout that took far longer
    // than the deadline means this process was not running when the timer fired, which
    // is evidence about this process rather than about the peer; it is logged so that
    // a false eviction can be explained afterwards.
    const auto t0 = std::chrono::steady_clock::now();
    const Delivery d = transport(to, request, reply);        // no lock held: this may take a while
    const long long took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    const bool good = d == Delivery::Ok && reply.compare(0, 3, "ERR") != 0 && valid(reply);
    // An orphan says so (D-040). That is the member's own word that it is out of the
    // ring -- proof, like a refusal, not suspicion -- so it is evicted at once, by any
    // node, its own siblings included (D-067). Without this, a sibling that had fallen
    // back onto an orphan kept it as successor for ever, and the orphan's every rejoin
    // lookup ended at that sibling, naming the orphan itself.
    const bool orphan = d == Delivery::Ok && reply.compare(0, 12, "ERR orphaned") == 0;

    Events events;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (good)                          clear_strikes_locked(s, to);
        else if (orphan)                   evict_locked(s, to, "it says it is orphaned", events, false, true);
        else if (d == Delivery::Refused)   evict_locked(s, to, "refused", events, true);
        else strike_locked(s, to, d == Delivery::Ok ? "bad reply to " + request.substr(0, request.find(' '))
                                                    : delivery_name(d), events,
                           " after " + std::to_string(took_ms) + " ms");
    }
    emit(s, events);
    if (good) return Call::Good;
    return d == Delivery::Refused ? Call::Refused : Call::Failed;
}

// A strict count: digits only, no leading zero, 1..MAX_SUCCESSORS.
bool parse_count(const std::string &w, int &out) {
    if (w.empty() || w.size() > 2 || w[0] == '0') return false;
    int value = 0;
    for (char c : w) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > MAX_SUCCESSORS) return false;
    out = value;
    return true;
}

}  // namespace

NodeState::NodeState(const Peer &me, int successors)
    : self(me), r(successors), succs{me}, fingers(ID_BITS, me) {}

bool same_node(const Peer &a, const Peer &b) { return a.ip == b.ip && a.port == b.port && a.id == b.id; }

bool same_process(const Peer &a, const Peer &b) { return a.ip == b.ip && a.port == b.port; }

// ---------------------------------------------------------------------------
// Starting states
// ---------------------------------------------------------------------------

void init_alone(NodeState &s) {
    std::lock_guard<std::mutex> lock(s.mu);
    s.has_pred = false;
    s.succs.assign(1, s.self);
    s.fingers.assign(ID_BITS, s.self);
    s.next_finger = 1;
}

void init_wired(NodeState &s, const std::vector<Peer> &members) {
    std::lock_guard<std::mutex> lock(s.mu);
    s.fingers = build_fingers(s.self.id, members);

    // Find ourselves by address (I8), then take the next r members clockwise,
    // stopping if the list wraps round to us.
    std::size_t me = 0;
    while (me < members.size() && !same_node(members[me], s.self)) ++me;
    s.succs.clear();
    for (std::size_t step = 1; step < members.size() && static_cast<int>(s.succs.size()) < s.r; ++step)
        s.succs.push_back(members[(me + step) % members.size()]);
    if (s.succs.empty()) s.succs.push_back(s.self);          // a ring of one

    s.has_pred = members.size() > 1;
    if (s.has_pred) s.pred = predecessor_of(s.self.id, members);
    s.fingers[0] = s.succs[0];
    s.next_finger = 1;
    for (const Peer &p : s.succs) remember_locked(s, p);
}

JoinStatus join(NodeState &s, const Peer &known, const Transport &transport, std::string &why) {
    const LookupResult r = lookup(known, s.self.id, transport);
    if (!r.ok()) {
        why = std::string("lookup failed (") + lookup_error_name(r.error) + "): " + r.message;
        return JoinStatus::Retry;
    }
    if (same_node(r.owner, s.self)) {
        // An earlier incarnation of this address is still in someone's tables.
        // We are not listening, so its next stabilise is refused and evicts it.
        why = "the ring still names " + address(s.self) + " as a member; waiting for it to be evicted";
        return JoinStatus::Retry;
    }
    if (r.owner.id == s.self.id) {
        why = "identifier " + id_to_hex(s.self.id) + " is already used by " + address(r.owner) +
              "; start this node on another port";
        return JoinStatus::Collision;
    }

    // Copy the successor's list now rather than at the first stabilise, so a
    // newcomer never depends on one node alone (D-040). Best effort: without it
    // the list is just the successor, and the first round fills it in. No
    // NOTIFY here -- we are not listening yet, and a successor that accepted us
    // as predecessor would ping us, be refused, and evict us.
    Neighbours nb;
    std::string reply, ignored;
    const bool have_list = transport(r.owner, "NEIGHBOURS", reply) == Delivery::Ok &&
                           parse_neighbours(reply, nb, ignored);

    std::lock_guard<std::mutex> lock(s.mu);
    s.has_pred = false;
    s.succs = have_list ? merge_successors(s.self, r.owner, nb.succs, s.r) : std::vector<Peer>{r.owner};
    s.fingers.assign(ID_BITS, r.owner);       // valid hints: every one makes progress
    s.next_finger = 1;
    s.orphaned = false;
    remember_locked(s, known);
    for (auto it = s.succs.rbegin(); it != s.succs.rend(); ++it) remember_locked(s, *it);
    return JoinStatus::Joined;
}

// ---------------------------------------------------------------------------
// Wire formats
// ---------------------------------------------------------------------------

std::string render_neighbours(const Neighbours &n) {
    std::string out = "NEIGHBOURS ";
    out += n.has_pred ? peer_words(n.pred) : "NONE";
    out += " " + std::to_string(n.succs.size());
    for (const Peer &p : n.succs) out += " " + peer_words(p);
    return out;
}

bool parse_neighbours(const std::string &line, Neighbours &out, std::string &why) {
    const std::vector<std::string> w = split_single_spaces(line);
    if (w.size() < 3 || w[0] != "NEIGHBOURS") { why = "not a NEIGHBOURS reply"; return false; }

    Neighbours parsed;
    std::size_t i = 1;
    if (w[1] == "NONE") {
        i = 2;
    } else {
        if (w.size() < 5 || !parse_peer_words(w[1], w[2], w[3], parsed.pred, why)) {
            if (why.empty()) why = "predecessor is incomplete";
            return false;
        }
        parsed.has_pred = true;
        i = 4;
    }

    int n = 0;
    if (!parse_count(w[i], n)) { why = "successor count is not a number in 1.." + std::to_string(MAX_SUCCESSORS); return false; }
    ++i;
    if (w.size() - i != static_cast<std::size_t>(3 * n)) { why = "successor count does not match the peers given"; return false; }
    for (int k = 0; k < n; ++k, i += 3) {
        Peer p;
        if (!parse_peer_words(w[i], w[i + 1], w[i + 2], p, why)) return false;
        parsed.succs.push_back(p);
    }
    out = std::move(parsed);                    // written only on success
    return true;
}

std::vector<Peer> merge_successors(const Peer &self, const Peer &succ,
                                   const std::vector<Peer> &succ_list, int r) {
    std::vector<Peer> out{succ};
    for (const Peer &p : succ_list) {
        if (static_cast<int>(out.size()) >= r) break;
        if (same_node(p, self)) break;          // the list has wrapped round to us
        if (p.id == self.id) continue;          // carries our identifier: never adopt (D-028)
        bool repeat = false;
        for (const Peer &q : out) repeat = repeat || same_node(p, q);
        if (!repeat) out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Answering requests
// ---------------------------------------------------------------------------

Handled handle_request(NodeState &s, const std::string &line, const Transport &transport) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    if (words.empty()) return {"ERR empty request", {}};
    const std::string &verb = words[0];

    // An orphan answers ERR to everything the ring uses to decide it is alive or
    // to route through it -- PING included. Answering PING would keep it in its
    // neighbours' tables (their checks would see it alive), and its own rejoin
    // would then wait for ever for the ring to forget it (D-040).
    if (verb == "PING" || verb == "FIND_SUCCESSOR" || verb == "NEIGHBOURS" || verb == "NOTIFY") {
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.orphaned) return {"ERR orphaned: rejoining the ring", {}};
    }

    if (verb == "PING" && words.size() == 1) return {"PONG " + id_to_hex(s.self.id), {}};

    if (verb == "FIND_SUCCESSOR" && words.size() == 2) {
        Id k;
        if (!id_from_hex(words[1], k)) return {"ERR identifier must be 16 hexadecimal characters", {}};
        RouteStep step;
        {
            std::lock_guard<std::mutex> lock(s.mu);          // a 64-entry scan: microseconds
            step = route_step(s.self, s.succs[0], s.fingers, k);
        }
        return {(step.owner ? "OWNER " : "NEXT ") + peer_words(step.peer), {}};
    }

    if (verb == "NEIGHBOURS" && words.size() == 1) {
        Neighbours n;
        {
            std::lock_guard<std::mutex> lock(s.mu);          // one snapshot: pred and list agree
            n.has_pred = s.has_pred;
            n.pred     = s.pred;
            n.succs    = s.succs;
        }
        return {render_neighbours(n), {}};
    }

    if (verb == "NOTIFY" && words.size() == 4) {
        Peer c;
        std::string why;
        if (!parse_peer_words(words[1], words[2], words[3], c, why)) return {"ERR " + why, {}};
        if (same_node(c, s.self)) return {"OK", {}};
        Events events;
        if (c.id == s.self.id) {
            events.push_back("refusing " + address(c) + " as predecessor: it carries my identifier");
            emit(s, events);
            return {"OK", {}};
        }

        Peer old;
        bool probe = false;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (!s.has_pred || in_range_oo(c.id, s.pred.id, s.self.id)) {
                s.pred = c;                                  // closer than what we had: Chord's notify
                s.has_pred = true;
                s.strikes.erase(member_key(c));
                remember_locked(s, c);
                events.push_back("predecessor is now " + address(c));
            } else if (!same_node(s.pred, c)) {
                old = s.pred;                                // not closer: is the old one still alive?
                probe = true;
            }
        }
        emit(s, events);
        if (!probe) return {"OK", {}};

        // Probe AFTER replying (D-035 part 5): a frozen predecessor can take the
        // whole deadline to time out, and the notifier must not wait for that.
        // `s` and `transport` are captured by reference: `after` runs before the
        // caller of handle_request returns, while both are still alive.
        return {"OK", [&s, &transport, old, c]() {
            std::string pong;
            if (call_peer(s, transport, old, "PING", pong, is_pong) == Call::Good) return;
            Events later;
            {
                // The probe failed. If that evicted the old predecessor -- a refusal,
                // or its second strike -- the slot is empty and the notifier fills it.
                std::lock_guard<std::mutex> lock(s.mu);
                if (!s.has_pred) {
                    s.pred = c;
                    s.has_pred = true;
                    remember_locked(s, c);
                    later.push_back("predecessor is now " + address(c));
                }
            }
            emit(s, later);
        }};
    }

    if (verb == "SUSPECT" && words.size() == 3) {
        // Only an exact match with a peer we hold counts; anything else -- a
        // node we do not know, a non-canonical address -- has nothing to evict.
        const std::string named = words[1] + ":" + words[2];
        Peer target;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            auto check = [&](const Peer &p) { if (!known && address(p) == named) { target = p; known = true; } };
            if (s.has_pred) check(s.pred);
            for (const Peer &p : s.succs) check(p);
            for (const Peer &p : s.fingers) check(p);
        }
        if (!known || same_process(target, s.self)) return {"OK", {}};

        // A hint, not a verdict (D-031): probe with our own connection, and act
        // only on a refusal. A timeout from a hint changes nothing, so a client
        // cannot run up strikes against a slow node.
        std::string pong;
        if (transport(target, "PING", pong) == Delivery::Refused) {
            Events events;
            {
                std::lock_guard<std::mutex> lock(s.mu);
                evict_locked(s, target, "refused, after a client reported it", events, true);
            }
            emit(s, events);
        }
        return {"OK", {}};
    }

    if (verb == "FINGERS" && words.size() == 1) {
        std::string body;
        int runs = 0;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            for (int i = 0; i < ID_BITS; ++i) {
                if (i > 0 && same_node(s.fingers[i], s.fingers[i - 1])) continue;   // same run
                body += " " + std::to_string(i) + " " + peer_words(s.fingers[i]);
                ++runs;
            }
        }
        std::string reply = "FINGERS " + std::to_string(runs) + body;
        if (reply.size() > MAX_LINE) return {"ERR too many distinct fingers for one line", {}};
        return {reply, {}};
    }

    return {"ERR unknown request", {}};
}

// ---------------------------------------------------------------------------
// The maintenance round
// ---------------------------------------------------------------------------

void stabilize(NodeState &s, const Transport &transport) {
    // Each pass asks the current successor. If the call evicted it -- a refusal,
    // or a second strike -- the next pass asks the next entry in the same round,
    // so k adjacent dead nodes cost one round, not k. And if a pass adopts a
    // closer successor, the next pass asks THAT node, until no closer node is
    // named (D-043). The paper takes one inward step per round, so a node that
    // fell back to a distant node walked back one node per period.
    for (int pass = 0; pass <= MAX_SUCCESSORS + MAX_WALK; ++pass) {
        Peer succ;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            succ = s.succs[0];
        }

        Neighbours nb;
        std::string reply, why;
        const Call c = call_peer(s, transport, succ, "NEIGHBOURS", reply,
                                 [&](const std::string &line) { return parse_neighbours(line, nb, why); });
        if (c != Call::Good) {
            std::lock_guard<std::mutex> lock(s.mu);
            if (same_node(s.succs[0], succ)) return;   // a first strike: next round asks again
            continue;                                  // evicted: try the next entry now
        }

        // Apply what the successor said -- but only if it is still our successor (D-030).
        Peer candidate;
        bool verify = false;
        Events events;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (!same_node(s.succs[0], succ)) return;
            s.succs = merge_successors(s.self, succ, nb.succs, s.r);
            s.fingers[0] = s.succs[0];
            for (auto it = s.succs.rbegin(); it != s.succs.rend(); ++it) remember_locked(s, *it);
            if (nb.has_pred && !same_node(nb.pred, s.self) && !same_node(nb.pred, succ)) {
                if (nb.pred.id == s.self.id)
                    events.push_back("refusing " + address(nb.pred) + " as successor: it carries my identifier");
                else if (in_range_oo(nb.pred.id, s.self.id, succ.id)) {
                    candidate = nb.pred;              // someone has joined between us
                    verify = true;
                }
            }
        }
        emit(s, events);

        // Verify before adopting (D-035 part 4): our successor's predecessor
        // pointer may still name a node that we have just evicted as dead.
        if (verify) {
            std::string pong;
            if (call_peer(s, transport, candidate, "PING", pong, is_pong) == Call::Good) {
                Events adopted;
                bool moved = false;
                {
                    std::lock_guard<std::mutex> lock(s.mu);
                    if (same_node(s.succs[0], succ)) {
                        std::vector<Peer> list{candidate};
                        for (const Peer &p : s.succs)
                            if (!same_node(p, s.self) && static_cast<int>(list.size()) < s.r) list.push_back(p);
                        s.succs = std::move(list);
                        s.fingers[0] = s.succs[0];
                        remember_locked(s, candidate);
                        adopted.push_back("successor is now " + address(candidate));
                        moved = true;
                    }
                }
                emit(s, adopted);
                // Ask the new successor in the same round: it may know a closer
                // node still. Every step moves strictly inward, so this ends.
                if (moved) continue;
            }
        }

        // Tell the successor about us. Its handler decides whether we are closer
        // than its current predecessor; we do not need to know the answer.
        Peer target;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            target = s.succs[0];
        }
        if (!same_node(target, s.self)) {
            std::string ok;
            call_peer(s, transport, target, "NOTIFY " + peer_words(s.self), ok, is_ok);
        }
        return;
    }
}

void check_predecessor(NodeState &s, const Transport &transport) {
    Peer p;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (!s.has_pred || same_node(s.pred, s.self)) return;
        p = s.pred;
    }
    std::string pong;
    call_peer(s, transport, p, "PING", pong, is_pong);     // reports refusals and strikes itself
}

void fix_next_finger(NodeState &s, const Transport &transport) {
    int i;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        i = s.next_finger;
    }
    const Id start = finger_start(s.self.id, i);

    // The client's own loop, starting at ourselves (D-034).
    const LookupResult r = lookup(s.self, start, transport);

    Events events;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (!r.ok()) {
            // A failed hop is evidence too (D-013): a refusal evicts the node that
            // refused, and anything else that node did wrong -- no answer, ERR, a
            // garbled or backwards reply -- is a strike against it. Only a failure
            // no single node can be blamed for (the request cap, a local error)
            // is held against nobody.
            const bool blame = r.error != LookupError::TooManyRequests && r.error != LookupError::Unreachable;
            if (blame && !r.path.empty()) {
                const std::string why = std::string(lookup_error_name(r.error)) + " during fix_fingers";
                if (r.error == LookupError::Refused) evict_locked(s, r.path.back(), why, events, true);
                else strike_locked(s, r.path.back(), why, events);
            }
            // Then MOVE ON. Retrying the same finger every round would let one
            // finger whose lookup keeps failing stop every later finger from ever
            // being refreshed -- seen on a real ring when a node was frozen.
            s.next_finger = (i + 1 >= ID_BITS) ? 1 : i + 1;
        } else if (r.owner.id == s.self.id && !same_node(r.owner, s.self)) {
            events.push_back("refusing " + address(r.owner) + " as a finger: it carries my identifier");
            s.next_finger = (i + 1 >= ID_BITS) ? 1 : i + 1;
        } else {
            // Fill forward: every later start that also lies in (self, owner] has
            // the same owner, because no node lies between that start and the owner.
            int j = i;
            while (j < ID_BITS && in_range_oc(finger_start(s.self.id, j), s.self.id, r.owner.id)) {
                s.fingers[j] = r.owner;
                ++j;
            }
            if (j == i) {                    // cannot happen for an answer that passed lookup()'s
                s.fingers[i] = r.owner;      // checks; guarded so a bad answer still advances
                j = i + 1;
            }
            s.fingers[0] = s.succs[0];
            s.next_finger = (j >= ID_BITS) ? 1 : j;
            // Finger owners are spread round the ring, unlike successors, so remembering
            // them keeps a contact alive when a whole neighbourhood dies at once (D-040).
            remember_locked(s, r.owner);
        }
    }
    emit(s, events);
}

void maintenance_round(NodeState &s, const Transport &transport) {
    // An orphan does no maintenance -- it has nobody to maintain against -- and
    // instead tries one remembered contact per round, round-robin (D-040).
    Peer contact;
    bool orphaned, have_contact = false;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        orphaned = s.orphaned;
        if (orphaned && !s.contacts.empty()) {
            contact = s.contacts[s.next_contact++ % s.contacts.size()];
            have_contact = true;
        }
    }
    if (orphaned) {
        if (!have_contact) return;
        std::string why;
        const JoinStatus st = join(s, contact, transport, why);
        Events events{st == JoinStatus::Joined ? "rejoined through " + address(contact)
                                               : "rejoin through " + address(contact) + " failed: " + why};
        emit(s, events);
        return;
    }

    stabilize(s, transport);
    check_predecessor(s, transport);
    fix_next_finger(s, transport);
}
