// In-process tests for Phase 4: the chunk store, body framing, member addressing,
// virtual nodes, replica placement, replicated writes, verified reads and repair.
//
// Several "processes" run in this one test process. Each has ring members (one
// NodeState per member, D-047) and, for the chunk tests, a ChunkNode with its own
// store in a temporary directory. A ring transport hands a request to the member
// it names; a frame transport hands a chunk request to the process at an address.
// A process marked dead refuses everything; one marked frozen times out.
//
// Oracles are written here from first principles -- clockwise distances and a
// brute-force walk for replica sets -- and share no code with what they check.

#include "chord.h"
#include "chunk.h"
#include "lookup.h"
#include "net.h"
#include "repair.h"
#include "stabilize.h"
#include "store.h"

#include <ftw.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
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

std::string addr(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }
Id cw(Id from, Id to) { return to - from; }    // clockwise distance, wraps modulo 2^64

// ---- temporary directories, removed at exit ---------------------------------

std::vector<std::string> g_tmpdirs;

std::string make_tmpdir() {
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/test-chunk-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { std::perror("mkdtemp"); std::exit(1); }
    g_tmpdirs.push_back(buf.data());
    return buf.data();
}

int remove_entry(const char *path, const struct stat *, int, struct FTW *) { return remove(path); }

