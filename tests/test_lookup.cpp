// In-process tests for the lookup originator (lookup.cpp).
//
// Every ring here is built in this process and reached through a constructed
// Transport -- the reason D-024 made the exchange a parameter. A correct ring
// of real processes can only ever produce correct replies; the cases that most
// need testing are replies a correct ring never sends: pointing backwards,
// naming itself, overshooting the key, garbage, ERR, a dead or silent node,
// and a ring that lies forever.
//
// The correct-ring tests compare against an INDEPENDENT model: owners by
// brute-force clockwise distance, and the expected path by a distance-based
// re-statement of the finger rule. Neither calls route_step, build_fingers or
// the in_range functions, so agreeing with them proves something.

#include "chord.h"
#include "lookup.h"

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

int checks   = 0;
int failures = 0;

void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

const Id MAX = UINT64_MAX;

std::string addr(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

// A distinct, canonical address for the n-th constructed node.
Peer make_peer(Id id, int n) {
    return Peer{id, "10.0." + std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255), 9000};
}

std::vector<Peer> ring_of(const std::vector<Id> &ids) {
    std::vector<Peer> ring;
    int n = 0;
    for (Id id : ids) ring.push_back(make_peer(id, ++n));
    std::string err;
    validate_ring(ring, err);          // sorts by identifier
    return ring;
}

// What a node puts on the wire, rendered exactly as node.cpp renders it.
std::string render(bool owner, const Peer &p) {
    return std::string(owner ? "OWNER " : "NEXT ") + id_to_hex(p.id) + " " + p.ip + " " +
           std::to_string(p.port);
}

// -------------------------------------------------------------------------
// A correct ring in process. Each member answers FIND_SUCCESSOR exactly as
// node.cpp does: tables from build_fingers, the decision from route_step.

struct Ring {
    std::vector<Peer>              members;   // sorted by identifier
    std::vector<std::vector<Peer>> fingers;
    std::map<std::string, size_t>  by_address;
    long requests_seen = 0;
    bool bad_request_seen = false;

    explicit Ring(std::vector<Peer> sorted) : members(std::move(sorted)) {
        for (size_t i = 0; i < members.size(); ++i) {
            fingers.push_back(build_fingers(members[i].id, members));
            by_address[addr(members[i])] = i;
        }
    }

    Delivery answer(const Peer &to, const std::string &request, std::string &reply) {
        ++requests_seen;
        const auto it = by_address.find(addr(to));
        if (it == by_address.end()) return Delivery::Refused;    // nobody at that address
        const size_t i = it->second;

        Id k;
        if (request.compare(0, 15, "FIND_SUCCESSOR ") != 0 || !id_from_hex(request.substr(15), k) ||
            request != "FIND_SUCCESSOR " + id_to_hex(k)) {        // exactly the canonical form
            bad_request_seen = true;
            reply = "ERR unknown request";
            return Delivery::Ok;
        }
        const RouteStep st = route_step(members[i], fingers[i][0], fingers[i], k);
        reply = render(st.owner, st.peer);
        return Delivery::Ok;
    }
};

// The oracle for owners: the nearest member clockwise from k. Unsigned
// subtraction is clockwise distance modulo 2^64.
const Peer &brute_owner(Id k, const std::vector<Peer> &members) {
    const Peer *best = &members[0];
    for (const Peer &p : members)
        if (Id(p.id - k) < Id(best->id - k)) best = &p;
    return *best;
}

// The independent model of the whole lookup: the list of node identifiers an
// iterative originator should query, written from the finger RULE in terms of
// clockwise distances. Fingers are computed with brute_owner, not build_fingers.
struct Model {
    const std::vector<Peer> &members;
    std::map<Id, std::vector<Id>> finger_cache;

    const std::vector<Id> &fingers_of(Id n) {
        auto it = finger_cache.find(n);
        if (it != finger_cache.end()) return it->second;
        std::vector<Id> f;
        for (int i = 0; i < ID_BITS; ++i) f.push_back(brute_owner(n + (Id(1) << i), members).id);
        return finger_cache[n] = f;
    }

