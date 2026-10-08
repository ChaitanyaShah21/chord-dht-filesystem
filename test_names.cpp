// In-process tests for the tracker's namespace (names.cpp, decisions D-058, D-059).
//
// The namespace is driven by request lines, exactly as tracker.cpp drives it, and
// its log is a real file in a temporary directory, so restarts, torn records,
// damaged records and a disk that refuses writes can all be constructed on purpose.

#include "names.h"
#include "net.h"
#include "store.h"

#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

std::vector<std::string> g_tmpdirs;

std::string make_tmpdir() {
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/test-names-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    g_tmpdirs.emplace_back(buf.data());
    return g_tmpdirs.back();
}

int remove_entry(const char *path, const struct stat *, int, struct FTW *) { return std::remove(path); }

std::string slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void append(const std::string &path, const std::string &bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    f << bytes;
}

std::size_t lines_in(const std::string &path) {
    const std::string s = slurp(path);
    std::size_t n = 0;
    for (char c : s) n += c == '\n';
    return n;
}

std::string key(int i) { return chunk_key("manifest " + std::to_string(i)); }

// A namespace opened from a log, owned so a test can "restart" by opening another.
std::unique_ptr<Namespace> open_ns(const std::string &path, std::string &report) {
    auto ns = std::make_unique<Namespace>();
    std::string err;
    if (!names_open(*ns, path, report, err)) report = "OPEN FAILED: " + err;
    return ns;
}

void close_ns(Namespace &ns) {
    if (ns.log_fd >= 0) close(ns.log_fd);
    ns.log_fd = -1;
}

void test_names_and_records() {
    check(valid_name("a") && valid_name(std::string(200, 'x')) && valid_name("photos/2026/a.jpg") && valid_name("..."),
          "names of 1 to 200 visible characters, slashes and dots included");
    check(!valid_name("") && !valid_name(std::string(201, 'x')), "an empty name and a 201-byte name are refused");
    check(!valid_name("a b") && !valid_name("a\nb") && !valid_name("a\rb") && !valid_name(std::string("a\0b", 3)) &&
              !valid_name("a\x7f") && !valid_name("caf\xc3\xa9") && !valid_name("\xff"),
          "spaces, control characters, DEL and bytes above 0x7e are refused");

    const std::string r = names_record("doc.pdf", key(1));
    std::string n, k;
    check(parse_record(r, n, k) && n == "doc.pdf" && k == key(1), "a record survives being written and read back");
    std::string tampered = r;
    tampered[20] = tampered[20] == 'a' ? 'b' : 'a';          // a character of the key
    check(!parse_record(tampered, n, k), "a record with one character of its key changed fails its check");
    check(!parse_record(r.substr(0, r.size() - 1), n, k), "a record missing the last character of its check fails");
    check(!parse_record("REGISTER doc.pdf " + key(1) + " 00000000", n, k), "a record with a wrong check fails");
    check(!parse_record("REGISTER doc.pdf " + key(1), n, k), "a record with no check fails");
    check(!parse_record("0 create_user alice pw", n, k), "a record in the old tracker's format is not mistaken for one");
}

void test_requests() {
    const std::string dir = make_tmpdir();
    std::string report;
    auto ns = open_ns(dir + "/names.log", report);
    Namespace &s = *ns;

    check(handle_names_request(s, "PING") == "PONG", "PING");
    check(handle_names_request(s, "REGISTER a.txt " + key(1)) == "OK", "a new name is registered");
    check(handle_names_request(s, "RESOLVE a.txt") == "FILE " + key(1), "and resolves to its key");
    check(handle_names_request(s, "REGISTER a.txt " + key(1)) == "OK", "the same name and key again succeeds (a retried upload)");
    check(handle_names_request(s, "REGISTER a.txt " + key(2)) == "EXISTS " + key(1), "a different key for a taken name is refused, naming the key");
    check(handle_names_request(s, "RESOLVE a.txt") == "FILE " + key(1), "and the name still resolves to the first key");
    check(lines_in(dir + "/names.log") == 1, "only the first registration was logged");
    check(handle_names_request(s, "RESOLVE nope") == "MISSING", "an unknown name is MISSING");

    const std::vector<std::string> bad = {
        "", "REGISTER", "REGISTER a.txt", "REGISTER a.txt " + key(1) + " extra", "REGISTER a.txt " + std::string(40, 'A'),
        "REGISTER a.txt ../../etc/passwd", "REGISTER " + std::string(201, 'n') + " " + key(1), "REGISTER  a.txt " + key(1),
        "RESOLVE", "RESOLVE a b", "LIST a b", "LIST " + std::string(201, 'n'), "register a.txt " + key(1), "BOGUS", "PING x"};
    bool all_err = true;
    for (const std::string &b : bad) {
        const std::string reply = handle_names_request(s, b);
        if (reply.compare(0, 4, "ERR ") != 0 || reply.find('\n') != std::string::npos) {
            all_err = false;
            std::printf("    not refused cleanly: '%s' -> '%s'\n", b.substr(0, 60).c_str(), reply.c_str());
        }
    }
    check(all_err, "every malformed request is answered with one ERR line (" + std::to_string(bad.size()) + " cases)");
    check(s.names.size() == 1, "and none of them changed the namespace");
    close_ns(s);
}

