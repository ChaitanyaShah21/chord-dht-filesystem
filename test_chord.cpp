// In-process tests for the identifier arithmetic.
//
// This file is the reason D-018 split the routing primitives out as free
// functions: every case below is a ring built by hand, with values chosen to
// sit exactly on a boundary. None of it needs a socket, a port or a process.
//
// The cases are deliberately constructed rather than sampled. A ring of eight
// nodes on loopback would exercise almost none of them -- in particular it
// would never produce an arc that wraps past zero with a key sitting on the
// endpoint, which is the failure this whole file exists to catch.

#include "chord.h"

#include <cstdint>
#include <cstdio>
#include <set>
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

std::string key_of(const std::string &head, char pad = '0') {
    std::string k = head;
    k.resize(40, pad);
    return k;
}

// -------------------------------------------------------------------------
void test_oc_no_wrap() {
    // The ordinary arc (10, 20]: lower bound open, upper bound closed.
    check(!in_range_oc(9,  10, 20), "(10,20] excludes 9");
    check(!in_range_oc(10, 10, 20), "(10,20] excludes its open lower bound 10");
    check( in_range_oc(11, 10, 20), "(10,20] includes 11");
    check( in_range_oc(15, 10, 20), "(10,20] includes 15");
    check( in_range_oc(20, 10, 20), "(10,20] includes its closed upper bound 20");
    check(!in_range_oc(21, 10, 20), "(10,20] excludes 21");
}

void test_oc_wraps() {
    // The arc (20, 10] runs clockwise from 20, past the top of the space,
    // through zero, and on to 10. A naive `a < k && k <= b` returns false for
    // every k here, which is the bug this test exists to find.
    check( in_range_oc(21,  20, 10), "(20,10] includes 21, just past the start");
    check( in_range_oc(MAX, 20, 10), "(20,10] includes the largest identifier");
    check( in_range_oc(0,   20, 10), "(20,10] includes zero, the wrap point");
    check( in_range_oc(10,  20, 10), "(20,10] includes its closed upper bound 10");
    check(!in_range_oc(20,  20, 10), "(20,10] excludes its open lower bound 20");
    check(!in_range_oc(11,  20, 10), "(20,10] excludes 11, just past the end");
    check(!in_range_oc(15,  20, 10), "(20,10] excludes the middle of the gap");
}

void test_oc_degenerate() {
    // (n, n] is the WHOLE ring. This is correct and load-bearing: it is how a
    // one-node ring expresses "I own everything". It is also invariant I8's
    // reason to exist -- if two nodes collided on an identifier, each would
    // read its own successor pointer and conclude the same thing.
    check(in_range_oc(0,   7, 7), "(7,7] is the whole ring: contains 0");
    check(in_range_oc(7,   7, 7), "(7,7] is the whole ring: contains 7 itself");
    check(in_range_oc(8,   7, 7), "(7,7] is the whole ring: contains 8");
    check(in_range_oc(MAX, 7, 7), "(7,7] is the whole ring: contains the largest id");

    // The near-miss: (20,19] is everything EXCEPT 20. One identifier apart from
    // the degenerate case, and a completely different answer.
    check(!in_range_oc(20, 20, 19), "(20,19] excludes exactly one point: 20");
    check( in_range_oc(19, 20, 19), "(20,19] includes 19");
    check( in_range_oc(21, 20, 19), "(20,19] includes 21");
}

void test_oc_space_boundary() {
    // Arcs pinned to the two ends of the identifier space, where an
    // accidental signed comparison or an off-by-one would show up.
    check( in_range_oc(0,   MAX, 5), "(MAX,5] includes 0");
    check( in_range_oc(5,   MAX, 5), "(MAX,5] includes 5");
    check(!in_range_oc(6,   MAX, 5), "(MAX,5] excludes 6");
    check(!in_range_oc(MAX, MAX, 5), "(MAX,5] excludes MAX, its open lower bound");

    check(!in_range_oc(0,   0, MAX), "(0,MAX] excludes 0, its open lower bound");
    check( in_range_oc(1,   0, MAX), "(0,MAX] includes 1");
    check( in_range_oc(MAX, 0, MAX), "(0,MAX] includes MAX, its closed upper bound");
}

void test_oo() {
    check(!in_range_oo(20, 10, 20), "(10,20) excludes its open upper bound");
    check( in_range_oo(19, 10, 20), "(10,20) includes 19");
    check(!in_range_oo(10, 10, 20), "(10,20) excludes its open lower bound");
    check( in_range_oo(0,  20, 10), "(20,10) wraps and includes 0");
    check(!in_range_oo(10, 20, 10), "(20,10) excludes its open upper bound");

    // (n, n) is everything except n -- one point different from (n, n].
    check(!in_range_oo(7, 7, 7), "(7,7) excludes 7 itself");
    check( in_range_oo(8, 7, 7), "(7,7) contains everything else");
}

