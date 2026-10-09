#include "lookup.h"
#include "log.h"

#include <arpa/inet.h>

#include <chrono>
#include <cstddef>

namespace {

// How a node is named in an error message: its address, which is what an
// operator can act on, and its identifier, which is what routing used.
std::string describe(const Peer &p) {
    return p.ip + ":" + std::to_string(p.port) + " (id " + id_to_hex(p.id) + ")";
}

// A reply quoted inside an error message. Replies come from peers, so they are
// shortened and any byte that is not printable ASCII becomes '?': a hostile
// reply must not be able to put a newline or a terminal escape into our output.
std::string shown(const std::string &reply) {
    constexpr std::size_t LIMIT = 80;
    std::string out = "\"";
    for (std::size_t i = 0; i < reply.size() && i < LIMIT; ++i) {
        const unsigned char c = static_cast<unsigned char>(reply[i]);
        out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    if (reply.size() > LIMIT) out += "...";
    return out + "\"";
}

// A port exactly as the node renders it: 1..65535, digits only, no leading zero.
bool parse_reply_port(const std::string &s, int &out) {
    if (s.empty() || s.size() > 5 || s[0] == '0') return false;
    int value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value > 65535) return false;
    out = value;
    return true;
}

// A dotted-quad IPv4 address in canonical form: it must survive a round trip
// through inet_pton and inet_ntop unchanged. That rejects names, IPv6, octets
// above 255, and any embedded NUL -- inet_pton reads a C string and would stop
// at the NUL, but the round-tripped string would then be shorter than the input.
bool parse_reply_ip(const std::string &s, std::string &out) {
    in_addr addr{};
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1) return false;
    char canonical[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr, canonical, sizeof canonical) == nullptr) return false;
    if (s != canonical) return false;
    out = canonical;
    return true;
}

LookupError error_of(Delivery d) {
    switch (d) {
        case Delivery::Refused: return LookupError::Refused;
        case Delivery::Timeout: return LookupError::Timeout;
        case Delivery::Closed:  return LookupError::Closed;
        default:                return LookupError::Unreachable;
    }
}

std::string transport_message(Delivery d, const Peer &node) {
    switch (d) {
        case Delivery::Refused: return "connection to " + describe(node) + " refused: nothing is listening";
        case Delivery::Timeout: return describe(node) + " did not answer in time";
        case Delivery::Closed:  return describe(node) + " closed the connection before replying";
        default:                return "could not exchange a message with " + describe(node);
    }
}

LookupResult fail(LookupResult r, LookupError e, const std::string &message) {
    r.error   = e;
    r.message = message;
    return r;
}

}  // namespace

// "OWNER  a b c" is five words here and is rejected -- strictness the caller
// wants, because a correct node never sends a doubled space.
std::vector<std::string> split_single_spaces(const std::string &s) {
    std::vector<std::string> words;
    std::size_t begin = 0;
    while (true) {
        const std::size_t space = s.find(' ', begin);
        if (space == std::string::npos) {
            words.push_back(s.substr(begin));
            return words;
        }
        words.push_back(s.substr(begin, space - begin));
        begin = space + 1;
    }
}

std::string peer_words(const Peer &p) {
    return id_to_hex(p.id) + " " + p.ip + " " + std::to_string(p.port);
}

bool parse_peer_words(const std::string &id, const std::string &ip, const std::string &port,
                      Peer &out, std::string &why) {
    Peer parsed;
    if (!id_from_hex(id, parsed.id)) { why = "identifier is not 16 hexadecimal characters"; return false; }
    if (!parse_reply_ip(ip, parsed.ip)) { why = "address is not a canonical IPv4 dotted quad"; return false; }
    if (!parse_reply_port(port, parsed.port)) { why = "port is not a number in 1..65535"; return false; }
    out = parsed;                      // written only once all three have parsed
    return true;
}

const char *lookup_error_name(LookupError e) {
    switch (e) {
        case LookupError::None:            return "none";
        case LookupError::Refused:         return "refused";
        case LookupError::Timeout:         return "timeout";
        case LookupError::Closed:          return "closed";
        case LookupError::Unreachable:     return "unreachable";
        case LookupError::ErrReply:        return "err-reply";
        case LookupError::Malformed:       return "malformed";
        case LookupError::NoProgress:      return "no-progress";
        case LookupError::TooManyRequests: return "too-many-requests";
    }
    return "unknown";
}

bool parse_reply(const std::string &line, bool &is_owner, Peer &peer, std::string &why) {
    const std::vector<std::string> words = split_single_spaces(line);
    if (words.size() != 4) {
        why = "expected 4 words separated by single spaces, got " + std::to_string(words.size());
        return false;
    }
    if (words[0] != "OWNER" && words[0] != "NEXT") {
        why = "first word is neither OWNER nor NEXT";
        return false;
    }

    Peer parsed;
    if (!parse_peer_words(words[1], words[2], words[3], parsed, why)) return false;

    // Outputs are written only once everything has parsed.
    is_owner = (words[0] == "OWNER");
    peer     = parsed;
    return true;
}