void remove_tmpdirs() {
    for (const std::string &d : g_tmpdirs) nftw(d.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string random_bytes(std::mt19937_64 &rng, std::size_t n) {
    std::string s(n, '\0');
    for (char &c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}

// Regular files under `dir`, counted by a recursive walk. nftw takes a plain
// function pointer, so the count lives in a global.
int g_file_count = 0;
int count_one(const char *, const struct stat *, int flag, struct FTW *) {
    if (flag == FTW_F) ++g_file_count;
    return 0;
}
int count_files(const std::string &dir) {
    g_file_count = 0;
    nftw(dir.c_str(), count_one, 16, FTW_PHYS);
    return g_file_count;
}

// ---------------------------------------------------------------------------
// A ring of processes, each with several members, all in this one test process.

struct Process {
    std::string address;
    std::vector<std::unique_ptr<NodeState>> members;
    std::unique_ptr<ChunkStore> store;          // null for a ring-only test
    std::unique_ptr<ChunkNode> chunks;
};

struct Cluster {
    std::map<std::string, std::unique_ptr<Process>> procs;   // by address
    std::set<std::string> dead, frozen;                      // by address: a process fails as a whole
    std::set<std::string> slow;                              // chunk requests answered after 300 ms
    std::set<std::string> frozen_members;                    // "ip:port/<id>": one member times out
    std::vector<std::string> events;
    int r;
    Transport route;          // ring requests, to a member
    FrameTransport send;      // chunk requests, to a process

    explicit Cluster(int successors = DEFAULT_SUCCESSORS) : r(successors) {
        // Both capture `this`, so a Cluster must never be copied or moved.
        route = [this](const Peer &to, const std::string &request, std::string &reply) {
            return deliver(to, request, reply);
        };
        send = [this](const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
            return deliver_frame(to, line, body, reply);
        };
    }
    Cluster(const Cluster &) = delete;
    Cluster &operator=(const Cluster &) = delete;

    Delivery deliver(const Peer &to, const std::string &request, std::string &reply) {
        reply.clear();
        const std::string a = addr(to);
        auto it = procs.find(a);
        if (it == procs.end() || dead.count(a)) return Delivery::Refused;
        if (frozen.count(a)) return Delivery::Timeout;
        if (frozen_members.count(a + "/" + id_to_hex(to.id))) return Delivery::Timeout;
        for (auto &m : it->second->members) {
            if (m->self.id != to.id) continue;
            Handled h = handle_request(*m, request, route);
            reply = h.reply;
            if (h.after) h.after();
            return Delivery::Ok;
        }
        reply = "ERR no ring member " + id_to_hex(to.id) + " here";    // as the node answers
        return Delivery::Ok;
    }

    Delivery deliver_frame(const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
        const std::string a = addr(to);
        auto it = procs.find(a);
        if (it == procs.end() || dead.count(a)) return Delivery::Refused;
        if (frozen.count(a)) return Delivery::Timeout;
        if (slow.count(a)) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ChunkHandled h = handle_chunk_request(*it->second->chunks, Frame{line, body});
        reply.line = h.reply;
        reply.body = h.body;
        if (h.after) h.after();
        return Delivery::Ok;
    }

    // A process at 10.0.<n/256>.<n%256>:9000 with `v` members. Identifiers are
    // drawn from `rng` unless given, so tests can place members exactly.
    Process &add(int n, const std::vector<Id> &ids, bool with_store) {
        auto p = std::make_unique<Process>();
        p->address = "10.0." + std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255) + ":9000";
        const std::string ip = p->address.substr(0, p->address.find(':'));
        for (Id id : ids) {
            auto m = std::make_unique<NodeState>(Peer{id, ip, 9000}, r);
            const std::string who = p->address;
            m->log = [this, who](const std::string &e) { events.push_back(who + " " + e); };
            p->members.push_back(std::move(m));
        }
        if (with_store) {
            p->store = std::make_unique<ChunkStore>();
            std::string report, err;
            if (!store_open(*p->store, make_tmpdir(), report, err)) { std::printf("store_open: %s\n", err.c_str()); std::exit(1); }
            p->chunks = std::make_unique<ChunkNode>();
            p->chunks->store = p->store.get();
            p->chunks->send = send;
            for (auto &m : p->members) p->chunks->members.push_back(m.get());
        }
        Process &ref = *p;
        procs[ref.address] = std::move(p);
        return ref;
    }

    // Every member of every process, sorted by identifier.
    std::vector<Peer> all_members(bool live_only = false) const {
        std::vector<Peer> out;
        for (const auto &kv : procs) {
            if (live_only && dead.count(kv.first)) continue;
            for (const auto &m : kv.second->members) out.push_back(m->self);
        }
        std::sort(out.begin(), out.end(), [](const Peer &a, const Peer &b) { return a.id < b.id; });
        return out;
    }

    void wire() {
        const std::vector<Peer> all = all_members();
        for (auto &kv : procs)
            for (auto &m : kv.second->members) init_wired(*m, all);
    }

    // One maintenance round for every member of every running process.
    void round(std::mt19937_64 &rng) {
        std::vector<NodeState *> order;
        for (auto &kv : procs) {
            if (dead.count(kv.first) || frozen.count(kv.first)) continue;
            for (auto &m : kv.second->members) order.push_back(m.get());
        }
        std::shuffle(order.begin(), order.end(), rng);
        for (NodeState *m : order) maintenance_round(*m, route);
    }

    Process &at(const std::string &a) { return *procs.at(a); }

    // A member of a live process, to start lookups from. A test that kills processes
    // must not then start its lookups at one of them.
    Peer live_start() const {
        for (const auto &kv : procs)
            if (!dead.count(kv.first) && !frozen.count(kv.first)) return kv.second->members[0]->self;
        return Peer{};
    }
};

// Build a cluster of `p` processes with `v` random members each.
void build(Cluster &c, std::mt19937_64 &rng, int p, int v, bool with_store) {
    std::set<Id> used;
    for (int i = 1; i <= p; ++i) {
        std::vector<Id> ids;
        for (int j = 0; j < v; ++j) {
            Id id;
            do { id = rng(); } while (used.count(id));
            used.insert(id);
            ids.push_back(id);
        }
        c.add(i, ids, with_store);
    }
    c.wire();
}

// The ORACLE for replica sets (D-048), from first principles: walk every live
// member clockwise from k, starting with k's owner, and take a member only if no
// member of its process is taken yet.
std::vector<std::string> expected_holders(const std::vector<Peer> &live_sorted, Id k, int rf) {
    std::vector<Peer> order = live_sorted;
    std::sort(order.begin(), order.end(), [&](const Peer &a, const Peer &b) { return cw(k, a.id) < cw(k, b.id); });
    std::vector<std::string> out;
    for (const Peer &p : order) {
        if (static_cast<int>(out.size()) >= rf) break;
        if (std::find(out.begin(), out.end(), addr(p)) == out.end()) out.push_back(addr(p));
    }
    return out;
}

// Is every member's successor and predecessor right, for the live members?
bool ring_correct(Cluster &c) {
    const std::vector<Peer> live = c.all_members(true);
    for (std::size_t i = 0; i < live.size(); ++i) {
        const Peer &me = live[i];
        const Peer &succ = live[(i + 1) % live.size()];
        const Peer &pred = live[(i + live.size() - 1) % live.size()];
        Process &p = c.at(addr(me));
        for (auto &m : p.members) {
            if (m->self.id != me.id) continue;
            std::lock_guard<std::mutex> lock(m->mu);
            if (!same_node(m->succs[0], succ)) return false;
            if (live.size() > 1 && (!m->has_pred || !same_node(m->pred, pred))) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The store

void test_keys() {
    std::printf("keys are validated before they reach the filesystem\n");
    const std::string good = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    check(valid_key(good), "40 lowercase hexadecimal characters are a key");
    check(chunk_key("") == good, "the empty chunk has SHA-1 da39a3ee...");
    const std::vector<std::string> bad = {
        "", good.substr(0, 39), good + "0", "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709",
        "../../../../../../../../../../etc/passwd", "da39a3ee5e6b4b0d3255bfef95601890afd8070/",
        "da39a3ee5e6b4b0d3255bfef95601890afd8070g", std::string("da39a3ee5e6b4b0d3255bfef95601890afd8070") + '\0',
        "da39a3ee5e6b4b0d3255bfef95601890afd8070 ", "..39a3ee5e6b4b0d3255bfef95601890afd80709",
    };
    int refused = 0;
    for (const std::string &k : bad) refused += valid_key(k) ? 0 : 1;
    check(refused == static_cast<int>(bad.size()), "every malformed key is refused (" + std::to_string(refused) +
                                                   "/" + std::to_string(bad.size()) + ")");

    ChunkStore s;
    std::string report, err;
    const std::string dir = make_tmpdir();
    check(store_open(s, dir, report, err), "a store opens in an empty directory");
    // Exactly 40 characters, so only the character check stands between this key and
    // <dir>/../../../../../../../../../../etc/passwd. (A shorter traversal key would be
    // refused by its length alone and prove nothing about the character check -- the
    // first version of this test used one, and a mutant accepting any 40 characters
    // survived it.)
    const std::string traversal = "../../../../../../../../../../etc/passwd";
    check(traversal.size() == 40, "the traversal key is exactly 40 characters");
    check(store_put(s, traversal, "x") == PutResult::BadKey, "a 40-character traversal key is refused by put");
    std::string out;
    check(store_get(s, traversal, out) == GetResult::BadKey && out.empty(),
          "a 40-character traversal key is refused by get, and nothing is read");
    check(count_files(dir) == 0, "and nothing was written anywhere under the store");
}

void test_store_basics() {
    std::printf("store: put, get, sizes, duplicates, mismatches\n");
    std::mt19937_64 rng(1);
    ChunkStore s;
    std::string report, err;
    const std::string dir = make_tmpdir();
    store_open(s, dir, report, err);

    const std::string empty = "";
    check(store_put(s, chunk_key(empty), empty) == PutResult::Stored, "a zero-byte chunk is stored");
    std::string out = "junk";
    check(store_get(s, chunk_key(empty), out) == GetResult::Found && out.empty(), "and read back as zero bytes");

    const std::string exact = random_bytes(rng, MAX_CHUNK);
    check(store_put(s, chunk_key(exact), exact) == PutResult::Stored, "a chunk of exactly 512 KiB is stored");
    check(store_get(s, chunk_key(exact), out) == GetResult::Found && out == exact, "and read back byte for byte");

    const std::string over = random_bytes(rng, MAX_CHUNK + 1);
    check(store_put(s, chunk_key(over), over) == PutResult::TooLarge, "512 KiB + 1 is refused");
    check(!store_has(s, chunk_key(over)), "and is not in the index");

    const std::string a = random_bytes(rng, 1000), b = random_bytes(rng, 1000);
    check(store_put(s, chunk_key(a), b) == PutResult::HashMismatch, "bytes that do not hash to the key are refused");
    check(store_get(s, chunk_key(a), out) == GetResult::Missing, "and nothing is stored under that key");

    check(store_put(s, chunk_key(a), a) == PutResult::Stored, "a chunk is stored once");
    check(store_put(s, chunk_key(a), a) == PutResult::AlreadyHeld, "a duplicate put is a no-op (content addressing)");
    check(count_files(dir) == 3, "three chunks, three files, no temporary files left (" +
                                 std::to_string(count_files(dir)) + ")");

    std::ifstream f(store_path(s, chunk_key(a)), std::ios::binary);
    std::string on_disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(on_disk == a, "the file at <dir>/<2 hex>/<key> holds exactly the chunk");
    check(store_path(s, chunk_key(a)) == dir + "/" + chunk_key(a).substr(0, 2) + "/" + chunk_key(a),
          "the path is the fan-out directory, then the key");
}

void test_store_restart() {
    std::printf("store: a restart reloads the index and removes unfinished writes\n");
    std::mt19937_64 rng(2);
    const std::string dir = make_tmpdir();
    std::vector<std::string> keys;
    {
        ChunkStore s;
        std::string report, err;
        store_open(s, dir, report, err);
        for (int i = 0; i < 20; ++i) {
            const std::string c = random_bytes(rng, 100 + i);
            store_put(s, chunk_key(c), c);
            keys.push_back(chunk_key(c));
        }
    }
    // What a crash between write and rename leaves: a temporary file. And a file
    // that is not ours at all.
    std::ofstream(dir + "/tmp/" + keys[0] + ".7") << "half a chunk";
    std::ofstream(dir + "/ab/not-a-key") << "x";

    ChunkStore again;
    std::string report, err;
    check(store_open(again, dir, report, err), "the store reopens");
    check(store_keys(again).size() == 20, "every chunk is in the index again (" + report + ")");
    check(count_files(dir + "/tmp") == 0, "the unfinished write was removed");
    check(report.find("1 unfinished") != std::string::npos && report.find("1 other") != std::string::npos,
          "the report counts the removed write and the foreign file: " + report);
    std::string out;
    check(store_get(again, keys[5], out) == GetResult::Found && chunk_key(out) == keys[5], "chunks read back intact");
}

void test_store_concurrent() {
    std::printf("store: many threads writing the same chunk at once\n");
    std::mt19937_64 rng(3);
    ChunkStore s;
    std::string report, err;
    const std::string dir = make_tmpdir();
    store_open(s, dir, report, err);
    const std::string c = random_bytes(rng, 64 * 1024);
    const std::string key = chunk_key(c);
    std::vector<PutResult> results(8);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back([&, i] { results[i] = store_put(s, key, c); });
    for (auto &t : threads) t.join();
    int ok = 0;
    for (PutResult r : results) ok += (r == PutResult::Stored || r == PutResult::AlreadyHeld) ? 1 : 0;
    check(ok == 8, "every concurrent write of the same chunk succeeds");
    std::string out;
    check(store_get(s, key, out) == GetResult::Found && out == c, "the file holds the chunk intact");
    check(count_files(dir) == 1 && store_keys(s).size() == 1, "one file, one index entry, no temporary files");
}

void test_store_arcs() {
    std::printf("store: keys in an arc, including one that wraps past zero\n");
    ChunkStore s;
    std::string report, err;
    store_open(s, make_tmpdir(), report, err);
    std::mt19937_64 rng(4);
    std::vector<std::string> keys;
    for (int i = 0; i < 200; ++i) {
        const std::string c = random_bytes(rng, 10);
        store_put(s, chunk_key(c), c);
        keys.push_back(chunk_key(c));
    }
    const Id from = 0xf000000000000000ULL, to = 0x1000000000000000ULL;   // wraps
    int expected = 0;
    for (const std::string &k : keys) {
        Id id;
        id_from_key(k, id);
        expected += (id > from || id <= to) ? 1 : 0;
    }
    check(static_cast<int>(store_keys_in_arc(s, from, to).size()) == expected,
          "a wrapping arc returns exactly the keys past `from` or up to `to`");
    check(store_keys_in_arc(s, 5, 5).size() == 200, "(n, n] is the whole ring");
}

// ---------------------------------------------------------------------------
// Body framing and member addressing

void test_body_length() {
    std::printf("body framing: which lines carry a body, and how long\n");
    std::size_t n = 99;
    check(body_length("GET abc", n) == Body::None && n == 99, "GET carries no body, and the length is untouched");
    check(body_length("HAVE abc", n) == Body::None, "HAVE carries no body");
    check(body_length("CHUNK 0", n) == Body::Present && n == 0, "CHUNK 0 is a zero-byte body");
    check(body_length("CHUNK 524288", n) == Body::Present && n == 524288, "exactly 512 KiB is allowed");
    check(body_length("CHUNK 524289", n) == Body::TooLarge, "512 KiB + 1 is too large");
    check(body_length("PUT k 2 17", n) == Body::Present && n == 17, "PUT: the last word is the length");
    check(body_length("STORE k 5", n) == Body::Present && n == 5, "STORE: the last word is the length");
    const std::vector<std::string> malformed = {"CHUNK", "CHUNK ", "CHUNK 007", "CHUNK -1", "CHUNK +1", "CHUNK 1x",
                                                "CHUNK 12345678", "PUT k 2 5 ", "STORE k"};
    int bad = 0;
    for (const std::string &l : malformed) bad += body_length(l, n) == Body::Malformed ? 1 : 0;
    // "STORE k": its last word is "k", which is not a number.
    check(bad == static_cast<int>(malformed.size()), "every malformed length is refused (" + std::to_string(bad) + "/" +
                                                     std::to_string(malformed.size()) + ")");
}

void test_split_target() {
    std::printf("member addressing: TO <identifier> <request>\n");
    Id id = 0;
    std::string rest;
    check(split_target("TO 0123456789abcdef PING", id, rest) == Target::Member && id == 0x0123456789abcdefULL &&
          rest == "PING", "a prefixed request names its member and the rest is the request");
    check(split_target("PING", id, rest) == Target::None && rest == "PING", "no prefix: the whole line, for member 0");
    const std::vector<std::string> bad = {"TO ", "TO 0123 PING", "TO 0123456789abcdef", "TO 0123456789abcdef ",
                                          "TO 0123456789abcdefPING", "TO 0123456789abcdeg PING", "TO  0123456789abcde PING"};
    int refused = 0;
    for (const std::string &l : bad) refused += split_target(l, id, rest) == Target::Malformed ? 1 : 0;
    check(refused == static_cast<int>(bad.size()), "every malformed prefix is refused (" + std::to_string(refused) +
                                                   "/" + std::to_string(bad.size()) + ")");
    // A finger that is another member of this same process is a real next hop: same
    // address, different identifier (D-047). Treating it as "self" would fall back to the
    // successor -- still correct, only slower, which is why only a direct check sees it.
    {
        const Peer self{1000, "10.0.0.1", 9000}, succ{2000, "10.0.0.2", 9000}, sibling{5000, "10.0.0.1", 9000};
        std::vector<Peer> fingers(ID_BITS, sibling);            // the farthest fingers name the sibling
        for (int i = 0; i < 12; ++i) fingers[i] = succ;
        const RouteStep step = route_step(self, succ, fingers, 9000);
        check(!step.owner && step.peer.id == 5000, "route_step jumps to a sibling member, not back to the successor");
    }
    const Peer p{42, "10.0.0.1", 9000};
    check(addressed(p, "NEIGHBOURS") == "TO 000000000000002a NEIGHBOURS", "addressed() writes the prefix");
    check(id_of_member("127.0.0.1", 7000, 0) == id_of_address("127.0.0.1", 7000), "member 0 keeps its old identifier");
    check(id_of_member("127.0.0.1", 7000, 1) != id_of_member("127.0.0.1", 7000, 2), "members 1 and 2 differ");
}

// Real sockets: a short read in the middle of a body, and a peer that hangs up.
void test_short_reads() {
    std::printf("body framing over a real socket: short reads and a peer that hangs up\n");
    std::mt19937_64 rng(5);
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    const std::string body = random_bytes(rng, 300000);
    // The writer sends the header, then the body in uneven pieces with pauses, so
    // the reader's recv calls return part of what it asked for.
    std::thread writer([&] {
        send_line(sv[0], "CHUNK " + std::to_string(body.size()));
        std::size_t at = 0, piece = 1;
        while (at < body.size()) {
            const std::size_t n = std::min(piece, body.size() - at);
            send_all(sv[0], body.data() + at, n);
            at += n;
            piece = piece * 3 + 7;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::string line, got;
    std::size_t n = 0;
    const bool ok = recv_line(sv[1], line) && body_length(line, n) == Body::Present && recv_exact(sv[1], n, got);
    writer.join();
    check(ok && got == body, "a body that arrives in many short pieces is reassembled exactly");
    close(sv[0]);
    close(sv[1]);

    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    send_line(sv[0], "CHUNK 1000");
    send_all(sv[0], body.data(), 400);       // 400 of the 1000 bytes, then the peer dies
    close(sv[0]);
    const bool got_line = recv_line(sv[1], line);
    const bool full = body_length(line, n) == Body::Present && recv_exact(sv[1], n, got);
    check(got_line && !full, "a peer that hangs up mid-body is a failed transfer, not a short chunk");
    check(errno == 0, "and it reads as a close (errno 0), not a timeout");
    close(sv[1]);
}

// ---------------------------------------------------------------------------
// Virtual nodes in the ring

void test_vnode_ring() {
    std::printf("virtual nodes: a ring of processes with several members each\n");
    std::mt19937_64 rng(6);
    Cluster c;
    build(c, rng, 8, 4, false);
    check(ring_correct(c), "a wired ring of 8 processes x 4 members starts correct");

    // A request for a member a process does not have is ERR, not some other member's answer.
    std::string reply;
    const Peer ghost{12345, c.procs.begin()->second->members[0]->self.ip, 9000};
    c.route(ghost, "PING", reply);
    check(reply.compare(0, 3, "ERR") == 0, "a request for an unknown member is answered ERR");

    // Kill a process: every one of its members must leave every table, at once.
    const std::string victim = std::next(c.procs.begin(), 3)->first;
    c.dead.insert(victim);
    int rounds = 0;
    while (!ring_correct(c) && rounds < 10) { c.round(rng); ++rounds; }
    check(ring_correct(c), "after a process dies the ring is correct again (" + std::to_string(rounds) + " rounds)");
    check(rounds <= 2, "within two rounds, though four members died at once");
    // Lookups use the client's recovery (D-036): a finger elsewhere in the ring can
    // still name a dead member until that node's own maintenance reaches it.
    const std::vector<Peer> live = c.all_members(true);
    int wrong = 0;
    for (int i = 0; i < 200; ++i) {
        const Id k = rng();
        Peer best = live[0];
        for (const Peer &p : live) if (cw(k, p.id) < cw(k, best.id)) best = p;
        const LookupResult r = lookup_with_recovery(live[i % live.size()], k, c.route, 3);
        if (!r.ok() || !same_node(r.owner, best)) ++wrong;
    }
    check(wrong == 0, "every lookup after the death reaches the oracle's owner (" + std::to_string(wrong) + " wrong)");

    // Eventually no table anywhere names a member of the dead process.
    auto mentions_victim = [&] {
        for (auto &kv : c.procs) {
            if (c.dead.count(kv.first)) continue;
            for (auto &m : kv.second->members) {
                std::lock_guard<std::mutex> lock(m->mu);
                for (const Peer &p : m->succs) if (addr(p) == victim) return true;
                for (const Peer &p : m->fingers) if (addr(p) == victim) return true;
                if (m->has_pred && addr(m->pred) == victim) return true;
            }
        }
        return false;
    };
    int more = 0;
    while (mentions_victim() && more < 200) { c.round(rng); ++more; }
    check(!mentions_victim(), "after " + std::to_string(more) + " more rounds no successor list, finger or predecessor "
                              "names any member of the dead process");
}

void test_vnode_siblings() {
    std::printf("virtual nodes: a member never suspects or evicts its own process\n");
    std::mt19937_64 rng(7);
    Cluster c;
    // Two processes, three members each, interleaved so siblings are adjacent.
    c.add(1, {100, 200, 900}, false);
    c.add(2, {300, 400, 500}, false);
    c.wire();
    check(ring_correct(c), "the interleaved ring starts correct");
    NodeState &m100 = *c.at("10.0.0.1:9000").members[0];
    {
        std::lock_guard<std::mutex> lock(m100.mu);
        check(m100.succs[0].id == 200 && same_process(m100.succs[0], m100.self),
              "member 100's successor is its own sibling 200");
    }
    // Freeze process 2: its members time out. Process 1's members strike them, and
    // after two strikes each is evicted -- but the siblings stay.
    c.frozen.insert("10.0.0.2:9000");
    for (int i = 0; i < 4; ++i) c.round(rng);
    c.frozen.clear();
    {
        std::lock_guard<std::mutex> lock(m100.mu);
        bool sibling_kept = false;
        for (const Peer &p : m100.succs) sibling_kept = sibling_kept || p.id == 200;
        check(sibling_kept, "the sibling is still in the successor list after its neighbours were evicted");
    }
    int strikes_on_self = 0;
    for (const std::string &e : c.events)
        if (e.find("10.0.0.1:9000 strike") == 0 && e.find("against 10.0.0.1:9000") != std::string::npos) ++strikes_on_self;
    check(strikes_on_self == 0, "no strike was ever counted against our own process");
    for (int i = 0; i < 6; ++i) c.round(rng);
    check(ring_correct(c), "after the thaw the frozen process's members re-insert themselves");

    // A call to our own sibling that times out -- our own process is overloaded -- is
    // evidence against ourselves, not the sibling: no strike, and never an eviction.
    c.frozen_members.insert("10.0.0.1:9000/" + id_to_hex(200));
    c.events.clear();
    for (int i = 0; i < 4; ++i) c.round(rng);
    c.frozen_members.clear();
    int own = 0;
    for (const std::string &e : c.events)
        if (e.compare(0, 13, "10.0.0.1:9000") == 0 && e.find("against 10.0.0.1:9000") != std::string::npos) ++own;
    {
        std::lock_guard<std::mutex> lock(m100.mu);
        check(own == 0 && m100.succs[0].id == 200,
              "a sibling that times out is neither struck nor evicted (" + std::to_string(own) + " strikes)");
    }
}

void test_vnode_frozen_member_vs_dead_process() {
    std::printf("virtual nodes: a refusal evicts a whole process; a timeout strikes one member\n");
    std::mt19937_64 rng(8);
    Cluster c;
    build(c, rng, 6, 3, false);
    const std::string victim = std::next(c.procs.begin(), 2)->first;
    c.dead.insert(victim);
    c.round(rng);
    int whole = 0, member_lines = 0;
    for (const std::string &e : c.events) {
        if (e.find("evict " + victim + " (refused)") != std::string::npos) {
            ++whole;
            if (e.find(" member ") != std::string::npos) ++member_lines;
        }
    }
    check(whole > 0 && member_lines == 0, "a refusal is logged as evicting the process, not one member");

    // A frozen process: each of its members is struck on its own, and evicted on its
    // own second strike -- the eviction names the member.
    const std::string frozen = std::next(c.procs.begin(), 4)->first;
    c.frozen.insert(frozen);
    c.events.clear();
    for (int i = 0; i < 4; ++i) c.round(rng);
    int by_member = 0, by_process = 0;
    for (const std::string &e : c.events) {
        if (e.find("evict " + frozen + " (timeout") == std::string::npos) continue;
        if (e.find(" member ") != std::string::npos) ++by_member;
        else ++by_process;
    }
    check(by_member > 0 && by_process == 0, "a timeout evicts one member at a time, by its second strike (" +
                                            std::to_string(by_member) + " member evictions)");
    c.frozen.clear();
}

// ---------------------------------------------------------------------------
// Replica placement

void test_replica_sets() {
    std::printf("replica placement: three distinct processes, never two copies in one\n");
    // Construct the worst case by hand: member 100's next three members are its own
    // siblings, then process 2 twice, then process 3.
    Cluster c;
    c.add(1, {100, 110, 120, 130}, false);
    c.add(2, {200, 210}, false);
    c.add(3, {300}, false);
    c.wire();
    NodeState &owner = *c.at("10.0.0.1:9000").members[0];
    const std::vector<Peer> set = replica_set(owner);
    check(set.size() == 3, "three replicas");
    check(set.size() == 3 && same_node(set[0], owner.self) && addr(set[1]) == "10.0.0.2:9000" &&
          addr(set[2]) == "10.0.0.3:9000", "the owner, then the first member of each other process clockwise");

    // A ring of two processes cannot hold three copies, and does not pretend to.
    Cluster two;
    two.add(1, {1, 3}, false);
    two.add(2, {2, 4}, false);
    two.wire();
    check(replica_set(*two.at("10.0.0.1:9000").members[0]).size() == 2, "two processes: a set of two, not a duplicate");

    // Property: on random rings, every member's set equals the brute-force oracle's
    // for a key it owns, whenever the successor list reaches far enough.
    std::mt19937_64 rng(9);
    int compared = 0, mismatched = 0, short_sets = 0;
    for (int trial = 0; trial < 30; ++trial) {
        Cluster r;
        const int p = 3 + static_cast<int>(rng() % 14), v = 1 + static_cast<int>(rng() % 8);
        build(r, rng, p, v, false);
        const std::vector<Peer> all = r.all_members();
        for (auto &kv : r.procs) {
            for (auto &m : kv.second->members) {
                const std::vector<Peer> got = replica_set(*m);
                const std::vector<std::string> want = expected_holders(all, m->self.id, RF);
                std::vector<std::string> got_addr;
                for (const Peer &q : got) got_addr.push_back(addr(q));
                ++compared;
                if (got_addr.size() < want.size()) ++short_sets;          // the list of 8 ran out
                else if (got_addr != want) ++mismatched;
                std::set<std::string> distinct(got_addr.begin(), got_addr.end());
                if (distinct.size() != got_addr.size()) ++mismatched;     // a duplicate process: never allowed
            }
        }
    }
    check(mismatched == 0, "every replica set equals the oracle's and has no repeated process (" +
                           std::to_string(compared) + " sets, " + std::to_string(mismatched) + " wrong)");
    std::printf("  info: %d of %d sets were short because 8 successors held fewer than 3 processes\n",
                short_sets, compared);
}

// ---------------------------------------------------------------------------
// Writes, reads and the body cap

void test_writes() {
    std::printf("writes: the owner coordinates, W counts confirmed copies\n");
    std::mt19937_64 rng(10);
    Cluster c;
    build(c, rng, 6, 2, true);
    const Peer start = c.procs.begin()->second->members[0]->self;

    const std::string chunk = random_bytes(rng, 200000);
    const std::string key = chunk_key(chunk);
    const PutOutcome p = put_chunk(start, chunk, 2, c.route, c.send);
    check(p.ok && p.copies >= 2, "a W = 2 write is acknowledged with at least 2 copies: " + p.message);

    Id k;
    id_from_key(key, k);
    const std::vector<std::string> want = expected_holders(c.all_members(), k, RF);
    int holders = 0, extra = 0;
    for (auto &kv : c.procs) {
        const bool has = store_has(*kv.second->store, key);
        const bool should = std::find(want.begin(), want.end(), kv.first) != want.end();
        holders += has && should ? 1 : 0;
        extra += has && !should ? 1 : 0;
    }
    check(holders == 3 && extra == 0, "after the write, exactly the oracle's three processes hold it");

    const PutOutcome again = put_chunk(start, chunk, 2, c.route, c.send);
    check(again.ok, "writing the same chunk again succeeds: content addressing makes it idempotent");
    int files = 0;
    for (auto &kv : c.procs) files += store_keys(*kv.second->store).size();
    check(files == 3, "and still exactly three copies exist (" + std::to_string(files) + ")");

    // A body that does not hash to the key it is sent under: refused, stored nowhere.
    const std::string other = random_bytes(rng, 1000);
    Frame answer;
    const std::string owner_addr = want[0];
    Peer owner{0, owner_addr.substr(0, owner_addr.find(':')), 9000};
    c.send(owner, "PUT " + key.substr(0, 39) + "0 2 " + std::to_string(other.size()), other, answer);
    check(answer.line.compare(0, 3, "ERR") == 0, "a PUT whose bytes do not hash to its key is refused: " + answer.line);
    c.send(owner, "PUT " + chunk_key(random_bytes(rng, 5)) + " 2 " + std::to_string(other.size()), other, answer);
    check(answer.line.find("do not hash") != std::string::npos, "with a reason naming the hash");
    files = 0;
    for (auto &kv : c.procs) files += store_keys(*kv.second->store).size();
    check(files == 3, "and nothing new was stored anywhere");

    // W against failures: one replica dead.
    const std::string c2 = random_bytes(rng, 1000);
    Id k2;
    id_from_key(chunk_key(c2), k2);
    const std::vector<std::string> set2 = expected_holders(c.all_members(), k2, RF);
    c.dead.insert(set2[2]);
    // The owner answers as soon as three copies can no longer be reached, so it may
    // report one or two confirmed: the third copy can still be in flight.
    const PutOutcome w3 = put_chunk(c.live_start(), c2, 3, c.route, c.send);
    check(w3.attempts == 1, "a live owner's ERR is final: no other coordinator is tried (" +
                            std::to_string(w3.attempts) + ")");
    check(!w3.ok && w3.message.find("ERR only") != std::string::npos && w3.message.find("of 3 copies") != std::string::npos &&
          w3.message.find(set2[2] + " refused") != std::string::npos,
          "W = 3 with a replica dead fails, naming the dead replica: " + w3.message);
    const PutOutcome w2 = put_chunk(c.live_start(), c2, 2, c.route, c.send);
    check(w2.ok && w2.copies == 2, "W = 2 with the same replica dead succeeds with 2 copies");
    c.dead.clear();

    const std::string c3 = random_bytes(rng, 1000);
    Id k3;
    id_from_key(chunk_key(c3), k3);
    const std::vector<std::string> set3 = expected_holders(c.all_members(), k3, RF);
    c.dead.insert(set3[1]);
    c.dead.insert(set3[2]);
    check(!put_chunk(c.live_start(), c3, 2, c.route, c.send).ok, "W = 2 with both other replicas dead fails loudly");
    const PutOutcome w1 = put_chunk(c.live_start(), c3, 1, c.route, c.send);
    check(w1.ok && w1.copies == 1, "W = 1 succeeds with one copy -- the risk W = 1 accepts: " + w1.message +
                                   " copies " + std::to_string(w1.copies));
    c.dead.clear();

    // One replica slow: W = 2 must answer on the faster copy, not wait for the slow one
    // (D-015's arithmetic: the minimum of two round trips, not the maximum).
    const std::string c5 = random_bytes(rng, 1000);
    Id k5;
    id_from_key(chunk_key(c5), k5);
    const std::vector<std::string> set5 = expected_holders(c.all_members(), k5, RF);
    c.slow.insert(set5[2]);
    const auto t5 = std::chrono::steady_clock::now();
    const PutOutcome s5 = put_chunk(c.live_start(), c5, 2, c.route, c.send);
    const long long ms5 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t5).count();
    c.slow.clear();
    check(s5.ok && s5.copies == 2, "W = 2 with one slow replica is acknowledged on 2 copies, not 3 (" +
                                   std::to_string(s5.copies) + ")");
    (void)ms5;     // in this harness the reply returns only after `after` has run, so no timing claim

    // The owner itself is dead and not yet evicted: the write falls through. Start at the
    // member just before the key: it answers OWNER <dead owner> itself, so routing never
    // meets the dead process and the PUT to it is what fails. Draw chunks until that
    // member belongs to a different process from the owner.
    std::string c4;
    Id k4 = 0;
    std::vector<std::string> set4;
    Peer before;
    const std::vector<Peer> all = c.all_members();
    for (int tries = 0; tries < 100; ++tries) {
        c4 = random_bytes(rng, 1000);
        id_from_key(chunk_key(c4), k4);
        set4 = expected_holders(all, k4, RF);
        before = all.back();
        for (const Peer &p : all) if (cw(p.id, k4) != 0 && cw(p.id, k4) < cw(before.id, k4)) before = p;
        if (addr(before) != set4[0]) break;
    }
    {
        c.dead.insert(set4[0]);
        const Peer from = before;
        const PutOutcome f = put_chunk(from, c4, 2, c.route, c.send);
        // Either the lookup itself met the dead owner and recovered (D-036), or the PUT to it
        // was refused and the client fell through (D-046); either way two live copies exist.
        int live_copies = 0;
        for (auto &kv : c.procs)
            if (!c.dead.count(kv.first) && store_has(*kv.second->store, chunk_key(c4))) ++live_copies;
        check(f.attempts >= 2, "a PUT refused by the dead owner falls through to the next replica (" +
                                   std::to_string(f.attempts) + " coordinators tried)");
        check(f.ok && live_copies >= 2, "with the owner dead, the write still lands on two live processes (" +
                                        std::to_string(live_copies) + ", " + std::to_string(f.attempts) +
                                        " coordinator(s) tried): " + f.message);
        c.dead.clear();
    }
}

void test_reads() {
    std::printf("reads: R = 1, verified locally, falling through on anything wrong\n");
    std::mt19937_64 rng(11);
    Cluster c;
    build(c, rng, 6, 2, true);
    const Peer start = c.procs.begin()->second->members[0]->self;
    const std::string chunk = random_bytes(rng, 100000);
    const std::string key = chunk_key(chunk);
    put_chunk(start, chunk, 3, c.route, c.send);
    Id k;
    id_from_key(key, k);
    const std::vector<std::string> set = expected_holders(c.all_members(), k, RF);

    GetOutcome g = get_chunk(start, key, c.route, c.send);
    check(g.ok && g.body == chunk && addr(g.served_by) == set[0], "a healthy read is served by the owner");

    // Corrupt the owner's copy on disk: one byte flipped.
    Process &owner = c.at(set[0]);
    {
        std::fstream f(store_path(*owner.store, key), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(5000);
        f.put('\x5a' ^ chunk[5000]);
    }
    g = get_chunk(start, key, c.route, c.send);
    check(g.ok && g.body == chunk && addr(g.served_by) == set[1],
          "a corrupt owner copy is detected by the reader's hash and the read falls through to the next replica");
    check(g.tried.size() == 2 && g.tried[0].find("do not hash") != std::string::npos,
          "and the record says why: " + (g.tried.empty() ? std::string("") : g.tried[0]));

    // Owner dead and not yet evicted.
    c.dead.insert(set[0]);
    g = get_chunk(c.live_start(), key, c.route, c.send);
    check(g.ok && g.body == chunk, "with the owner dead, the read is served by another replica");
    c.dead.clear();

    // Every replica wrong: the read fails and names each one.
    for (const std::string &a : set) {
        std::ofstream f(store_path(*c.at(a).store, key), std::ios::binary | std::ios::trunc);
        f << "garbage";
    }
    g = get_chunk(start, key, c.route, c.send);
    check(!g.ok && g.tried.size() == 3, "every replica corrupt: the read fails after trying all three");
    check(!g.ok && g.message.find(set[2]) != std::string::npos, "and the failure names each replica: " + g.message);

    g = get_chunk(start, chunk_key("never stored"), c.route, c.send);
    check(!g.ok && g.message.find("MISSING") != std::string::npos, "a chunk nobody has is MISSING everywhere");
}

void test_body_cap() {
    std::printf("memory: at most 32 bodies at once per process\n");
    ChunkNode n;
    std::vector<std::shared_ptr<void>> held;
    for (int i = 0; i < MAX_BODIES; ++i) held.push_back(hold_body(n));
    bool all = true;
    for (auto &h : held) all = all && h != nullptr;
    check(all && n.bodies == MAX_BODIES, "32 slots are granted");
    check(hold_body(n) == nullptr && n.bodies == MAX_BODIES, "the 33rd is refused and the count is unchanged");
    held.pop_back();
    check(n.bodies == MAX_BODIES - 1, "releasing a slot gives it back");
    check(hold_body(n) != nullptr, "and it can be taken again");
    held.clear();

    // Many threads racing for the slots: no more than 32 may ever be HELD at once. The
    // counter itself may briefly read above 32 -- a refused thread increments it before it
    // sees it is over, then gives the increment back -- so what is checked is the number of
    // slots granted and not yet released, counted by the test itself.
    std::atomic<int> holding{0}, peak{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 64; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                auto h = hold_body(n);
                if (!h) continue;
                const int now = ++holding;
                int p = peak.load();
                while (now > p && !peak.compare_exchange_weak(p, now)) {}
                --holding;                       // before `h` is released at the end of the iteration
            }
        });
    }
    for (auto &t : threads) t.join();
    check(peak <= MAX_BODIES && n.bodies == 0, "64 racing threads never hold more than 32 and leave 0 held (peak " +
                                               std::to_string(peak.load()) + ")");
}

// ---------------------------------------------------------------------------
// Repair

// Does every chunk sit on all of its correct live processes?
int under_replicated(Cluster &c, const std::vector<std::string> &keys) {
    const std::vector<Peer> live = c.all_members(true);
    int bad = 0;
    for (const std::string &key : keys) {
        Id k;
        id_from_key(key, k);
        for (const std::string &a : expected_holders(live, k, RF))
            if (!store_has(*c.at(a).store, key)) { ++bad; break; }
    }
    return bad;
}

void test_repair() {
    std::printf("repair: a process dies, and every chunk gets back to three copies\n");
    std::mt19937_64 rng(12);
    Cluster c;
    build(c, rng, 8, 3, true);
    const Peer start = c.procs.begin()->second->members[0]->self;
    std::vector<std::string> keys;
    for (int i = 0; i < 60; ++i) {
        const std::string chunk = random_bytes(rng, 2000 + i);
        if (put_chunk(start, chunk, 3, c.route, c.send).ok) keys.push_back(chunk_key(chunk));
    }
    check(keys.size() == 60 && under_replicated(c, keys) == 0, "60 chunks written with W = 3, all fully placed");

    std::map<std::string, RepairState> states;
    auto repair_all = [&](int scan_every) {
        for (auto &kv : c.procs)
            if (!c.dead.count(kv.first)) repair_round(*kv.second->chunks, states[kv.first], c.route, scan_every);
    };
    repair_all(0);          // first round: records every arc as checked

    std::string victim = std::next(c.procs.begin(), 4)->first;
    if (addr(start) == victim) victim = std::next(c.procs.begin(), 5)->first;
    c.dead.insert(victim);
    const int lost = under_replicated(c, keys);
    check(lost > 0, "killing a process leaves chunks one copy short (" + std::to_string(lost) + ")");
    int rounds = 0;
    while (under_replicated(c, keys) > 0 && rounds < 10) {
        c.round(rng);       // the ring notices
        repair_all(0);      // the owners re-replicate
        ++rounds;
    }
    check(under_replicated(c, keys) == 0, "every chunk is back on its three correct processes after " +
                                          std::to_string(rounds) + " rounds, with no full scan");

    // A file lost on a process that stays up changes nothing in the ring, so only
    // the full scan finds it: the scrub, then the owner's check.
    const std::string key = keys[7];
    Id k;
    id_from_key(key, k);
    const std::vector<std::string> holders = expected_holders(c.all_members(true), k, RF);
    Process &replica = c.at(holders[1]);
    {
        std::fstream f(store_path(*replica.store, key), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(10);
        f.put('!');
    }
    repair_all(0);
    check(store_has(*replica.store, key), "a corrupt copy is not noticed by a round with no change");
    repair_all(1);          // a full scan on every process
    repair_all(0);
    std::string out;
    check(store_get(*replica.store, key, out) == GetResult::Found && chunk_key(out) == key,
          "the full scan removes the corrupt copy and the owner restores a good one");

    // A chunk left on a process that is not in its replica set is handed to the owner.
    const std::string stray = random_bytes(rng, 3000);
    const std::string skey = chunk_key(stray);
    Id sk;
    id_from_key(skey, sk);
    const std::vector<std::string> shold = expected_holders(c.all_members(true), sk, RF);
    std::string outsider;
    for (auto &kv : c.procs)
        if (!c.dead.count(kv.first) && std::find(shold.begin(), shold.end(), kv.first) == shold.end()) outsider = kv.first;
    store_put(*c.at(outsider).store, skey, stray);
    repair_all(1);
    repair_all(0);
    check(under_replicated(c, {skey}) == 0, "a chunk held only by a non-owner reaches all three of its processes");
}

}  // namespace

int main() {
    test_keys();
    test_store_basics();
    test_store_restart();
    test_store_concurrent();
    test_store_arcs();
    test_body_length();
    test_split_target();
    test_short_reads();
    test_vnode_ring();
    test_vnode_siblings();
    test_vnode_frozen_member_vs_dead_process();
    test_replica_sets();
    test_writes();
    test_reads();
    test_body_cap();
    test_repair();
    remove_tmpdirs();

    if (failures == 0) {
        std::printf("PASS  %d/%d checks passed\n", checks, checks);
        return 0;
    }
    std::printf("FAIL  %d of %d checks failed\n", failures, checks);
    return 1;
}