    std::vector<Id> path(Id start, Id k, Id &owner) {
        std::vector<Id> visited;
        Id cur = start;
        while (true) {
            visited.push_back(cur);
            const std::vector<Id> &f = fingers_of(cur);
            const Id succ_dist = f[0] - cur;            // 0 means a one-node ring
            const Id key_dist  = k - cur;               // 0 means k is cur itself
            if (succ_dist == 0 || (key_dist != 0 && key_dist <= succ_dist)) {
                owner = f[0];
                return visited;
            }
            // The farthest finger strictly short of k. key_dist 0 = the whole ring.
            Id best = cur, best_dist = 0;
            for (Id x : f) {
                const Id d = x - cur;
                if (d == 0 || (key_dist != 0 && d >= key_dist)) continue;
                if (d > best_dist) { best = x; best_dist = d; }
            }
            cur = (best_dist == 0) ? f[0] : best;
        }
    }
};

void test_correct_rings() {
    std::mt19937_64 rng(20261007);
    int lookups = 0, wrong_owner = 0, wrong_path = 0, errors = 0, worst = 0;
    bool requests_well_formed = true;

    const std::vector<size_t> sizes = {1, 2, 3, 5, 8, 64, 1024};
    for (size_t n : sizes) {
        for (int trial = 0; trial < (n >= 1024 ? 2 : 6); ++trial) {
            std::vector<Id> ids;
            for (size_t i = 0; i < n; ++i) ids.push_back(rng());
            Ring ring(ring_of(ids));
            Model model{ring.members, {}};
            Transport t = [&ring](const Peer &to, const std::string &req, std::string &rep) {
                return ring.answer(to, req, rep);
            };

            // Keys on, just past and just before every node (sampled on big rings),
            // both ends of the space, and random keys.
            std::vector<Id> keys = {0, MAX};
            const size_t stride = n > 64 ? n / 64 : 1;
            for (size_t i = 0; i < n; i += stride) {
                keys.push_back(ring.members[i].id);
                keys.push_back(ring.members[i].id + 1);
                keys.push_back(ring.members[i].id - 1);
            }
            for (int i = 0; i < 64; ++i) keys.push_back(rng());

            for (size_t s = 0; s < n; s += stride) {
                const Peer &start = ring.members[s];
                for (Id k : keys) {
                    const LookupResult r = lookup(start, k, t);
                    ++lookups;
                    if (!r.ok()) {
                        if (++errors <= 3) std::printf("        unexpected error: %s\n", r.message.c_str());
                        continue;
                    }
                    if (r.owner.id != brute_owner(k, ring.members).id ||
                        addr(r.owner) != addr(brute_owner(k, ring.members)))
                        ++wrong_owner;
                    Id model_owner = 0;
                    const std::vector<Id> expect = model.path(start.id, k, model_owner);
                    std::vector<Id> got;
                    for (const Peer &p : r.path) got.push_back(p.id);
                    if (got != expect || model_owner != r.owner.id) ++wrong_path;
                    if (r.requests() > worst) worst = r.requests();
                    if (r.hops() != r.requests() - 1) ++wrong_path;
                }
            }
            requests_well_formed = requests_well_formed && !ring.bad_request_seen;
        }
    }

    std::printf("  info  %d lookups on random rings of 1..1024 nodes; worst %d requests\n", lookups, worst);
    check(errors == 0, "every lookup on a correct ring succeeds");
    check(wrong_owner == 0, "every owner equals the brute-force owner, identifier and address");
    check(wrong_path == 0, "every path equals the independent model's path, node for node");
    check(requests_well_formed, "every request is exactly \"FIND_SUCCESSOR <16 lowercase hex>\"");
    check(worst <= MAX_REQUESTS, "no correct-ring lookup comes near the request cap");
}