void test_finger_start() {
    check(finger_start(0, 0)  == 1,          "finger[0] of 0 is 1");
    check(finger_start(0, 1)  == 2,          "finger[1] of 0 is 2");
    check(finger_start(0, 63) == (Id(1) << 63), "finger[63] of 0 is 2^63");
    check(finger_start(100, 3) == 108,       "finger[3] of 100 is 100 + 8");

    // The wrap. These are the cases that would need a hand-written carry loop
    // at 160 bits, and that the hardware does for free at 64.
    check(finger_start(MAX, 0) == 0,           "MAX + 1 wraps to 0");
    check(finger_start(MAX, 63) == (Id(1) << 63) - 1, "MAX + 2^63 wraps to 2^63 - 1");
    check(finger_start(Id(1) << 63, 63) == 0,  "2^63 + 2^63 wraps to 0");
}

void test_id_from_key() {
    Id id = 0;

    check(id_from_key(key_of("0123456789abcdef"), id) && id == 0x0123456789abcdefULL,
          "the top 16 hexadecimal characters become the identifier");
    check(id_from_key(key_of("0123456789ABCDEF"), id) && id == 0x0123456789abcdefULL,
          "uppercase parses identically to lowercase");
    check(id_from_key(std::string(40, 'f'), id) && id == MAX,
          "forty f characters give the largest identifier");
    check(id_from_key(std::string(40, '0'), id) && id == 0,
          "forty zeros give identifier 0");

    // Length. A SHA-1 digest is exactly 40 characters; 39 and 41 are corrupt.
    check(!id_from_key(std::string(39, 'a'), id), "39 characters is rejected");
    check(!id_from_key(std::string(41, 'a'), id), "41 characters is rejected");
    check(!id_from_key("", id),                   "the empty key is rejected");

    // A non-hexadecimal character in the TAIL, past the 16 characters actually
    // used. Accepting this would mean a corrupt key routes perfectly.
    std::string tail_garbage = std::string(39, 'a') + "z";
    check(!id_from_key(tail_garbage, id), "garbage in the unused tail is rejected");

    std::string head_garbage = "z" + std::string(39, 'a');
    check(!id_from_key(head_garbage, id), "garbage in the used head is rejected");

    // A rejected parse must leave the caller's variable alone, so that a caller
    // who ignores the return value gets its old value rather than a half-parse.
    Id sentinel = 0xDEADBEEFDEADBEEFULL;
    check(!id_from_key("nonsense", sentinel) && sentinel == 0xDEADBEEFDEADBEEFULL,
          "a failed parse leaves the output untouched");
}

void test_id_of_address() {
    const Id a = id_of_address("127.0.0.1", 9001);
    const Id b = id_of_address("127.0.0.1", 9001);
    const Id c = id_of_address("127.0.0.1", 9002);
    const Id d = id_of_address("127.0.0.2", 9001);

    check(a == b, "the same address always hashes to the same identifier");
    check(a != c, "a different port gives a different identifier");
    check(a != d, "a different host gives a different identifier");

    // The separator matters: "127.0.0.1" + ":" + "9001" must not collide with
    // some other host/port pair that concatenates to the same string.
    check(id_of_address("127.0.0.1", 90001) != id_of_address("127.0.0.19", 0001),
          "the ':' separator keeps host and port apart");
}


// -------------------------------------------------------------------------
// Ring construction. Rings below are built from identifiers chosen by hand;
// addresses are irrelevant to routing and are filled in only so that each
// Peer is distinguishable.

std::vector<Peer> ring_of(const std::vector<Id> &ids) {
    std::vector<Peer> ring;
    int n = 0;
    for (Id id : ids) ring.push_back(Peer{id, "10.0.0." + std::to_string(++n), 9000});
    std::string err;
    validate_ring(ring, err);          // sorts
    return ring;
}

// The oracle. Deliberately a DIFFERENT algorithm from successor_of: no sorting
// assumption and no binary search, just modular subtraction. The clockwise
// distance from k to a member is (member.id - k) mod 2^64, which unsigned
// subtraction computes for free; the nearest member clockwise owns k. If the
// oracle and successor_of shared an implementation, agreeing would prove
// nothing.
Peer brute_owner(Id k, const std::vector<Peer> &members) {
    const Peer *best = &members[0];
    for (const Peer &p : members)
        if (Id(p.id - k) < Id(best->id - k)) best = &p;
    return *best;
}

