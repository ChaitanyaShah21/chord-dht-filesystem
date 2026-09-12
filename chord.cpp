#include "chord.h"
#include "sha1.h"

#include <cassert>
#include <cstddef>

namespace {

// One hexadecimal character to its value in 0..15. Returns false for anything
// that is not a hexadecimal digit, including '\0' and whitespace.
bool hex_nibble(char c, unsigned &out) {
    if (c >= '0' && c <= '9') { out = static_cast<unsigned>(c - '0');      return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<unsigned>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<unsigned>(c - 'A' + 10); return true; }
    return false;
}

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