void test_small_rings() {
    // One node: (n, n] is the whole ring, so the start node answers OWNER itself.
    Ring alone(ring_of({7}));
    Transport t1 = [&alone](const Peer &to, const std::string &q, std::string &r) { return alone.answer(to, q, r); };
    for (Id k : {Id(0), Id(7), Id(8), MAX}) {
        const LookupResult r = lookup(alone.members[0], k, t1);
        check(r.ok() && r.owner.id == 7 && r.requests() == 1 && r.hops() == 0,
              "one-node ring: key " + id_to_hex(k) + " owned by the only node in 1 request, 0 hops");
    }

    // Two nodes at the two ends of the space, and every key that sits on a boundary.
    Ring two(ring_of({0, MAX}));
    Transport t2 = [&two](const Peer &to, const std::string &q, std::string &r) { return two.answer(to, q, r); };
    bool all_right = true;
    for (const Peer &start : two.members)
        for (Id k : {Id(0), Id(1), MAX - 1, MAX}) {
            const LookupResult r = lookup(start, k, t2);
            all_right = all_right && r.ok() && r.owner.id == brute_owner(k, two.members).id;
        }
    check(all_right, "two-node ring {0, MAX}: keys 0, 1, MAX-1, MAX from both nodes reach the right owner");

    // k equal to the start node's own identifier: the start node owns it, but
    // the rule only lets a node claim keys for its SUCCESSOR, so the lookup must
    // go round to the start node's predecessor and come back with OWNER start.
    Ring three(ring_of({10, 20, 30}));
    Transport t3 = [&three](const Peer &to, const std::string &q, std::string &r) { return three.answer(to, q, r); };
    const LookupResult r = lookup(three.members[0], 10, t3);
    check(r.ok() && r.owner.id == 10 && r.requests() == 2 && r.path[1].id == 30,
          "k == start's own id: asks the predecessor, which names the start node as owner");
}

void test_cap_is_tight() {
    // The worst case D-024's derivation allows, built on purpose. Nodes sit at
    // 2^64 - 2^(64-j) for j = 0..64: 0, 2^63, 2^63 + 2^62, ..., MAX - 1, MAX.
    // Looking up k = 0 from node 0, the arc (0, 0) is the whole ring and k's
    // predecessor is MAX. Each node's longest finger short of k is the next node
    // in that list, so every jump covers only just over half the remaining
    // distance: 64 NEXT replies, and MAX's OWNER reply is request 65.
    std::vector<Id> ids;
    for (int j = 0; j <= 64; ++j) ids.push_back(j == 0 ? 0 : Id(0) - (j == 64 ? 1 : (Id(1) << (64 - j))));
    Ring ring(ring_of(ids));
    check(ring.members.size() == 65, "the worst-case ring has 65 distinct nodes");
    Transport t = [&ring](const Peer &to, const std::string &q, std::string &r) { return ring.answer(to, q, r); };

    const LookupResult r = lookup(ring.members[0], 0, t);
    check(r.ok(), "the worst-case lookup succeeds: the cap is never exceeded on a correct ring");
    // The literal 65, not MAX_REQUESTS: this is the derivation's number, and a
    // check against the constant would move with any edit to the constant.
    check(r.requests() == 65 && r.hops() == 64,
          "it needs exactly 65 requests (64 hops): the cap is tight, not slack");
    check(MAX_REQUESTS == 65, "MAX_REQUESTS is the derived 65 -- no lower, or this lookup fails; no higher");
    check(r.ok() && r.owner.id == 0, "and it reaches the right owner");
}

// -------------------------------------------------------------------------
// Hostile rings. A scripted transport: each address gives one fixed reply,
// or one fixed failure.

struct Script {
    std::map<std::string, std::string> reply;
    std::map<std::string, Delivery>    failure;

    Delivery answer(const Peer &to, const std::string &, std::string &out) {
        const std::string a = addr(to);
        if (failure.count(a)) return failure[a];
        if (!reply.count(a)) return Delivery::Refused;
        out = reply[a];
        return Delivery::Ok;
    }
};

const Peer S = Peer{100, "10.0.0.1", 9000};     // the start node
const Peer B = Peer{300, "10.0.0.2", 9000};     // a legitimate next hop for k = 500
const Id   K = 500;

LookupResult run(Script &script, Id k = K, const Peer &start = S) {
    Transport t = [&script](const Peer &to, const std::string &q, std::string &r) { return script.answer(to, q, r); };
    return lookup(start, k, t);
}