void test_parse_members() {
    std::vector<Peer> ring;
    std::string err;

    const std::string text =
        "# three local nodes\n"
        "127.0.0.1:9001\n"
        "\n"
        "   127.0.0.1:9002   \n"
        "127.0.0.1:9003\r\n";          // a line saved by a Windows editor
    check(parse_members(text, ring, err) && ring.size() == 3,
          "comments, blank lines, padding and \\r are all tolerated");
    check(ring.size() == 3 && ring[0].id < ring[1].id && ring[1].id < ring[2].id,
          "members come back sorted by identifier");
    bool ids_right = true;
    for (const Peer &p : ring) ids_right = ids_right && p.id == id_of_address(p.ip, p.port);
    check(ids_right, "every identifier is id_of_address of its own canonical address");

    std::vector<Peer> padded;
    check(parse_members("127.0.0.1:09001\n", padded, err) && padded.size() == 1 &&
          padded[0].port == 9001 && padded[0].id == id_of_address("127.0.0.1", 9001),
          "a zero-padded port is canonicalised before hashing");

    const std::vector<std::string> bad = {
        "127.0.0.1",            // no port
        "127.0.0.1:",           // empty port
        "127.0.0.1:0",          // port 0
        "127.0.0.1:65536",      // one past the largest port
        "127.0.0.1:123456",     // six digits
        "127.0.0.1:-1",         // sign
        "127.0.0.1:+80",        // sign that std::stoi would accept
        "127.0.0.1:9001x",      // trailing junk that std::stoi would accept
        "::1:9001",             // IPv6-shaped: separator inside the host
        "999.0.0.1:9001",       // octet out of range
        "localhost:9001",       // a name, not an address
        std::string("127.0.0.1\0junk:9001", 19),   // NUL hidden inside the host
    };
    for (const std::string &line : bad) {
        std::vector<Peer> r;
        std::string e;
        std::string shown;
        for (char c : line) shown += (c == '\0') ? std::string("\\0") : std::string(1, c);
        check(!parse_members(line + "\n", r, e), "rejects \"" + shown + "\"");
    }

    std::string e2;
    std::vector<Peer> r2;
    check(!parse_members("127.0.0.1:9001\nnot-an-address\n", r2, e2) &&
          e2.find("line 2") != std::string::npos,
          "the error names the offending line");

    std::vector<Peer> r3;
    check(!parse_members("", r3, err),                   "an empty list is refused");
    check(!parse_members("# only a comment\n\n", r3, err), "a list of only comments is refused");
    check(!parse_members("127.0.0.1:9001\n127.0.0.1:9001\n", r3, err) &&
          err.find("duplicate") != std::string::npos,    "the same member twice is refused");
    check(!parse_members("127.0.0.1:9001\n127.0.0.1:09001\n", r3, err),
          "the same member written two ways is still a duplicate");

    std::vector<Peer> untouched = ring_of({42});
    check(!parse_members("garbage\n", untouched, err) && untouched.size() == 1 &&
          untouched[0].id == 42, "a failed parse leaves the output untouched");
}

void test_collision_refused() {
    // Real SHA-1 output will never give a test two addresses with one
    // identifier, so construct them. This is invariant I8's first line of
    // defence in Phase 2: a ring that cannot be routed on is never started.
    std::vector<Peer> ring = {
        Peer{500, "10.0.0.1", 9001},
        Peer{100, "10.0.0.2", 9002},
        Peer{500, "10.0.0.3", 9003},
    };
    std::string err;
    check(!validate_ring(ring, err) && err.find("collision") != std::string::npos,
          "two addresses on one identifier are refused, and called a collision");
}

void test_successor_and_predecessor() {
    const std::vector<Peer> r = ring_of({10, 20, 30});

    check(successor_of(5,   r).id == 10, "successor of a key below every node is the smallest");
    check(successor_of(10,  r).id == 10, "a key equal to a node's id belongs to that node");
    check(successor_of(11,  r).id == 20, "a key just past a node belongs to the next");
    check(successor_of(30,  r).id == 30, "a key equal to the largest id belongs to it");
    check(successor_of(31,  r).id == 10, "a key past the largest wraps to the smallest");
    check(successor_of(MAX, r).id == 10, "the largest possible key wraps to the smallest");
    check(successor_of(0,   r).id == 10, "key 0 belongs to the smallest");

    check(predecessor_of(20, r).id == 10, "predecessor of 20 is 10");
    check(predecessor_of(15, r).id == 10, "predecessor of a gap point is the node below it");
    check(predecessor_of(10, r).id == 30, "predecessor of the smallest wraps to the largest");
    check(predecessor_of(5,  r).id == 30, "predecessor of a point below every node wraps");

    const std::vector<Peer> ends = ring_of({0, MAX});
    check(successor_of(1, ends).id == MAX,  "ring {0, MAX}: key 1 belongs to MAX");
    check(successor_of(MAX, ends).id == MAX, "ring {0, MAX}: MAX owns itself");
    check(predecessor_of(0, ends).id == MAX, "ring {0, MAX}: 0's predecessor wraps to MAX");

    const std::vector<Peer> alone = ring_of({7});
    check(successor_of(7, alone).id == 7 && successor_of(8, alone).id == 7 &&
          predecessor_of(7, alone).id == 7, "a one-node ring is its own successor and predecessor");
}

