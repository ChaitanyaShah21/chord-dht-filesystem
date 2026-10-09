#ifndef NAMES_H
#define NAMES_H

#include <cstddef>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// The namespace (decisions D-017, D-058, D-059): file name -> manifest key, and
// nothing else. Since D-080 it is the state machine a Raft group replicates: it
// keeps no log and no lock of its own -- the Raft log is its durability (D-074),
// and the replica's lock protects it (D-072). Nothing here opens a socket.
//
//   REGISTER <name> <manifest-key>  -> OK | EXISTS <key> | ERR <reason>
//   RESOLVE <name>                  -> FILE <key> | MISSING | ERR <reason>
//   LIST [<after>]                  -> NAMES <n> <name> ...   (sorted; one line; NAMES 0 = no more)
//   PING                            -> PONG
//
// (and, from a replica that cannot answer, REDIRECT <ip:port> or UNAVAILABLE
// <reason>: replica.cpp, D-081).
//
// A name is written once (D-059): registering a taken name with the same key
// answers OK and changes nothing; with a different key, EXISTS. Which registration
// takes a name is decided by the order the log applies them in (D-075).
//
// The log entries this state machine applies:
//
//   REGISTER <name> <key>     the first one applied for a name wins
//   NOOP                      a new leader's first entry; changes nothing
// ---------------------------------------------------------------------------

// The longest name, in bytes. With a 40-character key and the verb, a REGISTER
// line stays far inside the 1,024-byte line limit (net.h's MAX_LINE).
constexpr std::size_t MAX_NAME = 200;

// 1 to MAX_NAME bytes, each a visible ASCII character (0x21 to 0x7e): no space,
// so a name is one word on the wire and in the log, and no control character, so
// it cannot carry a newline. A name is never used as a path.
bool valid_name(const std::string &name);

struct Namespace {
    std::map<std::string, std::string> names;      // name -> key, sorted by name for LIST
};

// A client's request line, parsed and checked.
struct NamesRequest {
    enum class Kind { Register, Resolve, List, Ping, Invalid };
    Kind kind = Kind::Invalid;
    std::string name, key;       // REGISTER, RESOLVE
    std::string after;           // LIST <after>
    bool from_start = true;      // LIST with no <after>
    std::string error;           // Invalid: the one-line ERR reply
};

// Never throws on anything a client sends (I5).
NamesRequest parse_names_request(const std::string &line);

// The log entry a registration becomes.
std::string register_command(const std::string &name, const std::string &key);

// "NOOP", or "REGISTER <valid name> <valid key>" single-spaced: the only commands a
// replica proposes, applies, or accepts from a leader.
bool valid_command(const std::string &command);

// Apply one committed entry. Returns what a client waiting on it is told -- "OK" or
// "EXISTS <key>" -- or "" for a NOOP. A command that is not valid changes nothing.
std::string apply_command(Namespace &ns, const std::string &command);

// Answer a RESOLVE or LIST from the state as applied so far.
std::string answer_read(const Namespace &ns, const NamesRequest &req);

#endif