bool names(const LookupResult &r, const Peer &p) {
    return r.message.find(addr(p)) != std::string::npos;
}

void test_progress_check() {
    {   // Pointing backwards.
        Script s; s.reply[addr(S)] = render(false, Peer{50, "10.0.0.9", 9000});
        const LookupResult r = run(s);
        check(r.error == LookupError::NoProgress && names(r, S) && r.path.size() == 1,
              "NEXT pointing backwards is no-progress, blamed on the node that sent it");
    }
    {   // Naming itself, by identifier and address.
        Script s; s.reply[addr(S)] = render(false, S);
        check(run(s).error == LookupError::NoProgress, "NEXT naming the node itself is no-progress");
    }
    {   // Naming its own identifier at a different address (an identifier collision).
        Script s; s.reply[addr(S)] = render(false, Peer{S.id, "10.0.0.8", 9000});
        check(run(s).error == LookupError::NoProgress, "NEXT naming the node's own identifier elsewhere is no-progress");
    }
    {   // Landing exactly on k: that node is k's owner, so a NEXT to it overshoots.
        Script s; s.reply[addr(S)] = render(false, Peer{K, "10.0.0.9", 9000});
        check(run(s).error == LookupError::NoProgress, "NEXT landing exactly on k is no-progress (open arc)");
    }
    {   // Past k.
        Script s; s.reply[addr(S)] = render(false, Peer{K + 1, "10.0.0.9", 9000});
        check(run(s).error == LookupError::NoProgress, "NEXT overshooting past k is no-progress");
    }
    {   // An honest first hop, then a liar: the error must name the SECOND node.
        Script s;
        s.reply[addr(S)] = render(false, B);
        s.reply[addr(B)] = render(false, Peer{200, "10.0.0.9", 9000});
        const LookupResult r = run(s);
        check(r.error == LookupError::NoProgress && names(r, B) && !names(r, S) && r.path.size() == 2,
              "a liar after an honest hop is named, and the honest node is not");
    }
    {   // An attempted cycle: B sends the lookup back to S. Cycles are impossible
        // by construction, because S is not strictly between B and k.
        Script s;
        s.reply[addr(S)] = render(false, B);
        s.reply[addr(B)] = render(false, S);
        check(run(s).error == LookupError::NoProgress, "a two-node cycle is refused at its first repeat");
    }
    {   // OWNER that does not cover k.
        Script s; s.reply[addr(S)] = render(true, Peer{400, "10.0.0.9", 9000});
        const LookupResult r = run(s);
        check(r.error == LookupError::NoProgress && names(r, S), "OWNER that k does not fall before is no-progress");
    }
    {   // The closed end: an owner whose identifier IS k is right.
        Script s; s.reply[addr(S)] = render(true, Peer{K, "10.0.0.9", 9000});
        const LookupResult r = run(s);
        check(r.ok() && r.owner.id == K, "OWNER whose identifier equals k is accepted (closed arc)");
    }
    {   // Wrap-around: start near the top of the space, k just past zero.
        const Peer top = Peer{MAX - 10, "10.0.0.1", 9000};
        Script ok_s;  ok_s.reply[addr(top)]  = render(false, Peer{0, "10.0.0.2", 9000});
        ok_s.reply["10.0.0.2:9000"] = render(true, Peer{7, "10.0.0.3", 9000});
        const LookupResult r1 = run(ok_s, 5, top);
        check(r1.ok() && r1.owner.id == 7, "wrap-around: NEXT to 0 and OWNER 7 are both accepted for k = 5");

        Script bad_s; bad_s.reply[addr(top)] = render(false, Peer{MAX - 20, "10.0.0.2", 9000});
        check(run(bad_s, 5, top).error == LookupError::NoProgress,
              "wrap-around: NEXT to a node just behind the start is no-progress");
    }
    {   // KNOWN LIMIT, pinned on purpose. A node that claims to be alone -- OWNER
        // naming itself -- is accepted even after another node pointed at it,
        // because (n, n] is the whole ring. On a fixed ring that is a lie; during
        // a Phase 3 join it is a legitimate transient (the first node's successor
        // is itself until stabilisation runs), so rejecting it would accuse
        // correct nodes. If this check ever fails, the rule changed: decide it.
        Script s;
        s.reply[addr(S)] = render(false, B);
        s.reply[addr(B)] = render(true, B);
        const LookupResult r = run(s);
        check(r.ok() && r.owner.id == B.id, "KNOWN LIMIT: a node claiming to own everything is believed");
    }
}