namespace {

// Set once by set_lookup_observer, before any other thread starts; only read after.
LookupObserver g_observer = nullptr;

LookupResult run_lookup(const Peer &start, Id k, const Transport &transport);

}  // namespace

void set_lookup_observer(LookupObserver observer) { g_observer = observer; }

LookupResult lookup(const Peer &start, Id k, const Transport &transport) {
    const auto t0 = std::chrono::steady_clock::now();
    LookupResult r = run_lookup(start, k, transport);
    if (g_observer) g_observer(r, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return r;
}

namespace {

LookupResult run_lookup(const Peer &start, Id k, const Transport &transport) {
    LookupResult r;
    if (!transport) return fail(r, LookupError::Unreachable, "no transport supplied");

    // The request is the same at every hop: iterative routing asks each node
    // the same question, and only the node being asked changes.
    const std::string request = "FIND_SUCCESSOR " + id_to_hex(k);
    Peer current = start;

    for (int sent = 0; sent < MAX_REQUESTS; ++sent) {
        // Recorded BEFORE asking, so a failure leaves the failing node last.
        r.path.push_back(current);

        std::string reply;
        const Delivery d = transport(current, request, reply);
        if (d != Delivery::Ok) return fail(r, error_of(d), transport_message(d, current));

        // ERR is the node refusing the question -- a different fault from a
        // reply that cannot be parsed, so it gets its own error.
        if (reply == "ERR" || reply.compare(0, 4, "ERR ") == 0)
            return fail(r, LookupError::ErrReply, describe(current) + " answered " + shown(reply));

        bool is_owner = false;
        Peer named;
        std::string why;
        if (!parse_reply(reply, is_owner, named, why))
            return fail(r, LookupError::Malformed,
                        describe(current) + " sent a malformed reply (" + why + "): " + shown(reply));

        // The progress check (D-024, F19d). The OWNER claim must cover k...
        if (is_owner) {
            if (!in_range_oc(k, current.id, named.id))
                return fail(r, LookupError::NoProgress,
                            describe(current) + " named " + describe(named) +
                            " as owner, but the key is not in (" + id_to_hex(current.id) + ", " +
                            id_to_hex(named.id) + "]");
            r.owner = named;
            return r;
        }

        // ...and a NEXT must land strictly between the node asked and k:
        // not backwards, not on itself, not on k, not past k.
        if (!in_range_oo(named.id, current.id, k))
            return fail(r, LookupError::NoProgress,
                        describe(current) + " sent the lookup to " + describe(named) +
                        ", which is not strictly inside (" + id_to_hex(current.id) + ", " +
                        id_to_hex(k) + ")");
        current = named;
    }

    return fail(r, LookupError::TooManyRequests,
                "no owner after " + std::to_string(MAX_REQUESTS) + " requests; the last NEXT came from " +
                describe(r.path.back()));
}

}  // namespace

Delivery socket_transport(const Peer &to, const std::string &request, std::string &reply) {
    // The request id, if this thread or process has one, goes first (D-087).
    return exchange_line(to.ip, to.port, with_request_id(addressed(to, request)), reply, LOOKUP_TIMEOUT_MS);
}

std::string addressed(const Peer &to, const std::string &request) {
    return "TO " + id_to_hex(to.id) + " " + request;
}

Target split_target(const std::string &line, Id &id, std::string &rest) {
    if (line.compare(0, 3, "TO ") != 0) { rest = line; return Target::None; }
    // "TO " + 16 hex + " " + at least one character of request = 21 characters minimum.
    if (line.size() < 21 || line[19] != ' ' || !id_from_hex(line.substr(3, 16), id)) return Target::Malformed;
    rest = line.substr(20);
    return Target::Member;
}

LookupResult lookup_with_recovery(const Peer &start, Id k, const Transport &transport, int retries) {
    LookupResult r = lookup(start, k, transport);
    for (int attempt = 0; attempt < retries && !r.ok(); ++attempt) {
        // Only a node that could not be REACHED is worth reporting. A node that
        // answered with nonsense was reached; that is a different fault.
        const bool unreachable = r.error == LookupError::Refused || r.error == LookupError::Timeout ||
                                 r.error == LookupError::Closed;
        // path.back() failed; path[size - 2] is the node that named it. With one
        // entry the start node itself failed, and nobody named it.
        if (!unreachable || r.path.size() < 2) break;

        const Peer &dead  = r.path[r.path.size() - 1];
        const Peer &namer = r.path[r.path.size() - 2];
        std::string ignored;
        transport(namer, "SUSPECT " + dead.ip + " " + std::to_string(dead.port), ignored);

        const int attempts = r.attempts + 1;
        r = lookup(start, k, transport);
        r.attempts = attempts;
    }
    return r;
}