void test_list() {
    const std::string dir = make_tmpdir();
    std::string report;
    auto ns = open_ns(dir + "/names.log", report);
    check(handle_names_request(*ns, "LIST") == "NAMES 0", "an empty namespace lists nothing");
    std::set<std::string> expect;
    for (int i = 0; i < 300; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file-%04d-%010d", i, i * 7919);
        expect.insert(name);
        handle_names_request(*ns, std::string("REGISTER ") + name + " " + key(i));
    }
    std::vector<std::string> got;
    std::string after;
    bool bounded = true;
    int pages = 0;
    for (; pages < 100; ++pages) {
        const std::string reply = handle_names_request(*ns, after.empty() ? "LIST" : "LIST " + after);
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
    check(got.size() == 300 && std::set<std::string>(got.begin(), got.end()) == expect &&
              std::is_sorted(got.begin(), got.end()),
          "every name exactly once, in order");
    check(handle_names_request(*ns, "LIST " + std::string(200, '~')) == "NAMES 0", "listing after the last name ends the listing");
    close_ns(*ns);
}

void test_persistence() {
    const std::string dir = make_tmpdir();
    const std::string log = dir + "/names.log";
    std::string report;
    {
        auto ns = open_ns(log, report);
        check(report.find("0 names replayed") != std::string::npos, "a missing log is an empty namespace: " + report);
        for (int i = 0; i < 3; ++i) handle_names_request(*ns, "REGISTER n" + std::to_string(i) + " " + key(i));
        close_ns(*ns);
    }
    {
        auto ns = open_ns(log, report);
        check(ns->names.size() == 3 && handle_names_request(*ns, "RESOLVE n2") == "FILE " + key(2),
              "three names survive a restart: " + report);
        check(handle_names_request(*ns, "REGISTER n1 " + key(9)) == "EXISTS " + key(1), "and are still written once after it");
        close_ns(*ns);
    }

    // A crash mid-append: the last record has no newline. It is reported, cut off,
    // and the next record starts on a line of its own.
    const std::size_t before = slurp(log).size();
    append(log, names_record("torn", key(5)).substr(0, 30));
    {
        auto ns = open_ns(log, report);
        check(ns->names.size() == 3 && report.find("torn") != std::string::npos && slurp(log).size() == before,
              "a torn final record is skipped and cut off the file: " + report);
        check(handle_names_request(*ns, "REGISTER after-torn " + key(6)) == "OK", "a registration after it succeeds");
        close_ns(*ns);
    }
    {
        auto ns = open_ns(log, report);
        check(ns->names.count("after-torn") == 1 && report.find("damaged") == std::string::npos,
              "and survives the next restart intact: " + report);
        close_ns(*ns);
    }

    // A record torn INSIDE its last word still ends in a newline here; its check fails.
    append(log, names_record("cut", key(7)).substr(0, 60) + "\n");
    // A damaged record in the middle, an old-format line, and a conflicting record.
    append(log, "0 create_user alice pw\n");
    append(log, names_record("n0", key(8)) + "\n");
    append(log, names_record("later", key(9)) + "\n");
    {
        auto ns = open_ns(log, report);
        check(ns->names.count("cut") == 0 && ns->names.count("later") == 1 && report.find("2 damaged") != std::string::npos,
              "a record cut inside its key, and an old-format line, are skipped: " + report);
        check(handle_names_request(*ns, "RESOLVE n0") == "FILE " + key(0) && report.find("1 conflicting") != std::string::npos,
              "a later record for a taken name does not replace the first");
        close_ns(*ns);
    }
}

void test_write_failures() {
    const std::string dir = make_tmpdir();
    std::string report;
    auto ns = open_ns(dir + "/names.log", report);
    close(ns->log_fd);
    ns->log_fd = open("/dev/full", O_WRONLY);            // every write fails: ENOSPC
    const std::string reply = handle_names_request(*ns, "REGISTER full " + key(1));
    check(reply.compare(0, 4, "ERR ") == 0, "a registration the disk refuses is answered ERR: " + reply);
    check(handle_names_request(*ns, "RESOLVE full") == "MISSING", "and is not applied: written ahead, or not at all");
    // /dev/full cannot be truncated either, so the log cannot be repaired: every
    // later registration is refused rather than risk an unreadable record.
    check(handle_names_request(*ns, "REGISTER other " + key(2)).find("unavailable") != std::string::npos,
          "once a failed append cannot be undone, later registrations are refused");
    check(handle_names_request(*ns, "PING") == "PONG", "reads still work");
    close_ns(*ns);
}

void test_concurrency() {
    const std::string dir = make_tmpdir();
    const std::string log = dir + "/names.log";
    std::string report;
    auto ns = open_ns(log, report);
    std::vector<std::thread> threads;
    std::vector<std::string> race(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i)
                handle_names_request(*ns, "REGISTER t" + std::to_string(t) + "-" + std::to_string(i) + " " + key(t * 1000 + i));
            race[t] = handle_names_request(*ns, "REGISTER contested " + key(5000 + t));
        });
    }
    for (std::thread &th : threads) th.join();
    int ok = 0, exists = 0;
    for (const std::string &r : race) {
        ok += r == "OK";
        exists += r.compare(0, 7, "EXISTS ") == 0;
    }
    check(ok == 1 && exists == 7, "8 threads register one name with 8 keys: exactly one wins");
    const std::string winner = ns->names["contested"];
    bool same = true;
    for (const std::string &r : race) same = same && (r == "OK" || r == "EXISTS " + winner);
    check(same, "and every loser is told the winner's key");
    check(lines_in(log) == 801, "800 names and the contested one: 801 records in the log");
    close_ns(*ns);
    auto again = open_ns(log, report);
    check(again->names.size() == 801 && again->names["contested"] == winner, "all 801 come back after a restart");
    close_ns(*again);
}

}  // namespace

int main() {
    std::printf("test-names: the tracker's namespace and its log\n");
    test_names_and_records();
    test_requests();
    test_list();
    test_persistence();
    test_write_failures();
    test_concurrency();
    for (const std::string &d : g_tmpdirs) nftw(d.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