void test_malformed_replies() {
    const std::string id  = id_to_hex(400);
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"",                                            "the empty reply"},
        {"OWNER " + id + " 10.0.0.2",                   "3 words"},
        {"OWNER " + id + " 10.0.0.2 9000 extra",        "5 words"},
        {"OWNER  " + id + " 10.0.0.2 9000",             "a doubled space"},
        {" OWNER " + id + " 10.0.0.2 9000",             "a leading space"},
        {"OWNER " + id + " 10.0.0.2 9000 ",             "a trailing space"},
        {"OWNER\t" + id + "\t10.0.0.2\t9000",           "tabs as separators"},
        {"owner " + id + " 10.0.0.2 9000",              "a lowercase verb"},
        {"FOUND " + id + " 10.0.0.2 9000",              "an unknown verb"},
        {"OWNER 000000000000190 10.0.0.2 9000",         "a 15-character identifier"},
        {"OWNER 00000000000001900 10.0.0.2 9000",       "a 17-character identifier"},
        {"OWNER 000000000000019g 10.0.0.2 9000",        "a non-hex identifier"},
        {"OWNER 0x00000000000190 10.0.0.2 9000",        "a 0x-prefixed identifier"},
        {"OWNER " + id + " 999.0.0.1 9000",             "an octet above 255"},
        {"OWNER " + id + " 10.0.0 9000",                "three octets"},
        {"OWNER " + id + " 10.0.0.02 9000",             "a non-canonical address"},
        {"OWNER " + id + " localhost 9000",             "a host name"},
        {"OWNER " + id + " ::1 9000",                   "an IPv6 address"},
        {"OWNER " + id + " " + std::string("10.0.0.2\0x", 10) + " 9000", "an address hiding a NUL"},
        {"OWNER " + id + " 10.0.0.2 0",                 "port 0"},
        {"OWNER " + id + " 10.0.0.2 65536",             "port 65536"},
        {"OWNER " + id + " 10.0.0.2 -1",                "a negative port"},
        {"OWNER " + id + " 10.0.0.2 +80",               "a signed port"},
        {"OWNER " + id + " 10.0.0.2 09000",             "a zero-padded port"},
        {"OWNER " + id + " 10.0.0.2 9000a",             "a port with trailing junk"},
        {"OWNER " + id + " 10.0.0.2 123456",            "a six-digit port"},
        {"ERRATIC " + id + " 10.0.0.2 9000",            "a verb that merely starts with ERR"},
    };
    for (const auto &c : bad) {
        Script s; s.reply[addr(S)] = c.first;
        const LookupResult r = run(s);
        check(r.error == LookupError::Malformed && names(r, S), "malformed reply rejected: " + c.second);
    }

    {   // The quoted reply in the message is sanitised: no raw control bytes.
        Script s; s.reply[addr(S)] = "OWNER \x1b[31m\nINJECTED";
        const LookupResult r = run(s);
        bool clean = true;
        for (char c : r.message) clean = clean && static_cast<unsigned char>(c) >= 0x20;
        check(r.error == LookupError::Malformed && clean, "a hostile reply cannot put control bytes into the message");
    }

    {   // A failed parse leaves the caller's outputs untouched.
        bool owner = true;
        Peer p{42, "1.2.3.4", 7};
        std::string why;
        const bool parsed = parse_reply("NEXT " + id + " 10.0.0.2 99999", owner, p, why);
        check(!parsed && owner && p.id == 42 && p.ip == "1.2.3.4" && p.port == 7 && !why.empty(),
              "parse_reply leaves its outputs untouched on failure and says why");
        check(parse_reply("NEXT " + id + " 10.0.0.2 65535", owner, p, why) && !owner &&
              p.id == 400 && p.ip == "10.0.0.2" && p.port == 65535, "the largest port parses");
        check(parse_reply("OWNER FFFFFFFFFFFFFFFF 255.255.255.255 1", owner, p, why) && owner &&
              p.id == MAX && p.port == 1, "uppercase hex, the top address and port 1 parse");
    }

    {   // ERR is its own error, distinct from malformed, and carries the node's reason.
        Script s; s.reply[addr(S)] = "ERR identifier must be 16 hexadecimal characters";
        const LookupResult r = run(s);
        check(r.error == LookupError::ErrReply && names(r, S) &&
              r.message.find("16 hexadecimal") != std::string::npos,
              "ERR <reason> is an err-reply naming the node and quoting the reason");
        Script s2; s2.reply[addr(S)] = "ERR";
        check(run(s2).error == LookupError::ErrReply, "a bare ERR is an err-reply");
    }
}

