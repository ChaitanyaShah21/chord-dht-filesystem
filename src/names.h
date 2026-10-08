#ifndef NAMES_H
#define NAMES_H

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// The namespace (decisions D-017, D-058, D-059): file name -> manifest key, and
// nothing else. This is all the tracker knows. Like stabilize.cpp and chunk.cpp,
// nothing here opens a socket: tracker.cpp reads request lines and passes them
// in, so the tests drive the namespace directly, log file included.
//
//   REGISTER <name> <manifest-key>  -> OK | EXISTS <key> | ERR <reason>
//   RESOLVE <name>                  -> FILE <key> | MISSING | ERR <reason>
//   LIST [<after>]                  -> NAMES <n> <name> ...   (sorted; one line; NAMES 0 = no more)
//   PING                            -> PONG
//
// A name is written once (D-059): registering a taken name with the same key
// answers OK and changes nothing; with a different key, EXISTS.
//
// The log (D-058) is one line per registration, appended and flushed BEFORE the
// answer OK is sent:
//
//   REGISTER <name> <key> <check>
//
// where <check> is the first 8 hexadecimal characters of SHA-1 of everything
// before " <check>". A record torn by a crash, or damaged on disk, fails its
// check (or lacks its newline) and is skipped at start-up instead of being
// replayed as something else.
// ---------------------------------------------------------------------------

// The longest name, in bytes. With a 40-character key and the verb, a REGISTER
// line stays far inside the 1,024-byte line limit (net.h's MAX_LINE).
constexpr std::size_t MAX_NAME = 200;

// 1 to MAX_NAME bytes, each a visible ASCII character (0x21 to 0x7e): no space,
// so a name is one word on the wire and one field in the log, and no control
// character, so it cannot carry a newline. A name is never used as a path.
bool valid_name(const std::string &name);

struct Namespace {
    mutable std::mutex mu;                         // guards everything below
    std::map<std::string, std::string> names;      // name -> key, sorted by name for LIST
    int log_fd = -1;                               // the open log, appended to under `mu`
    bool log_broken = false;                       // a failed append could not be undone
    std::string log_path;
};

// One log record for this registration, without its newline.
std::string names_record(const std::string &name, const std::string &key);

// Parse one log line (without its newline). False if it is malformed or its
// check does not match; `name` and `key` are then not to be used.
bool parse_record(const std::string &line, std::string &name, std::string &key);

// Open the log at `path`, creating it if it does not exist, and replay every
// record into `ns`. A final line with no newline (torn by a crash mid-append)
// is reported and cut off the file, so the next record starts on a line of its
// own; any other line that fails to parse is reported and skipped. `report`
// says what was found. Call once, before any other thread uses `ns`.
bool names_open(Namespace &ns, const std::string &path, std::string &report, std::string &err);

// Answer one request line. Never throws on anything a client sends (I5).
std::string handle_names_request(Namespace &ns, const std::string &line);

#endif
