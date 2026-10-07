// lookup -- the originator as a command-line tool (decision D-024).
//
//   ./lookup <start-ip> <start-port>  < identifiers
//
// Reads 16-hexadecimal-character routing identifiers from standard input, one
// per line, and looks each one up starting at the given node. One process runs
// many lookups, so a benchmark sweep does not start a process per lookup.
//
// Prints exactly one line per non-blank input line:
//
//   <key> OWNER <owner-id> <ip>:<port> requests <r> hops <h> path <ip:port>,<ip:port>,...
//   <key> ERROR <kind> requests <r> hops <h> path <ip:port>,... : <message>
//
// `requests` counts FIND_SUCCESSOR messages sent; `hops` counts NEXT replies
// followed, which is the Chord paper's path length (decision D-026). An input
// line that is not an identifier is reported as "? ERROR bad-input ...".
// A failed lookup never stops the run.
//
// Exit status: 0 if every lookup succeeded, 1 if any failed, 2 on a usage error.

#include "chord.h"
#include "lookup.h"

#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

// The path as "ip:port,ip:port,...", or "-" if no request was sent.
std::string render_path(const std::vector<Peer> &path) {
    if (path.empty()) return "-";
    std::string out;
    for (const Peer &p : path) {
        if (!out.empty()) out += ',';
        out += p.ip + ":" + std::to_string(p.port);
    }
    return out;
}

// Strip surrounding spaces, tabs and the '\r' a Windows editor leaves.
std::string trim(const std::string &s) {
    const char *space = " \t\r";
    const std::size_t begin = s.find_first_not_of(space);
    if (begin == std::string::npos) return "";
    return s.substr(begin, s.find_last_not_of(space) - begin + 1);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <start-ip> <start-port>  < identifiers\n", argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);      // D-011: a departed node is an error value

    // The start node goes through the same parser as a membership file, so its
    // address is validated and canonicalised and its identifier computed exactly
    // as every node computes it.
    std::vector<Peer> start;
    std::string err;
    if (!parse_members(std::string(argv[1]) + ":" + argv[2], start, err)) {
        std::fprintf(stderr, "lookup: invalid start node %s:%s\n", argv[1], argv[2]);
        return 2;
    }

    bool any_failed = false;
    std::string line;
    int line_no = 0;
    while (std::getline(std::cin, line)) {
        ++line_no;
        const std::string key_text = trim(line);
        if (key_text.empty()) continue;

        Id k;
        if (!id_from_hex(key_text, k)) {
            std::printf("? ERROR bad-input requests 0 hops 0 path - : line %d is not a "
                        "16-hexadecimal-character identifier\n", line_no);
            any_failed = true;
            continue;
        }

        const LookupResult r = lookup(start[0], k, socket_transport);
        const std::string path = render_path(r.path);
        if (r.ok()) {
            std::printf("%s OWNER %s %s:%d requests %d hops %d path %s\n",
                        id_to_hex(k).c_str(), id_to_hex(r.owner.id).c_str(), r.owner.ip.c_str(),
                        r.owner.port, r.requests(), r.hops(), path.c_str());
        } else {
            std::printf("%s ERROR %s requests %d hops %d path %s : %s\n",
                        id_to_hex(k).c_str(), lookup_error_name(r.error), r.requests(), r.hops(),
                        path.c_str(), r.message.c_str());
            any_failed = true;
        }
    }

    std::fflush(stdout);
    return any_failed ? 1 : 0;
}
