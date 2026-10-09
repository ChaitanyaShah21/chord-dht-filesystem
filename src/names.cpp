#include "names.h"

#include "lookup.h"     // split_single_spaces: the one strict word splitter
#include "net.h"        // MAX_LINE: the longest reply
#include "store.h"      // valid_key: a manifest key is a chunk key

#include <cstring>
#include <vector>

namespace {

const char *const BAD_NAME = "ERR a name is 1 to 200 visible ASCII characters";
const char *const BAD_KEY  = "ERR a key is 40 lowercase hexadecimal characters";

// One page of names after `after` (from the start if `from_start`), as many as fit
// in one reply line. The count is at most 4 digits: one line holds at most 512
// one-character names.
std::string list_names(const Namespace &ns, const std::string &after, bool from_start) {
    const std::size_t budget = MAX_LINE - std::strlen("NAMES 9999");
    auto it = from_start ? ns.names.begin() : ns.names.upper_bound(after);
    std::string page;
    int count = 0;
    for (; it != ns.names.end(); ++it) {
        if (page.size() + 1 + it->first.size() > budget) break;
        page += " " + it->first;
        ++count;
    }
    return "NAMES " + std::to_string(count) + page;
}

}  // namespace

bool valid_name(const std::string &name) {
    if (name.empty() || name.size() > MAX_NAME) return false;
    // Compared as unsigned bytes: plain char is signed on x86-64 and unsigned on
    // ARM, and a byte of 0x80 or more must be refused on both.
    for (unsigned char c : name)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}

NamesRequest parse_names_request(const std::string &line) {
    NamesRequest r;
    if (line.empty()) {
        r.error = "ERR empty request";
        return r;
    }
    const std::vector<std::string> w = split_single_spaces(line);
    const std::string &verb = w[0];

    if (verb == "PING" && w.size() == 1) {
        r.kind = NamesRequest::Kind::Ping;
    } else if (verb == "REGISTER") {
        if (w.size() != 3) r.error = "ERR usage: REGISTER <name> <manifest-key>";
        else if (!valid_name(w[1])) r.error = BAD_NAME;
        else if (!valid_key(w[2])) r.error = BAD_KEY;
        else {
            r.kind = NamesRequest::Kind::Register;
            r.name = w[1];
            r.key = w[2];
        }
    } else if (verb == "RESOLVE") {
        if (w.size() != 2) r.error = "ERR usage: RESOLVE <name>";
        else if (!valid_name(w[1])) r.error = BAD_NAME;
        else {
            r.kind = NamesRequest::Kind::Resolve;
            r.name = w[1];
        }
    } else if (verb == "LIST") {
        if (w.size() > 2) r.error = "ERR usage: LIST [<after>]";
        else if (w.size() == 2 && !valid_name(w[1])) r.error = BAD_NAME;
        else {
            r.kind = NamesRequest::Kind::List;
            r.from_start = w.size() == 1;
            if (w.size() == 2) r.after = w[1];
        }
    } else {
        r.error = "ERR unknown request";
    }
    return r;
}

std::string register_command(const std::string &name, const std::string &key) { return "REGISTER " + name + " " + key; }

bool valid_command(const std::string &command) {
    if (command == "NOOP") return true;
    const std::vector<std::string> w = split_single_spaces(command);
    return w.size() == 3 && w[0] == "REGISTER" && valid_name(w[1]) && valid_key(w[2]);
}

// The state change itself, and nothing else -- D-009's split, kept: every replica
// applies every committed entry through this one function, in log order, so they
// cannot disagree about who holds a name.
std::string apply_command(Namespace &ns, const std::string &command) {
    if (!valid_command(command) || command == "NOOP") return "";
    const std::vector<std::string> w = split_single_spaces(command);
    const auto it = ns.names.find(w[1]);
    if (it == ns.names.end()) {
        ns.names.emplace(w[1], w[2]);
        return "OK";
    }
    // Written once (D-059): the same key again is a retry and succeeds; a different
    // key is refused, and the state does not change either way.
    return it->second == w[2] ? "OK" : "EXISTS " + it->second;
}

std::string answer_read(const Namespace &ns, const NamesRequest &req) {
    if (req.kind == NamesRequest::Kind::Resolve) {
        const auto it = ns.names.find(req.name);
        return it == ns.names.end() ? "MISSING" : "FILE " + it->second;
    }
    if (req.kind == NamesRequest::Kind::List) return list_names(ns, req.after, req.from_start);
    return "ERR not a read";
}
