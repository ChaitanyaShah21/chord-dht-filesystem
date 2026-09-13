#ifndef CHORD_H
#define CHORD_H

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The identifier space (decision D-019).
//
// A ROUTING IDENTIFIER is the top 64 bits of a SHA-1 digest, held in a uint64_t.
// A KEY is the whole 40-character digest, and it does not appear in this file.
//
//   Invariant I7: an Id decides WHICH NODE holds an object, never WHICH OBJECT.
//                 The chunk store is keyed by the full 40-character key.
//
// Everything here is a free function taking its state as an explicit argument
// (decision D-018), so a ring can be built by hand in a test with no sockets,
// no ports and no processes.
// ---------------------------------------------------------------------------

using Id = std::uint64_t;

constexpr int ID_BITS = 64;

// Parse a 40-character hexadecimal SHA-1 digest into a routing identifier.
// Every one of the 40 characters is validated, though only the first 16 are
// used: a key with garbage in its tail is a corrupt key even though it would
// route perfectly well.
//
// Returns false and leaves `out` untouched on any malformed input. It never
// throws and never aborts, because keys arrive from peers, and invariant I5
// says malformed input from a peer must not be able to end a process.
bool id_from_key(const std::string &hex_key, Id &out);

// A routing identifier on the wire: exactly 16 hexadecimal characters (D-021).
// id_to_hex always writes lowercase and pads with leading zeros, so identifier 1
// is "0000000000000001". id_from_hex accepts either case, rejects any other
// length, and leaves `out` untouched on failure. It never throws (I5).
std::string id_to_hex(Id id);
bool id_from_hex(const std::string &hex16, Id &out);

// The routing identifier of a ring member: SHA-1("ip:port"), truncated.
//
// Two hazards live in that one string, both checked rather than assumed:
//
//  1. The ':' separator must not be able to occur inside the data it separates,
//     or "127.0.0.1" + "90001" and "127.0.0.19" + "0001" would hash alike. For
//     IPv4 that holds -- a dotted quad contains no colon -- and this codebase is
//     IPv4-only (AF_INET / sockaddr_in / inet_pton(AF_INET, ...) throughout).
//     It would NOT hold for IPv6, where "::1" contains the separator itself.
//     Adding IPv6 therefore means switching to the bracket form "[::1]:9001"
//     BEFORE any identifier is computed, not afterwards: identifiers that move
//     are a ring that silently re-partitions.
//
//  2. The identifier is derived from a CANONICAL rendering, so every node must
//     produce the byte-identical string for the same member. "9001" and "09001"
//     are the same port and different identifiers. Callers must therefore parse
//     a membership entry into (ip, int port) and call this function -- never
//     hash a raw line from a file.
Id id_of_address(const std::string &ip, int port);

// Membership tests for an arc travelled CLOCKWISE from `a` round to `b`.
// Both handle the arc that wraps past zero, which is the whole reason they
// exist as named functions rather than as inline comparisons.
//
// Degenerate arcs are deliberate, not accidental:
//   in_range_oc(k, n, n) is TRUE for every k  -- "(n, n] is the whole ring",
//                                                which is how a one-node ring
//                                                says that it owns everything.
//   in_range_oo(k, n, n) is TRUE for every k except n itself.
//
// The first of those is why invariant I8 exists: a node must never ask
// "successor.id == my_id?" to find out whether it is alone, because two nodes
// that collide on an identifier would both conclude they own the entire ring.
// Compare ADDRESSES for that question.
bool in_range_oc(Id k, Id a, Id b);   // (a, b]
bool in_range_oo(Id k, Id a, Id b);   // (a, b)

// finger[i] covers the arc beginning at n + 2^i (mod 2^64).
//
// `i` is ZERO-BASED, so finger[0] starts at n+1 and finger[63] at n + 2^63.
// The Chord paper numbers fingers from 1 and writes the offset 2^(i-1); this
// is the same table with the index shifted, and the difference is written down
// here because an index that is 0-based in one place and 1-based in another is
// a classic source of a silently wrong routing table.
//
// `i` must satisfy 0 <= i < ID_BITS and must NEVER be taken from a peer
// message: it is an internal loop counter, and a value outside that range is a
// programmer error, checked with assert() rather than tolerated.
Id finger_start(Id n, int i);