void test_build_fingers() {
    const std::vector<Peer> r = ring_of({10, 20, 30});

    // The off-by-one trap written into the header: successor_of(my_id) is the
    // node itself; the node's actual successor is finger[0].
    check(successor_of(10, r).id == 10, "successor_of(my_id) is the node itself -- the trap");
    check(build_fingers(10, r)[0].id == 20, "finger[0] is the true successor");
    check(build_fingers(30, r)[0].id == 10, "the largest node's successor wraps to the smallest");
    check(build_fingers(MAX, ring_of({5, MAX}))[0].id == 5,
          "a node at MAX: finger[0] starts at MAX+1 = 0 and finds 5");

    bool all_self = true;
    for (const Peer &f : build_fingers(7, ring_of({7}))) all_self = all_self && f.id == 7;
    check(all_self, "in a one-node ring every one of the 64 fingers is the node itself");

    // Cross-check every finger of every node against the independent oracle,
    // on rings built to be awkward: clustered ids, both ends of the space, and
    // a two-node ring.
    const std::vector<std::vector<Id>> rings = {
        {10, 20, 30},
        {0, MAX},
        {MAX - 2, MAX - 1, MAX, 0, 1},
        {1, 2, 3, Id(1) << 63},
        {100, Id(1) << 40, Id(1) << 62, (Id(1) << 63) + 5, MAX - 100},
        {12345, 67890},
    };
    int disagreements = 0;
    for (const auto &ids : rings) {
        const std::vector<Peer> ring = ring_of(ids);
        for (const Peer &node : ring) {
            const std::vector<Peer> f = build_fingers(node.id, ring);
            for (int i = 0; i < ID_BITS; ++i)
                if (f[i].id != brute_owner(finger_start(node.id, i), ring).id) ++disagreements;
        }
    }
    check(disagreements == 0, "every finger of every node agrees with the brute-force oracle");

    // Why the table is O(log N): 1024 evenly spaced nodes, 2^54 apart. From node
    // 0, fingers 0..54 all land on node 1 (their starts are all <= 2^54), and
    // fingers 55..63 land on nodes 2, 4, ..., 512. 64 rows, 10 distinct nodes,
    // and log2(1024) = 10.
    std::vector<Id> even;
    for (Id k = 0; k < 1024; ++k) even.push_back(k << 54);
    std::set<Id> distinct;
    for (const Peer &p : build_fingers(0, ring_of(even))) distinct.insert(p.id);
    check(distinct.size() == 10, "1024 evenly spaced nodes: 64 fingers, exactly 10 distinct");
}

void test_id_hex() {
    check(id_to_hex(0)   == "0000000000000000", "identifier 0 is padded to 16 zeros");
    check(id_to_hex(1)   == "0000000000000001", "identifier 1 keeps its leading zeros");
    check(id_to_hex(MAX) == "ffffffffffffffff", "the largest identifier is 16 f characters");
    check(id_to_hex(0x0123456789abcdefULL) == "0123456789abcdef", "digits come out lowercase, in order");

    bool round_trip = true;
    for (Id v : {Id(0), Id(1), Id(0x8000000000000000ULL), MAX, Id(0x00ff00ff00ff00ffULL)}) {
        Id back = 0;
        round_trip = round_trip && id_from_hex(id_to_hex(v), back) && back == v;
    }
    check(round_trip, "every identifier survives id_to_hex then id_from_hex unchanged");

    Id id = 0;
    check(id_from_hex("0123456789ABCDEF", id) && id == 0x0123456789abcdefULL, "uppercase is accepted");
    check(!id_from_hex("123456789abcdef", id),   "15 characters is rejected");
    check(!id_from_hex("00123456789abcdef", id), "17 characters is rejected");
    check(!id_from_hex("0x23456789abcdef", id),  "a 0x prefix is rejected, not skipped");
    check(!id_from_hex("0123456789abcdeg", id),  "a non-hex character is rejected");
    check(!id_from_hex("", id),                  "the empty string is rejected");

    Id sentinel = 42;
    check(!id_from_hex("nope", sentinel) && sentinel == 42, "a failed parse leaves the output untouched");
}

}  // namespace

int main() {
    std::printf("test-chord: identifier arithmetic and ring construction\n");

    test_oc_no_wrap();
    test_oc_wraps();
    test_oc_degenerate();
    test_oc_space_boundary();
    test_oo();
    test_finger_start();
    test_id_from_key();
    test_id_of_address();
    test_id_hex();
    test_parse_members();
    test_collision_refused();
    test_successor_and_predecessor();
    test_build_fingers();

    std::printf("%s  %d/%d checks passed\n",
                failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
