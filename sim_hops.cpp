// sim-hops -- the hop-count experiment with the ring in one process (decision D-027).
//
//   ./sim-hops <membership-file>  < workload
//
// Each workload line is "<start-ip>:<start-port> <16-hex identifier>". The
// output is exactly the format ./lookup prints, one line per workload line:
//
//   <key> OWNER <owner-id> <ip>:<port> requests <r> hops <h> path <ip:port>,...
//   <key> ERROR <kind> requests <r> hops <h> path ... : <message>
//
// Nothing here is a second implementation of routing. The membership file is
// parsed by parse_members, each node's table is built by build_fingers and its
// answer decided by route_step -- exactly as node.cpp does -- and the loop is
// lookup() from lookup.cpp, the same function the ./lookup tool runs. Only the
// transport differs: a function call instead of a TCP connection. So a ring
// that fits in memory as real processes can be replayed here and must give the
// same answer for every lookup, node for node; and a ring far too large to run
// as processes can still be measured.
//
// Memory is the reason it exists, so it holds only the sorted membership (about
// 50 bytes a node) and builds a node's 64 fingers when that node is asked,
// instead of keeping 64 fingers for every node (about 3 KB a node, 3 GB at 10^6).

#include "chord.h"
#include "lookup.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string render_path(const std::vector<Peer> &path) {
    if (path.empty()) return "-";
    std::string out;
    for (const Peer &p : path) {
        if (!out.empty()) out += ',';
        out += p.ip + ":" + std::to_string(p.port);
    }
    return out;
}

// One ring member answering FIND_SUCCESSOR, the way node.cpp's handle() does.
// `members` is sorted by identifier; a node is found by binary search on its
// identifier and must also match by address (invariant I8).
Delivery answer(const std::vector<Peer> &members, const Peer &to, const std::string &request,
                std::string &reply) {
    auto it = std::lower_bound(members.begin(), members.end(), to.id,
                               [](const Peer &p, Id id) { return p.id < id; });
    if (it == members.end() || it->id != to.id || it->ip != to.ip || it->port != to.port)
        return Delivery::Refused;                    // nobody is at that address
    const Peer &self = *it;

    Id k;
    if (request.compare(0, 15, "FIND_SUCCESSOR ") != 0 || !id_from_hex(request.substr(15), k)) {
        reply = "ERR unknown request";
        return Delivery::Ok;
    }

    // The node's routing state, built exactly as node.cpp's bootstrap builds it.
    const std::vector<Peer> fingers = build_fingers(self.id, members);
    const RouteStep step = route_step(self, fingers[0], fingers, k);
    reply = std::string(step.owner ? "OWNER " : "NEXT ") + id_to_hex(step.peer.id) + " " +
            step.peer.ip + " " + std::to_string(step.peer.port);
    return Delivery::Ok;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <membership-file>  < workload\n", argv[0]);
        return 2;
    }

    std::ifstream file(argv[1]);
    if (!file) { std::fprintf(stderr, "sim-hops: cannot open %s\n", argv[1]); return 2; }
    std::stringstream text;
    text << file.rdbuf();

    std::vector<Peer> members;
    std::string err;
    if (!parse_members(text.str(), members, err)) {
        std::fprintf(stderr, "sim-hops: %s\n", err.c_str());
        return 2;
    }

    // The transport captures `members` by reference. That is safe only because
    // lookup() never stores a Transport (the rule written in lookup.h), and
    // `members` outlives every call below.
    const Transport in_process = [&members](const Peer &to, const std::string &q, std::string &r) {
        return answer(members, to, q, r);
    };

    bool any_failed = false;
    std::string line;
    int line_no = 0;
    while (std::getline(std::cin, line)) {
        ++line_no;
        std::istringstream words(line);
        std::string start_text, key_text, extra;
        if (!(words >> start_text)) continue;                // a blank line: no output

        // Every other line produces exactly one output line, even when it is
        // malformed -- a harness pairs inputs with outputs by position, so a
        // silently skipped line would shift every result after it.
        std::vector<Peer> start;
        std::string start_err;
        Id k;
        if (!(words >> key_text) || (words >> extra) ||
            !parse_members(start_text, start, start_err) || !id_from_hex(key_text, k)) {
            std::printf("? ERROR bad-input requests 0 hops 0 path - : line %d is not "
                        "\"<ip>:<port> <16-hex identifier>\"\n", line_no);
            any_failed = true;
            continue;
        }

        const LookupResult r = lookup(start[0], k, in_process);
        const std::string path = render_path(r.path);
        if (r.ok()) {
            std::printf("%s OWNER %s %s:%d requests %d hops %d path %s\n", id_to_hex(k).c_str(),
                        id_to_hex(r.owner.id).c_str(), r.owner.ip.c_str(), r.owner.port, r.requests(),
                        r.hops(), path.c_str());
        } else {
            std::printf("%s ERROR %s requests %d hops %d path %s : %s\n", id_to_hex(k).c_str(),
                        lookup_error_name(r.error), r.requests(), r.hops(), path.c_str(), r.message.c_str());
            any_failed = true;
        }
    }

    std::fflush(stdout);
    return any_failed ? 1 : 0;
}