void test_transport_failures() {
    const std::vector<std::pair<Delivery, LookupError>> cases = {
        {Delivery::Refused, LookupError::Refused},
        {Delivery::Timeout, LookupError::Timeout},
        {Delivery::Closed,  LookupError::Closed},
        {Delivery::Failed,  LookupError::Unreachable},
    };
    for (const auto &c : cases) {
        Script s;
        s.reply[addr(S)] = render(false, B);
        s.failure[addr(B)] = c.first;
        const LookupResult r = run(s);
        check(r.error == c.second && names(r, B) && !names(r, S) && r.path.size() == 2 &&
              addr(r.path.back()) == addr(B),
              std::string("a ") + delivery_name(c.first) + " second node gives " + lookup_error_name(c.second) +
              ", names that node, and ends the path with it");
    }
    check(lookup_error_name(LookupError::Refused) != std::string(lookup_error_name(LookupError::Timeout)),
          "refused and timeout are reported under different names (D-013)");

    Transport empty;
    check(lookup(S, K, empty).error == LookupError::Unreachable, "an empty transport is an error, not a throw");
}

void test_liars() {
    {   // A liar ring that names a brand-new random node at every step. A random
        // identifier is almost never strictly between the current node and k for
        // long, so the progress check stops it well before the cap.
        std::mt19937_64 rng(7);
        int counter = 0;
        Transport t = [&rng, &counter](const Peer &, const std::string &, std::string &rep) {
            rep = render(false, make_peer(rng(), ++counter));
            return Delivery::Ok;
        };
        const LookupResult r = lookup(S, MAX - 1, t);
        check(r.error == LookupError::NoProgress && r.requests() < MAX_REQUESTS,
              "a ring naming random new nodes is stopped by the progress check (after " +
              std::to_string(r.requests()) + " requests)");
    }
    {   // A creeping liar: every reply is a NEW node exactly one identifier
        // closer to k. Each reply passes the progress check, so only the cap can
        // stop it -- and it must, at exactly MAX_REQUESTS, naming the node.
        int counter = 0;
        long asked = 0;
        Transport t = [&counter, &asked](const Peer &to, const std::string &, std::string &rep) {
            ++asked;
            rep = render(false, make_peer(to.id + 1, ++counter));
            return Delivery::Ok;
        };
        const LookupResult r = lookup(S, MAX, t);
        check(r.error == LookupError::TooManyRequests && r.requests() == 65 && asked == 65,
              "a creeping liar is stopped by the cap after exactly 65 requests, not one more");
        check(r.error == LookupError::TooManyRequests && names(r, r.path.back()),
              "the cap error names the node that sent the last NEXT");
    }
}

}  // namespace

int main() {
    std::printf("test-lookup: the originator's loop, against correct and hostile rings\n");

    test_correct_rings();
    test_small_rings();
    test_cap_is_tight();
    test_progress_check();
    test_malformed_replies();
    test_transport_failures();
    test_liars();

    std::printf("%s  %d/%d checks passed\n",
                failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
