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
#include <string>

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

}  // namespace

int main() {
    std::printf("test-chord: identifier arithmetic\n");

    test_oc_no_wrap();
    test_oc_wraps();
    test_oc_degenerate();
    test_oc_space_boundary();
    test_oo();
    test_finger_start();
    test_id_from_key();
    test_id_of_address();

    std::printf("%s  %d/%d checks passed\n",
                failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
