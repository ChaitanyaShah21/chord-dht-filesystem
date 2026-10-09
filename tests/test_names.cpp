// In-process tests for the tracker's namespace (names.cpp, decisions D-058, D-059,
// D-080): the state machine a Raft group replicates.
//
// Since D-080 the namespace keeps no log of its own -- its durability is the Raft
// log, tested in test-raft -- and no lock -- the replica's lock protects it, tested
// in test-replica. What is left, and tested here: which requests are well formed,
// what applying an entry does, and what a read answers.

#include "names.h"
#include "net.h"
#include "store.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>
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

std::string key(int i) { return chunk_key("manifest " + std::to_string(i)); }

// What a replica that leads a group of one does with a request line: a REGISTER
// becomes an entry and is applied; a read is answered from the state (replica.cpp
// does the same after Raft has committed or confirmed).
std::string handle(Namespace &ns, const std::string &line) {
    const NamesRequest r = parse_names_request(line);
    switch (r.kind) {
        case NamesRequest::Kind::Invalid:  return r.error;
        case NamesRequest::Kind::Ping:     return "PONG";
        case NamesRequest::Kind::Register: return apply_command(ns, register_command(r.name, r.key));
        default:                           return answer_read(ns, r);
    }
}

void test_names() {
    check(valid_name("a") && valid_name(std::string(200, 'x')) && valid_name("photos/2026/a.jpg") && valid_name("..."),
          "names of 1 to 200 visible characters, slashes and dots included");
    check(!valid_name("") && !valid_name(std::string(201, 'x')), "an empty name and a 201-byte name are refused");
    check(!valid_name("a b") && !valid_name("a\nb") && !valid_name("a\rb") && !valid_name(std::string("a\0b", 3)) &&
              !valid_name("a\x7f") && !valid_name("caf\xc3\xa9") && !valid_name("\xff"),
          "spaces, control characters, DEL and bytes above 0x7e are refused");
}

void test_commands() {
    check(register_command("doc.pdf", key(1)) == "REGISTER doc.pdf " + key(1), "a registration's entry");
    check(valid_command("NOOP") && valid_command(register_command("doc.pdf", key(1))), "NOOP and REGISTER are commands");
    check(!valid_command("") && !valid_command("NOOP x") && !valid_command("REGISTER doc.pdf") &&
              !valid_command("REGISTER doc.pdf " + std::string(40, 'A')) && !valid_command("REGISTER  doc.pdf " + key(1)) &&
              !valid_command("register doc.pdf " + key(1)) && !valid_command("DELETE doc.pdf"),
          "anything else is not -- an APPEND carrying one is refused whole (raft_wire.cpp)");

    Namespace ns;
    check(apply_command(ns, "NOOP").empty() && ns.names.empty(), "a NOOP changes nothing and answers nobody");
    check(apply_command(ns, "BOGUS x y").empty() && ns.names.empty(), "an invalid command changes nothing");
    check(apply_command(ns, register_command("a", key(1))) == "OK" && ns.names.at("a") == key(1), "the first registration of a name takes it");
    check(apply_command(ns, register_command("a", key(1))) == "OK" && ns.names.size() == 1,
          "the same name and key again: OK, and nothing changes (a retry, D-075)");
    check(apply_command(ns, register_command("a", key(2))) == "EXISTS " + key(1) && ns.names.at("a") == key(1),
          "another key: EXISTS naming the first, and nothing changes");

    // Two replicas applying the same entries in the same order agree; that is all
    // Raft needs from a state machine. In another order they need not -- which is
    // why the order is the log's.
    const std::vector<std::string> log = {register_command("x", key(3)), register_command("x", key(4)), "NOOP",
                                          register_command("y", key(5))};
    Namespace one, two;
    std::vector<std::string> r1, r2;
    for (const std::string &c : log) r1.push_back(apply_command(one, c));
    for (const std::string &c : log) r2.push_back(apply_command(two, c));
    check(one.names == two.names && r1 == r2, "the same log applied twice gives the same state and the same answers");
}