// ---------------------------------------------------------------------------
// Building a fixed ring (decision D-020).
//
// A node is started with a membership list, computes its routing state from
// it, and lets the list die by scope. These functions are that computation.
// ---------------------------------------------------------------------------

// A ring member as routing state sees it: where it sits and how to reach it.
// This is a routing-table ENTRY, not a node -- D-018 deliberately has no node
// type, because Phase 4's virtual nodes put many identifiers in one process.
struct Peer {
    Id          id = 0;
    std::string ip;        // canonical dotted quad, re-rendered by inet_ntop
    int         port = 0;
};

// Parse a membership list: one "ip:port" per line. Blank lines and lines
// starting with '#' are skipped; whitespace around an entry, including the
// '\r' a Windows editor leaves, is ignored. Addresses are canonicalised before
// hashing, so "127.0.0.1:09001" and "127.0.0.1:9001" are the same member.
//
// On success `out` holds every member SORTED BY IDENTIFIER. On failure `out`
// is untouched and `err` says which line and why. Never throws (invariant I5).
bool parse_members(const std::string &text, std::vector<Peer> &out, std::string &err);

// Sort by identifier and refuse a ring that cannot be routed on: empty, the
// same address twice, or two addresses colliding on one identifier (I8).
// Split out from parse_members so a collision -- which real SHA-1 output will
// never hand a test -- can be exercised with constructed identifiers.
bool validate_ring(std::vector<Peer> &members, std::string &err);

// The owner of key k: the first member whose identifier is >= k, wrapping
// round to the smallest. `sorted` must be non-empty and sorted by identifier.
//
// NOTE the trap: successor_of(my_id) returns the node ITSELF, because the
// bound is inclusive. A node's successor is successor_of(my_id + 1), which is
// exactly finger[0].
//
// Returns BY VALUE on purpose. D-020 destroys the membership list when
// bootstrap returns, so a reference into it would dangle the moment it was
// stored in node state.
Peer successor_of(Id k, const std::vector<Peer> &sorted);

// The member immediately before identifier `id`, wrapping round to the largest.
Peer predecessor_of(Id id, const std::vector<Peer> &sorted);

// finger[i] = successor_of(finger_start(my_id, i)), for i in [0, ID_BITS).
std::vector<Peer> build_fingers(Id my_id, const std::vector<Peer> &sorted);

// ---------------------------------------------------------------------------
// One routing step (step 2.2).
// ---------------------------------------------------------------------------

// The finger that gets closest to k WITHOUT reaching it: the farthest entry
// lying strictly inside the arc (self, k). The scan runs from finger[63] down to
// finger[0], so the first match is the longest jump that does not pass k.
//
// The arc is OPEN at k on purpose. A finger equal to k is k's owner, and jumping
// to it overshoots: that node is not the answer to "who precedes k", and the
// lookup has to travel round to its predecessor. The owner found is still
// correct -- only the hop count grows -- which is exactly the silent hop
// inflation D-019 warned about, so the choice is tested against an oracle
// finger by finger, not only by checking owners.
//
// Degenerate case: k == self.id makes the arc (self, self), which is the whole
// ring except self (see in_range_oo), so the node jumps far instead of walking.
//
// Returns `self` if no finger qualifies.
Peer closest_preceding_finger(const Peer &self, const std::vector<Peer> &fingers, Id k);

// The whole decision a node makes for FIND_SUCCESSOR k (D-012, D-021c):
//   owner == true   ->  `peer` owns k             (k is in (self, successor])
//   owner == false  ->  ask `peer` next           (the closest preceding finger)
//
// Never names `self` as the next hop. With a correct table that cannot happen:
// whenever k is not in (self, successor], the successor itself lies in
// (self, k), so finger[0] always qualifies. With a stale table (Phase 3) it can,
// and naming self would send the originator back to the same node forever -- so
// it falls back to the successor, which always makes progress.
struct RouteStep {
    bool owner = false;
    Peer peer;
};
RouteStep route_step(const Peer &self, const Peer &successor,
                     const std::vector<Peer> &fingers, Id k);

#endif
