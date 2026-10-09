#ifndef NAMES_H
#define NAMES_H

#include <cstddef>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// The namespace (decisions D-017, D-058, D-059, D-089): file name -> manifest key,
// and who may delete it. Since D-080 it is the state machine a Raft group
// replicates: it keeps no log and no lock of its own -- the Raft log is its
// durability (D-074), and the replica's lock protects it (D-072). Nothing here
// opens a socket.
//
//   REGISTER <name> <manifest-key> [<owner-hash>]  -> OK | EXISTS <key> | ERR <reason>
//   UNREGISTER <name> <secret>                     -> OK | MISSING | DENIED | ERR <reason>
//   RESOLVE <name>                                 -> FILE <key> | MISSING | ERR <reason>
//   LIST [<after>]                                 -> NAMES <n> <name> ...          (sorted; NAMES 0 = no more)
//   ENTRIES [<after>]                              -> ENTRIES <n> <name> <key> ...  (the same, with keys)
//   PING                                           -> PONG
//
// (and, from a replica that cannot answer, REDIRECT <ip:port> or UNAVAILABLE
// <reason>: replica.cpp, D-081).
//
// Owners (D-089). The uploader chooses a secret and registers only its SHA-1, the
// owner hash. UNREGISTER carries the secret itself; the replica hashes it before
// anything is logged, so no secret is ever in the Raft log. A name registered with
// no owner hash -- the original three-word REGISTER -- can never be deleted.
//
// A name is written once per owner (D-059, refined by D-089): registering a taken
// name with the same key and the same owner answers OK and changes nothing (a
// retry); anything else answers EXISTS. Once deleted, the name is free again.
// Which request takes effect is decided by the order the log applies them in (D-075).
//
// The log entries this state machine applies:
//
//   REGISTER <name> <key>              a name with no owner: never deletable
//   REGISTER <name> <key> <owner>      a name its owner may delete
//   UNREGISTER <name> <owner>          deleted if <owner> is the name's owner hash
//   NOOP                               a new leader's first entry; changes nothing
// ---------------------------------------------------------------------------

// The longest name, in bytes. With a 40-character key, a 40-character owner hash
// and the verb, a REGISTER line stays far inside the 1,024-byte limit (MAX_LINE).
constexpr std::size_t MAX_NAME = 200;

// 1 to MAX_NAME bytes, each a visible ASCII character (0x21 to 0x7e): no space,
// so a name is one word on the wire and in the log, and no control character, so
// it cannot carry a newline. A name is never used as a path.
bool valid_name(const std::string &name);

// An owner's secret: 32 to 64 lowercase hexadecimal characters -- at least 128
// random bits, written so it is one word.
bool valid_secret(const std::string &secret);

// The owner hash of a secret: its SHA-1, as 40 lowercase hexadecimal characters.
std::string owner_hash(const std::string &secret);

struct NameEntry {
    std::string key;           // the root manifest's key
    std::string owner;         // SHA-1 of the owner's secret; empty: no owner, never deletable
};
inline bool operator==(const NameEntry &a, const NameEntry &b) { return a.key == b.key && a.owner == b.owner; }

struct Namespace {
    std::map<std::string, NameEntry> names;     // sorted by name, for LIST and ENTRIES
};

// A client's request line, parsed and checked.
struct NamesRequest {
    enum class Kind { Register, Unregister, Resolve, List, Entries, Ping, Invalid };
    Kind kind = Kind::Invalid;
    std::string name, key;       // REGISTER, UNREGISTER, RESOLVE
    std::string owner;           // REGISTER: the owner hash, or empty
    std::string secret;          // UNREGISTER: the secret, hashed before it goes anywhere
    std::string after;           // LIST <after>, ENTRIES <after>
    bool from_start = true;      // LIST or ENTRIES with no <after>
    std::string error;           // Invalid: the one-line ERR reply
};

// Never throws on anything a client sends (I5).
NamesRequest parse_names_request(const std::string &line);

// The log entries a registration and a deletion become. An empty owner gives the
// three-word form.
std::string register_command(const std::string &name, const std::string &key, const std::string &owner = "");
std::string unregister_command(const std::string &name, const std::string &owner);

// "NOOP", or one of the REGISTER and UNREGISTER forms above, single-spaced, with
// valid names, keys and hashes: the only commands a replica proposes, applies, or
// accepts from a leader.
bool valid_command(const std::string &command);

// Apply one committed entry. Returns what a client waiting on it is told -- "OK",
// "EXISTS <key>", "MISSING" or "DENIED" -- or "" for a NOOP. A command that is not
// valid changes nothing.
std::string apply_command(Namespace &ns, const std::string &command);

// Answer a RESOLVE, LIST or ENTRIES from the state as applied so far.
std::string answer_read(const Namespace &ns, const NamesRequest &req);

#endif
