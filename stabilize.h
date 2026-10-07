#ifndef STABILIZE_H
#define STABILIZE_H

#include "chord.h"
#include "lookup.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// A ring member's routing state, and everything that changes it (Phase 3).
//
// Like chord.cpp and lookup.cpp, nothing here opens a socket. Every call to
// another node goes through a Transport passed in (lookup.h), so the tests can
// run a whole ring in one process, kill and freeze nodes at chosen moments, and
// order every node's rounds by hand. node.cpp supplies real sockets.
//
// The concurrency rule (decision D-030):
//   NodeState::mu guards every mutable field. It is held only to compute or copy,
//   and NEVER while a Transport call is in progress. Anything read before a call
//   may have changed by the time the reply arrives, so every result is applied
//   only after re-checking the state it was based on.
// ---------------------------------------------------------------------------

constexpr int DEFAULT_SUCCESSORS = 8;      // r, the successor-list length (D-032)
constexpr int MAX_SUCCESSORS     = 16;     // what fits in one MAX_LINE reply (D-031)
constexpr int STRIKES_TO_EVICT   = 2;      // non-refusal failures in a row (D-035)
constexpr int JOIN_ATTEMPTS      = 30;     // join lookups before giving up (D-029)
constexpr int MAX_CONTACTS       = 32;     // addresses remembered for rejoining (D-040)

struct NodeState {
    NodeState(const Peer &me, int successors);

    const Peer self;                       // never changes, so read without the lock
    const int  r;                          // successor-list length, 1..MAX_SUCCESSORS

    // Where events go (an eviction, a new successor). Set once before any thread
    // starts and never changed. Called WITHOUT the lock held: it may write to a
    // pipe, and a full pipe must not be able to stall every request.
    std::function<void(const std::string &)> log;

    mutable std::mutex mu;                 // guards every field below
    bool              has_pred = false;    // Chord's "predecessor = nil" is has_pred == false
    Peer              pred;
    std::vector<Peer> succs;               // never empty: succs[0] IS the successor, {self} when alone
    std::vector<Peer> fingers;             // ID_BITS entries; fingers[0] always equals succs[0]
    int               next_finger = 1;     // where fix_fingers resumes (finger 0 is stabilise's)

    // Orphaned (D-040): every node this one knew has been evicted. It then
    // refuses to route -- answering would mean claiming the whole ring, (n, n] --
    // and each round tries to rejoin through an address it remembers.
    bool              orphaned = false;
    std::vector<Peer> contacts;            // newest first, at most MAX_CONTACTS, never self
    std::size_t       next_contact = 0;

    // The failure detector's memory (D-035): consecutive failed calls to each
    // peer this node holds -- successor, predecessor or finger -- by address.
    // Cleared by a valid reply; the second strike evicts.
    std::map<std::string, int> strikes;
};

// Same node? By ADDRESS, never by identifier (invariant I8).
bool same_node(const Peer &a, const Peer &b);

// ---- Starting states (D-029) ----------------------------------------------

// A ring of one: successor = self, every finger = self, no predecessor.
void init_alone(NodeState &s);

// The fixed-ring state from a sorted membership list (D-020), plus the next r
// members as the successor list. `members` must contain `s.self`.
void init_wired(NodeState &s, const std::vector<Peer> &members);

enum class JoinStatus {
    Joined,       // successor set; start listening and maintaining
    Retry,        // try again later; `why` says what went wrong
    Collision,    // another node already has this identifier (D-028); do not retry
};

// Look up our own identifier through `known` and take the owner as successor.
// `known` and the owner are remembered as contacts for rejoining (D-040).
// Retry if the lookup fails or if it names our own address: a previous
// incarnation of this node is still in the ring and must be evicted first.
JoinStatus join(NodeState &s, const Peer &known, const Transport &transport, std::string &why);

// ---- Answering requests ---------------------------------------------------

// One reply line, plus work to do AFTER the reply has been sent. NOTIFY uses
// `after` to probe a predecessor without making the notifier wait: a notifier
// kept waiting past its deadline would count a strike against this node.
struct Handled {
    std::string           reply;
    std::function<void()> after;      // empty if there is nothing to do
};

// Every request a node answers. Never throws on anything a peer sends (I5).
// An orphaned node answers ERR to PING, FIND_SUCCESSOR, NEIGHBOURS and NOTIFY (D-040).
//   PING                     -> PONG <id>
//   FIND_SUCCESSOR <id>      -> OWNER|NEXT <peer>            (route_step, as in Phase 2)
//   NEIGHBOURS               -> NEIGHBOURS <pred|NONE> <n> <peer>...
//   NOTIFY <peer>            -> OK
//   SUSPECT <ip> <port>      -> OK                           (after probing the suspect)
//   FINGERS                  -> FINGERS <n> <i> <peer> ...   (one entry per run)
// `transport` is used only to probe; it is not kept.
Handled handle_request(NodeState &s, const std::string &line, const Transport &transport);

// ---- The maintenance round (D-034, D-035) ----------------------------------

// Ask the successor for its neighbours; adopt a closer successor if one has
// appeared (after verifying it answers); take its successor list; notify it.
// A refused successor is evicted and the next one tried in the same round.
void stabilize(NodeState &s, const Transport &transport);

// Ping the predecessor; forget it on a refusal or a second strike.
void check_predecessor(NodeState &s, const Transport &transport);

// One lookup, for the next finger not yet refreshed; the owner fills every
// finger whose start it covers.
void fix_next_finger(NodeState &s, const Transport &transport);

// stabilize, then check_predecessor, then fix_next_finger -- or, for an
// orphaned node, one attempt to rejoin through the next remembered contact.
void maintenance_round(NodeState &s, const Transport &transport);

// ---- Wire formats, exposed so the tests can attack them --------------------

struct Neighbours {
    bool              has_pred = false;
    Peer              pred;
    std::vector<Peer> succs;
};

std::string render_neighbours(const Neighbours &n);
// Strict: exact words, a count in 0..MAX_SUCCESSORS that matches the peers given.
bool parse_neighbours(const std::string &line, Neighbours &out, std::string &why);

// The successor list a node should hold after asking `succ`: succ first, then
// succ's own list, cut where it wraps back round to `self`, without repeats or
// anything carrying self's identifier (D-028), at most r entries.
std::vector<Peer> merge_successors(const Peer &self, const Peer &succ,
                                   const std::vector<Peer> &succ_list, int r);

#endif
