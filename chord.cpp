#include "chord.h"
#include "sha1.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <utility>

namespace {

// One hexadecimal character to its value in 0..15. Returns false for anything
// that is not a hexadecimal digit, including '\0' and whitespace.
bool hex_nibble(char c, unsigned &out) {
    if (c >= '0' && c <= '9') { out = static_cast<unsigned>(c - '0');      return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<unsigned>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<unsigned>(c - 'A' + 10); return true; }
    return false;
}

// Strip leading and trailing whitespace, including '\r'.
// The cast matters: std::isspace on a negative char -- any byte above 127 on a
// platform where char is signed -- is undefined behaviour.
std::string trim(const std::string &s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))  ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

// A TCP port in 1..65535, digits only. No std::stoi: it throws (defect D4), and
// it would accept "+80", " 80" and "80abc".
bool parse_port(const std::string &s, int &out) {
    if (s.empty() || s.size() > 5) return false;
    int value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 65535) return false;
    out = value;
    return true;
}

// One trimmed "ip:port" entry to a Peer with its identifier computed.
bool parse_member(const std::string &entry, Peer &out) {
    // An embedded NUL would be invisible to inet_pton, which reads a C string:
    // "127.0.0.1\0garbage" would parse as a clean 127.0.0.1.
    if (entry.find('\0') != std::string::npos) return false;

    // Exactly one colon. None is malformed; more than one is IPv6-shaped, and
    // this system is IPv4-only (see the separator note on id_of_address).
    const std::size_t colon = entry.find(':');
    if (colon == std::string::npos || entry.find(':', colon + 1) != std::string::npos)
        return false;

    int port = 0;
    if (!parse_port(entry.substr(colon + 1), port)) return false;

    // Parse, then re-render, the host. Hashing the re-rendered form rather than
    // what the file said is what makes the identifier canonical.
    in_addr addr{};
    if (inet_pton(AF_INET, entry.substr(0, colon).c_str(), &addr) != 1) return false;
    char canonical[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr, canonical, sizeof canonical) == nullptr) return false;

    out.ip   = canonical;
    out.port = port;
    out.id   = id_of_address(out.ip, out.port);
    return true;
}

bool by_id(const Peer &a, const Peer &b) { return a.id < b.id; }

}  // namespace

bool id_from_key(const std::string &hex_key, Id &out) {
    if (hex_key.size() != 40) return false;

    Id value = 0;
    for (std::size_t i = 0; i < hex_key.size(); ++i) {
        unsigned nibble;
        if (!hex_nibble(hex_key[i], nibble)) return false;   // `out` still untouched
        if (i < 16) value = (value << 4) | nibble;           // top 64 bits only
    }

    out = value;
    return true;
}

Id id_of_address(const std::string &ip, int port) {
    Id id = 0;
    // sha1() always returns exactly 40 lowercase hexadecimal characters, so the
    // parse cannot fail. The result is checked anyway rather than discarded, so
    // that a future change to sha1() cannot silently start returning zero.
    const bool ok = id_from_key(sha1(ip + ":" + std::to_string(port)), id);
    assert(ok && "sha1() must return 40 hexadecimal characters");
    (void)ok;
    return id;
}

bool in_range_oc(Id k, Id a, Id b) {
    if (a < b) return a < k && k <= b;   // the arc does not cross zero
    return a < k || k <= b;              // it wraps past zero (or a == b: whole ring)
}

bool in_range_oo(Id k, Id a, Id b) {
    if (a < b) return a < k && k < b;
    return a < k || k < b;               // wraps (or a == b: everything except a)
}

Id finger_start(Id n, int i) {
    assert(i >= 0 && i < ID_BITS);
    // Unsigned overflow is DEFINED to wrap modulo 2^64, so this single addition
    // is the modular arithmetic: the carry out of bit 63 is discarded by the
    // hardware, which is exactly "(mod 2^64)".
    return n + (static_cast<Id>(1) << i);
}

bool validate_ring(std::vector<Peer> &members, std::string &err) {
    if (members.empty()) { err = "membership list is empty"; return false; }

    std::sort(members.begin(), members.end(), by_id);

    // Sorting puts equal identifiers next to each other, so one adjacent scan
    // catches both a repeated address (which always repeats its identifier)
    // and two different addresses that collide.
    for (std::size_t i = 1; i < members.size(); ++i) {
        const Peer &a = members[i - 1];
        const Peer &b = members[i];
        if (a.id != b.id) continue;

        const std::string sa = a.ip + ":" + std::to_string(a.port);
        const std::string sb = b.ip + ":" + std::to_string(b.port);
        err = (sa == sb) ? "duplicate member " + sa
                         : "identifier collision between " + sa + " and " + sb;
        return false;
    }
    return true;
}

bool parse_members(const std::string &text, std::vector<Peer> &out, std::string &err) {
    std::vector<Peer> members;
    std::istringstream in(text);
    std::string raw;
    int line_no = 0;

    while (std::getline(in, raw)) {
        ++line_no;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        Peer p;
        if (!parse_member(line, p)) {
            err = "line " + std::to_string(line_no) + ": not an IPv4 ip:port";
            return false;
        }
        members.push_back(std::move(p));
    }

    if (!validate_ring(members, err)) return false;
    out = std::move(members);          // `out` is written only on success
    return true;
}

Peer successor_of(Id k, const std::vector<Peer> &sorted) {
    assert(!sorted.empty());
    // lower_bound: the first member whose id is NOT less than k, i.e. id >= k.
    auto it = std::lower_bound(sorted.begin(), sorted.end(), k,
                               [](const Peer &p, Id key) { return p.id < key; });
    return it == sorted.end() ? sorted.front() : *it;   // past the largest: wrap
}

Peer predecessor_of(Id id, const std::vector<Peer> &sorted) {
    assert(!sorted.empty());
    auto it = std::lower_bound(sorted.begin(), sorted.end(), id,
                               [](const Peer &p, Id key) { return p.id < key; });
    return it == sorted.begin() ? sorted.back() : *(it - 1);   // before the smallest: wrap
}

std::vector<Peer> build_fingers(Id my_id, const std::vector<Peer> &sorted) {
    std::vector<Peer> fingers;
    fingers.reserve(ID_BITS);
    for (int i = 0; i < ID_BITS; ++i)
        fingers.push_back(successor_of(finger_start(my_id, i), sorted));
    return fingers;
}
