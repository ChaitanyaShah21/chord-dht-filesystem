#ifndef LOOKUP_H
#define LOOKUP_H

#include "chord.h"
#include "net.h"

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The lookup originator (decision D-024).
//
// Iterative routing (D-012): the originator asks a node FIND_SUCCESSOR k, the
// node answers OWNER (my successor owns k) or NEXT (ask this closer node), and
// the originator itself asks the next node. This file is that loop and nothing
// else. It never touches a socket: the one operation "send this line to this
// node and give me its reply" is passed in, so tests can hand it a ring built
// in process -- including rings that lie.
// ---------------------------------------------------------------------------

// The exchange the loop needs: send `request` to `to`, put its one-line reply
// in `reply`, and say what happened. Real sockets supply it in the lookup tool
// (see socket_transport below); tests supply constructed rings.
//
// LIFETIME RULE -- read before changing lookup():
//   A Transport is valid ONLY for the duration of the lookup() call it is
//   passed to. lookup() must never store it: no member, no static, no cache,
//   no detached thread, no deferred retry. Tests build transports as lambdas
//   that capture a local ring BY REFERENCE. If lookup() kept a copy and called
//   it after that ring was destroyed, it would read freed memory -- and freed
//   memory usually still holds the old bytes, so the tests would keep passing
//   and the failure would surface later, somewhere unrelated. Copying the
//   std::function does not make it safe: the copy holds the same reference.
using Transport = std::function<Delivery(const Peer &to, const std::string &request,
                                         std::string &reply)>;

// Everything that can end a lookup. Each error message names the node at fault.
enum class LookupError {
    None,            // success: `owner` is valid
    Refused,         // a node's address refused the connection: nothing listening (proof, D-013)
    Timeout,         // a node did not answer within the deadline (suspicion only, D-013)
    Closed,          // a node closed or reset the connection before replying
    Unreachable,     // any other transport failure reaching a node
    ErrReply,        // a node answered "ERR <reason>"
    Malformed,       // a node's reply was not "OWNER|NEXT <16-hex id> <ip> <port>"
    NoProgress,      // a node's reply would not move the lookup towards k (D-024, F19d)
    TooManyRequests, // MAX_REQUESTS sent without reaching an owner
};

// A short lowercase name for an error, for output lines: "refused", "no-progress", ...
const char *lookup_error_name(LookupError e);

// The most requests one lookup may send on a correct fixed ring (D-024, F19d).
//
// Let p be k's predecessor -- the node that will answer OWNER. With correct
// fingers, each NEXT more than halves the clockwise distance to p: if p lies in
// [n + 2^i, n + 2^(i+1)), finger i lies in [n + 2^i, p], so the jump covers at
// least 2^i, which is more than half of (p - n). A distance that starts below
// 2^64 and more than halves each time reaches 0 in at most 64 halvings, so at
// most 64 NEXT replies reach p, and p's OWNER reply is request 65.
//
// The bound is TIGHT: test_lookup.cpp builds a 65-node ring on which one lookup
// needs exactly 65 requests. A 66th request therefore means a bug or a liar.
// Stale fingers during churn can legitimately need more; D-037 keeps the cap
// anyway, so such a lookup fails with its reason named and is retried.
constexpr int MAX_REQUESTS = 65;

// The originator's per-socket deadline (D-024 F19c, applied before connect by
// D-025). A hang guard, not a failure detector: far above any loopback reply.
constexpr int LOOKUP_TIMEOUT_MS = 5000;

struct LookupResult {
    LookupError       error = LookupError::None;
    std::string       message;   // empty on success; otherwise names the offending node
    Peer              owner;     // valid only when error == None
    std::vector<Peer> path;      // every node a request was sent to, in order --
                                 // on failure, the last entry is the node that failed

    bool ok() const { return error == LookupError::None; }

    // Two counts from one path (decision D-026). requests() is the number of
    // FIND_SUCCESSOR messages sent. hops() is the number of NEXT replies
    // followed -- fingers followed, the Chord paper's path length -- which is
    // one fewer, because the first request only reaches the starting node.
    int requests() const { return static_cast<int>(path.size()); }
    int hops() const { return path.empty() ? 0 : requests() - 1; }

    // How many times the whole lookup was run. Always 1 from lookup(); more
    // only from lookup_with_recovery(), and then `path` is the LAST attempt's.
    int attempts = 1;
};