void test_requests() {
    Namespace s;
    check(handle(s, "PING") == "PONG", "PING");
    check(handle(s, "REGISTER a.txt " + key(1)) == "OK", "a new name is registered");
    check(handle(s, "RESOLVE a.txt") == "FILE " + key(1), "and resolves to its key");
    check(handle(s, "REGISTER a.txt " + key(1)) == "OK", "the same name and key again succeeds (a retried upload)");
    check(handle(s, "REGISTER a.txt " + key(2)) == "EXISTS " + key(1), "a different key for a taken name is refused, naming the key");
    check(handle(s, "RESOLVE a.txt") == "FILE " + key(1), "and the name still resolves to the first key");
    check(handle(s, "RESOLVE nope") == "MISSING", "an unknown name is MISSING");

    const std::vector<std::string> bad = {
        "", "REGISTER", "REGISTER a.txt", "REGISTER a.txt " + key(1) + " extra", "REGISTER a.txt " + std::string(40, 'A'),
        "REGISTER a.txt ../../etc/passwd", "REGISTER " + std::string(201, 'n') + " " + key(1), "REGISTER  a.txt " + key(1),
        "RESOLVE", "RESOLVE a b", "LIST a b", "LIST " + std::string(201, 'n'), "register a.txt " + key(1), "BOGUS", "PING x"};
    bool all_err = true;
    for (const std::string &b : bad) {
        const NamesRequest r = parse_names_request(b);
        if (r.kind != NamesRequest::Kind::Invalid || r.error.compare(0, 4, "ERR ") != 0 || r.error.find('\n') != std::string::npos) {
            all_err = false;
            std::printf("    not refused cleanly: '%s' -> '%s'\n", b.substr(0, 60).c_str(), r.error.c_str());
        }
    }
    check(all_err, "every malformed request is refused with one ERR line (" + std::to_string(bad.size()) + " cases)");
    for (const std::string &b : bad) handle(s, b);
    check(s.names.size() == 1, "and none of them changed the namespace");
}

void test_list() {
    Namespace ns;
    check(handle(ns, "LIST") == "NAMES 0", "an empty namespace lists nothing");
    std::set<std::string> expect;
    for (int i = 0; i < 300; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%04d-%010d", i, i * 7919);
        expect.insert(name);
        handle(ns, std::string("REGISTER ") + name + " " + key(i));
    }
    std::vector<std::string> got;
    std::string after;
    bool bounded = true;
    int pages = 0;
    for (; pages < 100; ++pages) {
        const std::string reply = handle(ns, after.empty() ? "LIST" : "LIST " + after);
        bounded = bounded && reply.size() <= MAX_LINE;
        std::istringstream in(reply);
        std::string word;
        int n = -1;
        in >> word >> n;
        if (word != "NAMES" || n < 0) { bounded = false; break; }
        if (n == 0) break;
        for (int i = 0; i < n && in >> word; ++i) got.push_back(word);
        after = got.back();
    }
    check(bounded && pages > 1, "300 names list in " + std::to_string(pages) + " pages, each one line within MAX_LINE");
    check(got.size() == 300 && std::set<std::string>(got.begin(), got.end()) == expect && std::is_sorted(got.begin(), got.end()),
          "every name exactly once, in order");
    check(handle(ns, "LIST " + std::string(200, '~')) == "NAMES 0", "listing after the last name ends the listing");
    NamesRequest w;
    w.kind = NamesRequest::Kind::Register;
    check(answer_read(ns, w).compare(0, 4, "ERR ") == 0, "answer_read refuses anything but a read");
}

}  // namespace

int main() {
    std::printf("test-names: the tracker's namespace, the state machine Raft replicates\n");
    test_names();
    test_commands();
    test_requests();
    test_list();
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
