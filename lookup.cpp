#include "lookup.h"

#include <arpa/inet.h>

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

// Split on SINGLE spaces. A leading, trailing or doubled space produces an
// empty word, so "OWNER  a b c" is five words and is rejected -- strictness the
// caller wants, because a correct node never sends it.
std::vector<std::string> split_spaces(const std::string &s) {
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
    const std::vector<std::string> words = split_spaces(line);
    if (words.size() != 4) {
        why = "expected 4 words separated by single spaces, got " + std::to_string(words.size());
        return false;
    }
    if (words[0] != "OWNER" && words[0] != "NEXT") {
        why = "first word is neither OWNER nor NEXT";
        return false;
    }

    Peer parsed;
    if (!id_from_hex(words[1], parsed.id)) { why = "identifier is not 16 hexadecimal characters"; return false; }
    if (!parse_reply_ip(words[2], parsed.ip)) { why = "address is not a canonical IPv4 dotted quad"; return false; }
    if (!parse_reply_port(words[3], parsed.port)) { why = "port is not a number in 1..65535"; return false; }

    // Outputs are written only once everything has parsed.
    is_owner = (words[0] == "OWNER");
    peer     = parsed;
    return true;
}

LookupResult lookup(const Peer &start, Id k, const Transport &transport) {
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

Delivery socket_transport(const Peer &to, const std::string &request, std::string &reply) {
    return exchange_line(to.ip, to.port, request, reply, LOOKUP_TIMEOUT_MS);
}
