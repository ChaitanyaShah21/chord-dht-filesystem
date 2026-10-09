#include "names.h"

#include "lookup.h"     // split_single_spaces: the one strict word splitter
#include "net.h"        // MAX_LINE: the longest reply
#include "store.h"      // valid_key and chunk_key: a manifest key is a chunk key; an owner hash is a SHA-1 too

#include <cstring>
#include <vector>

namespace {

const char *const BAD_NAME   = "ERR a name is 1 to 200 visible ASCII characters";
const char *const BAD_KEY    = "ERR a key is 40 lowercase hexadecimal characters";
const char *const BAD_OWNER  = "ERR an owner hash is 40 lowercase hexadecimal characters";
const char *const BAD_SECRET = "ERR a secret is 32 to 64 lowercase hexadecimal characters";

// One page after `after` (from the start if `from_start`), as many entries as fit
// in one reply line. `word` writes one entry: " <name>" for LIST, " <name> <key>"
// for ENTRIES. The count is at most 4 digits: one line holds at most 512 entries.
template <typename Word>
std::string page(const Namespace &ns, const char *verb, const std::string &after, bool from_start, Word word) {
    const std::size_t budget = MAX_LINE - std::strlen(verb) - std::strlen(" 9999");
    auto it = from_start ? ns.names.begin() : ns.names.upper_bound(after);
    std::string text;
    int count = 0;
    for (; it != ns.names.end(); ++it) {
        const std::string w = word(*it);
        if (text.size() + w.size() > budget) break;
        text += w;
        ++count;
    }
    return std::string(verb) + " " + std::to_string(count) + text;
}

// "<verb>" or "<verb> <after>", as LIST and ENTRIES take them.
bool parse_page(const std::vector<std::string> &w, NamesRequest &r, NamesRequest::Kind kind, const char *usage) {
    if (w.size() > 2) r.error = usage;
    else if (w.size() == 2 && !valid_name(w[1])) r.error = BAD_NAME;
    else {
        r.kind = kind;
        r.from_start = w.size() == 1;
        if (w.size() == 2) r.after = w[1];
        return true;
    }
    return false;
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

bool valid_secret(const std::string &secret) {
    if (secret.size() < 32 || secret.size() > 64) return false;
    for (char c : secret)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string owner_hash(const std::string &secret) { return chunk_key(secret); }

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
        if (w.size() != 3 && w.size() != 4) r.error = "ERR usage: REGISTER <name> <manifest-key> [<owner-hash>]";
        else if (!valid_name(w[1])) r.error = BAD_NAME;
        else if (!valid_key(w[2])) r.error = BAD_KEY;
        else if (w.size() == 4 && !valid_key(w[3])) r.error = BAD_OWNER;
        else {
            r.kind = NamesRequest::Kind::Register;
            r.name = w[1];
            r.key = w[2];
            if (w.size() == 4) r.owner = w[3];
        }
    } else if (verb == "UNREGISTER") {
        if (w.size() != 3) r.error = "ERR usage: UNREGISTER <name> <secret>";
        else if (!valid_name(w[1])) r.error = BAD_NAME;
        else if (!valid_secret(w[2])) r.error = BAD_SECRET;
        else {
            r.kind = NamesRequest::Kind::Unregister;
            r.name = w[1];
            r.secret = w[2];
        }
    } else if (verb == "RESOLVE") {
        if (w.size() != 2) r.error = "ERR usage: RESOLVE <name>";
        else if (!valid_name(w[1])) r.error = BAD_NAME;
        else {
            r.kind = NamesRequest::Kind::Resolve;
            r.name = w[1];
        }
    } else if (verb == "LIST") {
        parse_page(w, r, NamesRequest::Kind::List, "ERR usage: LIST [<after>]");
    } else if (verb == "ENTRIES") {
        parse_page(w, r, NamesRequest::Kind::Entries, "ERR usage: ENTRIES [<after>]");
    } else {
        r.error = "ERR unknown request";
    }
    return r;
}

std::string register_command(const std::string &name, const std::string &key, const std::string &owner) {
    return "REGISTER " + name + " " + key + (owner.empty() ? "" : " " + owner);
}

std::string unregister_command(const std::string &name, const std::string &owner) {
    return "UNREGISTER " + name + " " + owner;
}

bool valid_command(const std::string &command) {
    if (command == "NOOP") return true;
    const std::vector<std::string> w = split_single_spaces(command);
    if (w[0] == "REGISTER")
        return (w.size() == 3 || w.size() == 4) && valid_name(w[1]) && valid_key(w[2]) && (w.size() == 3 || valid_key(w[3]));
    if (w[0] == "UNREGISTER") return w.size() == 3 && valid_name(w[1]) && valid_key(w[2]);
    return false;
}

// The state change itself, and nothing else -- D-009's split, kept: every replica
// applies every committed entry through this one function, in log order, so they
// cannot disagree about who holds a name.
std::string apply_command(Namespace &ns, const std::string &command) {
    if (!valid_command(command) || command == "NOOP") return "";
    const std::vector<std::string> w = split_single_spaces(command);
    const auto it = ns.names.find(w[1]);

    if (w[0] == "UNREGISTER") {
        if (it == ns.names.end()) return "MISSING";
        // Only the owner, proved by the secret's hash; a name with no owner never.
        if (it->second.owner.empty() || it->second.owner != w[2]) return "DENIED";
        ns.names.erase(it);
        return "OK";
    }

    const std::string owner = w.size() == 4 ? w[3] : "";
    if (it == ns.names.end()) {
        ns.names.emplace(w[1], NameEntry{w[2], owner});
        return "OK";
    }
    // Written once per owner (D-059, D-089): the same key by the same owner again is a
    // retry and succeeds; anything else is refused, and the state does not change.
    return it->second.key == w[2] && it->second.owner == owner ? "OK" : "EXISTS " + it->second.key;
}

std::string answer_read(const Namespace &ns, const NamesRequest &req) {
    if (req.kind == NamesRequest::Kind::Resolve) {
        const auto it = ns.names.find(req.name);
        return it == ns.names.end() ? "MISSING" : "FILE " + it->second.key;
    }
    using Item = std::pair<const std::string, NameEntry>;
    if (req.kind == NamesRequest::Kind::List)
        return page(ns, "NAMES", req.after, req.from_start, [](const Item &e) { return " " + e.first; });
    if (req.kind == NamesRequest::Kind::Entries)
        return page(ns, "ENTRIES", req.after, req.from_start, [](const Item &e) { return " " + e.first + " " + e.second.key; });
    return "ERR not a read";
}
