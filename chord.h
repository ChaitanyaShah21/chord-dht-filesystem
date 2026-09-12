#ifndef CHORD_H
#define CHORD_H

#include <cstdint>
#include <string>

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

#endif