// Find the owner of identifier k, starting at `start`.
//
// Every reply is checked before it is believed (D-024, F19d):
//   NEXT  n  is accepted only if n is strictly inside (current, k),
//   OWNER o  is accepted only if k is in (current, o].
// A correct node passes both even with a stale table, so a violation names a
// faulty or lying node. Because the distance to k strictly shrinks, no node
// can be asked twice: a cycle is impossible by construction.
//
// Never throws on anything a node sends, however malformed (invariant I5).
LookupResult lookup(const Peer &start, Id k, const Transport &transport);

// Something to tell about every lookup when it ends -- its result and how long it
// took, in seconds -- for a process's metrics (D-084). A plain function pointer, set
// once at start-up before any other thread exists, and null (nothing told) by
// default, so tests and tools are unaffected. Each attempt of lookup_with_recovery is
// a lookup of its own.
using LookupObserver = void (*)(const LookupResult &result, double seconds);
void set_lookup_observer(LookupObserver observer);

// lookup(), plus recovery from a node that could not be reached (decision D-036).
//
// If a lookup fails because the node it was sent to refused, timed out or closed
// the connection, and an EARLIER node had named that node, the originator sends
//
//     SUSPECT <ip> <port>
//
// to the node that named it, and then runs the whole lookup again from `start`,
// at most `retries` more times. The SUSPECT reply is ignored: it is a hint, and
// the receiver acts only on its own probe of the suspect. With retries == 0 this
// is exactly lookup(). The same lifetime rule applies: `transport` is never kept.
LookupResult lookup_with_recovery(const Peer &start, Id k, const Transport &transport, int retries);

// The real transport: one fresh connection per request (D-024 F19b), with
// LOOKUP_TIMEOUT_MS on connect, send and receive. Holds no state, so it is
// safe to pass anywhere. The request is addressed to the member `to` names
// (addressed(), below), so it reaches the right member of a process, and carries
// the current request id, if there is one (D-087).
Delivery socket_transport(const Peer &to, const std::string &request, std::string &reply);

// Strictly parse one reply line: exactly "OWNER" or "NEXT", one space, 16 hex
// characters, one space, a canonical dotted-quad IPv4 address, one space, and a
// port in 1..65535 with no sign or leading zero. Exposed so tests can attack it
// directly. On failure returns false, sets `why`, and leaves the outputs alone.
bool parse_reply(const std::string &line, bool &is_owner, Peer &peer, std::string &why);

// ---------------------------------------------------------------------------
// Wire helpers shared with the node's maintenance messages (stabilize.cpp), so
// that every message naming a peer is rendered and parsed by one copy.
// ---------------------------------------------------------------------------

// A peer as it appears on the wire: "<16-hex id> <ip> <port>".
std::string peer_words(const Peer &p);

// Split on SINGLE spaces. A leading, trailing or doubled space produces an empty
// word, so a sloppy line fails its word count instead of being quietly accepted.
std::vector<std::string> split_single_spaces(const std::string &s);

// Parse one peer from its three wire words, as strictly as parse_reply does:
// 16 hexadecimal characters, a canonical dotted quad, a port in 1..65535 with no
// sign or leading zero. On failure returns false, sets `why`, leaves `out` alone.
bool parse_peer_words(const std::string &id, const std::string &ip, const std::string &port,
                      Peer &out, std::string &why);

// ---------------------------------------------------------------------------
// Addressing a member of a process (virtual nodes, decision D-047).
//
// One process listens on one socket and may be several ring members, so a
// request says which member it is for:   TO <16-hex identifier> <request>
// A request without the prefix is for the process's member 0, whose identifier
// is SHA-1 of its address -- so tools written before virtual nodes still work.
// ---------------------------------------------------------------------------

// "TO <to.id> " + request. Every transport that sends to a ring member uses it.
std::string addressed(const Peer &to, const std::string &request);

enum class Target {
    None,        // no prefix: `rest` is the whole line, for member 0
    Member,      // "TO <id> <rest>": `id` and `rest` set
    Malformed,   // starts with "TO " but the identifier or the space after it is wrong
};
// Split a received line. Strict: exactly "TO", one space, 16 hexadecimal
// characters, one space, then a non-empty request.
Target split_target(const std::string &line, Id &id, std::string &rest);

#endif
