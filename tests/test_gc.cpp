// In-process tests for deletion's data side (decisions D-090 to D-092): a node's
// store dropping chunks only when they are old enough, the KEYS and DROP requests,
// the ring walk that finds every node, and whole collections.
//
// Each node here is a real ChunkStore in a temporary directory behind the node's own
// request handler (handle_chunk_request); the ring a manifest is read from is a map
// in memory; the namespace is a list of (name, key) pairs; and the collector's clock
// is a number the test sets. So the cases that matter can be built on purpose: a
// chunk shared by two files, an old orphan reused by a new upload, a name whose
// manifest is missing, a collection that runs past its deadline.

#include "chunk.h"
#include "collect.h"
#include "names.h"
#include "store.h"
#include "transfer.h"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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

std::vector<std::string> g_tmpdirs;

std::string make_tmpdir() {
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/test-gc-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { std::perror("mkdtemp"); std::exit(1); }
    g_tmpdirs.emplace_back(buf.data());
    return g_tmpdirs.back();
}

int remove_entry(const char *path, const struct stat *, int, struct FTW *) { return std::remove(path); }

void remove_tmpdirs() {
    for (const std::string &d : g_tmpdirs) nftw(d.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

bool on_disk(const ChunkStore &s, const std::string &key) {
    struct stat st{};
    return stat(store_path(s, key).c_str(), &st) == 0;
}

std::string bytes_of(std::mt19937_64 &rng, std::size_t n) {
    std::string s(n, '\0');
    for (char &c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}

using std::chrono::hours;
using std::chrono::milliseconds;

// Make a chunk look as if it was last written `ago` before now.
void age(ChunkStore &s, const std::string &key, StoreClock::duration ago) {
    std::lock_guard<std::mutex> lock(s.mu);
    s.touched[key] = StoreClock::now() - ago;
}

// ---------------------------------------------------------------------------
// The store: drop only what is old enough
// ---------------------------------------------------------------------------

void test_store_drop() {
    std::printf("store: a chunk is dropped only after its grace, and any write renews it\n");
    std::mt19937_64 rng(11);
    ChunkStore s;
    std::string report, err;
    store_open(s, make_tmpdir(), report, err);
    const std::string a = bytes_of(rng, 5000), b = bytes_of(rng, 3000);
    const std::string ka = chunk_key(a), kb = chunk_key(b);
    store_put(s, ka, a);
    store_put(s, kb, b);
    std::uint64_t freed = 0;

    check(store_drop(s, ka, hours(1), freed) == DropResult::TooYoung && on_disk(s, ka) && store_has(s, ka),
          "a chunk written just now is too young for a 1 h grace, and stays");
    age(s, ka, hours(2));
    const std::uint64_t before = s.bytes.load();
    check(store_drop(s, ka, hours(1), freed) == DropResult::Dropped && freed == a.size(),
          "written 2 h ago: dropped, and the bytes freed are its size");
    check(!on_disk(s, ka) && !store_has(s, ka) && s.bytes.load() == before - a.size(),
          "gone from the disk, from the index, and from the bytes held");
    std::string out;
    check(store_get(s, ka, out) == GetResult::Missing, "a read finds it missing");
    check(store_drop(s, ka, hours(1), freed) == DropResult::NotHeld, "dropping it again: not held");
    check(store_drop(s, "../../etc/passwd", hours(1), freed) == DropResult::BadKey, "a bad key is refused");

    // The deduplication race, in one store: an old orphan written again by a new
    // upload is renewed, so it is no longer old.
    age(s, kb, hours(3));
    check(store_put(s, kb, b) == PutResult::AlreadyHeld, "an upload writes an old orphan again: already held");
    check(store_drop(s, kb, hours(1), freed) == DropResult::TooYoung && on_disk(s, kb),
          "and the write renewed it: too young to drop (git's freshen)");
    check(store_put(s, ka, a) == PutResult::Stored && store_drop(s, ka, hours(1), freed) == DropResult::TooYoung,
          "a chunk stored again after a drop is new, and young");
}

void test_store_restart_is_a_write() {
    std::printf("store: a restart counts as writing every chunk\n");
    std::mt19937_64 rng(12);
    const std::string dir = make_tmpdir();
    const std::string c = bytes_of(rng, 2000), k = chunk_key(c);
    {
        ChunkStore s;
        std::string report, err;
        store_open(s, dir, report, err);
        store_put(s, k, c);
        age(s, k, hours(5));                 // old before the "crash"
    }
    ChunkStore again;
    std::string report, err;
    store_open(again, dir, report, err);
    std::uint64_t freed = 0;
    check(again.touched.empty(), "write times are not on disk: after a restart none are known");
    check(store_drop(again, k, hours(1), freed) == DropResult::TooYoung,
          "so a chunk 5 h old before the restart counts as written at the restart: too young");
    again.opened = StoreClock::now() - hours(2);
    check(store_drop(again, k, hours(1), freed) == DropResult::Dropped, "and is dropped once the grace has passed since");
}

void test_store_forget_old() {
    std::printf("store: write times older than the grace are forgotten\n");
    std::mt19937_64 rng(13);
    ChunkStore s;
    std::string report, err;
    store_open(s, make_tmpdir(), report, err);
    std::vector<std::string> keys;
    for (int i = 0; i < 10; ++i) {
        const std::string c = bytes_of(rng, 100 + i);
        store_put(s, chunk_key(c), c);
        keys.push_back(chunk_key(c));
    }
    for (int i = 0; i < 6; ++i) age(s, keys[i], hours(2));
    store_forget_old(s, hours(1));
    check(s.touched.size() == 4, "the six older than the grace are forgotten, the four young kept (" +
                                     std::to_string(s.touched.size()) + ")");
    s.opened = StoreClock::now() - hours(3);
    std::uint64_t freed = 0;
    check(store_drop(s, keys[0], hours(1), freed) == DropResult::Dropped &&
              store_drop(s, keys[9], hours(1), freed) == DropResult::TooYoung,
          "a forgotten chunk is as old as the store's start; a remembered one keeps its time");
}

void test_store_drop_races() {
    std::printf("store: writes and drops of one chunk racing, 2,000 times\n");
    std::mt19937_64 rng(14);
    ChunkStore s;
    std::string report, err;
    store_open(s, make_tmpdir(), report, err);
    const std::string c = bytes_of(rng, 4096), k = chunk_key(c);
    bool consistent = true;
    for (int round = 0; round < 2000; ++round) {
        // A grace of zero: every held chunk is old enough, so a drop always races a write.
        std::thread writer([&] { store_put(s, k, c); });
        std::thread dropper([&] { std::uint64_t f; store_drop(s, k, milliseconds(0), f); });
        writer.join();
        dropper.join();
        // Whatever order they ran in, the index and the disk must agree: a write the
        // index records is a file that exists. (An unlink outside the lock could delete
        // a file a racing write had just renamed into place.)
        const bool held = store_has(s, k), there = on_disk(s, k);
        if (held != there || s.bytes.load() != (held ? c.size() : 0)) consistent = false;
    }
    check(consistent, "after every round the index, the disk and the bytes held agree");
}

// ---------------------------------------------------------------------------
// KEYS and DROP, through the node's own request handler
// ---------------------------------------------------------------------------

const std::string GC_KEY = std::string(32, '7');

void test_requests() {
    std::printf("node: KEYS lists what is held; DROP needs the key and respects the grace\n");
    std::mt19937_64 rng(15);
    ChunkStore store;
    std::string report, err;
    store_open(store, make_tmpdir(), report, err);
    ChunkNode n;
    n.store = &store;
    n.gc_grace = hours(1);
    std::set<std::string> keys;
    for (int i = 0; i < 45; ++i) {
        const std::string c = bytes_of(rng, 50 + i);
        store_put(store, chunk_key(c), c);
        keys.insert(chunk_key(c));
    }
    auto ask = [&n](const std::string &line) { return handle_chunk_request(n, Frame{line, ""}).reply; };

    std::vector<std::string> listed, page;
    long grace = -1;
    std::string cursor = "-";
    int pages = 0;
    while (parse_keys_reply(ask("KEYS " + cursor), page, grace) && !page.empty() && pages < 10) {
        listed.insert(listed.end(), page.begin(), page.end());
        cursor = page.back();
        ++pages;
    }
    check(pages == 3 && listed.size() == 45 && std::set<std::string>(listed.begin(), listed.end()) == keys && grace == 3600,
          "45 chunks list in 3 pages of at most 20, in order, each with the grace in seconds");
    check(ask("KEYS").compare(0, 4, "ERR ") == 0 && ask("KEYS nothex").compare(0, 4, "ERR ") == 0,
          "a KEYS without a cursor, or with a bad one, is refused");

    const std::vector<std::string> v(keys.begin(), keys.end());
    check(ask("DROP " + GC_KEY + " " + v[0]) == "ERR deletion is not enabled on this node",
          "a node without a collection key refuses every DROP");
    n.gc_key = GC_KEY;
    check(ask("DROP " + std::string(32, '8') + " " + v[0]) == "ERR wrong collection key", "the wrong key is refused");
    check(ask("DROP " + GC_KEY.substr(1) + " " + v[0]) == "ERR wrong collection key", "a key one character short too");
    check(store_has(store, v[0]), "and nothing was dropped by either");

    age(store, v[0], hours(2));
    age(store, v[1], hours(2));
    const std::string gone = chunk_key("never stored");
    std::string bits;
    std::uint64_t freed = 0;
    const std::string reply = ask("DROP " + GC_KEY + " " + v[0] + " " + v[1] + " " + v[2] + " " + gone);
    check(parse_dropped_reply(reply, 4, bits, freed) && bits == "1100",
          "the right key: two old chunks dropped, a young one and an absent one kept (" + reply + ")");
    check(!store_has(store, v[0]) && !store_has(store, v[1]) && store_has(store, v[2]), "exactly those two are gone");
    check(n.dropped_chunks.value() == 2 && n.kept_young.value() == 1 && n.dropped_bytes.value() == freed && freed > 0,
          "and counted: dropped, freed bytes, kept as too young");

    std::string many = "DROP " + GC_KEY;
    for (int i = 0; i < MAX_HAVE_KEYS + 1; ++i) many += " " + v[3 + i];
    check(ask(many).compare(0, 4, "ERR ") == 0 && store_has(store, v[3]), "more than 20 keys in one DROP is refused whole");
    check(ask("DROP " + GC_KEY + " " + v[3] + " bad").compare(0, 4, "ERR ") == 0 && store_has(store, v[3]),
          "one bad key refuses the whole DROP before anything is dropped");
    check(ask("DROP " + GC_KEY).compare(0, 4, "ERR ") == 0, "a DROP with no keys is refused");

    check(same_secret("abc", "abc") && !same_secret("abc", "abd") && !same_secret("abc", "xbc") && !same_secret("abc", "ab") &&
              !same_secret("", "a") && same_secret("", ""),
          "same_secret: equal, last or first character different, different lengths");
}

// ---------------------------------------------------------------------------
// Finding every node: the ring walk
// ---------------------------------------------------------------------------

void test_walk_ring() {
    std::printf("collector: walking the ring finds every process\n");
    // 12 processes x 4 members each, wired as a correct ring; the transport hands
    // each request to the member it is addressed to, as a node would.
    std::vector<Peer> processes, members;
    for (int p = 0; p < 12; ++p) {
        const Peer proc{0, "127.0.0.1", 7000 + p};
        processes.push_back(proc);
        for (int i = 0; i < 4; ++i) members.push_back(Peer{id_of_member(proc.ip, proc.port, i), proc.ip, proc.port});
    }
    std::string err;
    validate_ring(members, err);                       // sorts by identifier
    std::map<Id, std::unique_ptr<NodeState>> states;
    for (const Peer &m : members) {
        auto s = std::make_unique<NodeState>(m, 8);
        init_wired(*s, members);
        states[m.id] = std::move(s);
    }
    int asked = 0;
    Transport route = [&](const Peer &to, const std::string &request, std::string &reply) {
        ++asked;
        const auto it = states.find(to.id);
        if (it == states.end()) return Delivery::Refused;
        reply = handle_request(*it->second, request, {}).reply;
        return Delivery::Ok;
    };
    const Peer start{id_of_member("127.0.0.1", 7005, 0), "127.0.0.1", 7005};
    std::vector<Peer> found;
    check(walk_ring(start, route, found, err) && found.size() == 12,
          "all 12 processes found from one of them (" + std::to_string(found.size()) + ", " + err + ")");
    check(asked <= 48 / 8 + 2, "jumping a successor list at a time: " + std::to_string(asked) + " requests for 48 members");

    // A member that cannot be asked: the walk says so, and what it found is still returned.
    std::size_t at = 0;
    while (members[at].id != start.id) ++at;
    states.erase(members[(at + 8) % members.size()].id);          // the farthest of start's 8 successors: the first jump
    found.clear();
    const bool whole = walk_ring(start, route, found, err);
    check(!whole && !found.empty() && !err.empty(), "a member that cannot be asked ends the walk with a reason, keeping what it found");
}

// ---------------------------------------------------------------------------
// Whole collections
// ---------------------------------------------------------------------------

// A small system: a ring that is a map (where manifests are read), three nodes with
// real stores holding copies, a namespace, and a clock.
struct World {
    std::mutex mu;
    std::map<std::string, std::string> ring;                     // key -> bytes: every chunk, for reads
    std::vector<std::unique_ptr<ChunkStore>> stores;
    std::vector<std::unique_ptr<ChunkNode>> nodes;
    std::vector<Peer> addresses;
    std::vector<std::pair<std::string, std::string>> names;      // the namespace: (name, key), sorted
    double clock = 1000;
    int entries_calls = 0;

    explicit World(int n) {
        for (int i = 0; i < n; ++i) {
            stores.push_back(std::make_unique<ChunkStore>());
            std::string report, err;
            store_open(*stores.back(), make_tmpdir(), report, err);
            nodes.push_back(std::make_unique<ChunkNode>());
            nodes.back()->store = stores.back().get();
            nodes.back()->gc_key = GC_KEY;
            nodes.back()->gc_grace = std::chrono::seconds(5000);
            addresses.push_back(Peer{0, "127.0.0.1", 9000 + i});
        }
    }

    // Upload a file: every chunk and manifest into the ring and onto every node.
    std::string upload(const std::string &bytes) {
        PutFn put = [this](const std::string &body) {
            PutOutcome p;
            p.key = chunk_key(body);
            std::lock_guard<std::mutex> lock(mu);
            ring[p.key] = body;
            for (auto &s : stores) store_put(*s, p.key, body);
            p.ok = true;
            p.copies = 3;
            return p;
        };
        const std::string path = make_tmpdir() + "/f";
        std::ofstream(path, std::ios::binary) << bytes;
        TransferOptions o;
        o.chunk = 1000;
        o.fanout = 4;                                            // small manifests: a tree, not one manifest
        o.workers = 2;
        const UploadOutcome u = upload_file(path, put, o);
        return u.ok ? u.root_key : "";
    }

    void name(const std::string &n, const std::string &key) {
        names.emplace_back(n, key);
        std::sort(names.begin(), names.end());
    }
    void unname(const std::string &n) {
        names.erase(std::remove_if(names.begin(), names.end(), [&](const auto &e) { return e.first == n; }), names.end());
    }
    // Every chunk on every node, last written `ago` -- and every store opened before
    // that, as it would have been: a store cannot hold a chunk written before it opened.
    void age_all(StoreClock::duration ago) {
        for (auto &s : stores) {
            std::lock_guard<std::mutex> lock(s->mu);
            s->opened = StoreClock::now() - ago - hours(1);
            for (const std::string &k : s->keys) s->touched[k] = StoreClock::now() - ago;
        }
    }

    CollectIO io() {
        CollectIO io;
        io.entries = [this](const std::string &after, std::vector<std::pair<std::string, std::string>> &page, std::string &) {
            ++entries_calls;
            page.clear();
            for (const auto &e : names)                         // pages of 2, to exercise the paging
                if ((after.empty() || e.first > after) && page.size() < 2) page.push_back(e);
            return true;
        };
        io.get = [this](const std::string &key) {
            GetOutcome g;
            std::lock_guard<std::mutex> lock(mu);
            const auto it = ring.find(key);
            if (it == ring.end()) { g.message = "MISSING everywhere"; return g; }
            g.ok = true;
            g.body = it->second;
            return g;
        };
        io.nodes = [this](std::vector<Peer> &out, std::string &) { out = addresses; return true; };
        io.ask = [this](const Peer &to, const std::string &line, std::string &reply) {
            clock += 1;                                          // every exchange takes "a second"
            reply = handle_chunk_request(*nodes[static_cast<std::size_t>(to.port - 9000)], Frame{line, ""}).reply;
            return Delivery::Ok;
        };
        io.now = [this] { return clock; };
        return io;
    }

    bool everywhere(const std::string &key) {
        for (auto &s : stores)
            if (!store_has(*s, key)) return false;
        return true;
    }
    bool nowhere(const std::string &key) {
        for (auto &s : stores)
            if (store_has(*s, key)) return false;
        return true;
    }
};

CollectSettings settings() {
    CollectSettings s;
    s.gc_key = GC_KEY;
    s.deadline_s = 1000;
    s.upload_limit_s = 3000;
    s.tracker_wait_s = 10;                                       // 3000 + 10 + 1000 < the nodes' 5000
    return s;
}

void test_collect_shared_chunk() {
    std::printf("collector: deleting one of two files that share chunks frees only what the other does not use\n");
    std::mt19937_64 rng(21);
    World w(3);
    const std::string common = bytes_of(rng, 6000);             // six 1000-byte chunks in both files
    const std::string a = common + bytes_of(rng, 4000), b = common + bytes_of(rng, 3500);
    const std::string ra = w.upload(a), rb = w.upload(b);
    w.name("a", ra);
    w.name("b", rb);

    Resolved ta = resolve_manifest(ra, w.io().get), tb = resolve_manifest(rb, w.io().get);
    std::set<std::string> in_a, in_b;
    for (const ChunkRef &c : ta.chunks) in_a.insert(c.key);
    for (const ChunkRef &c : tb.chunks) in_b.insert(c.key);
    std::set<std::string> shared, only_a;
    for (const std::string &k : in_a) (in_b.count(k) ? shared : only_a).insert(k);
    check(shared.size() == 6 && only_a.size() == 4 && ta.manifest_keys.size() > 1,
          "the files share 6 chunks; 4 are a's alone; a's manifests form a tree (" + std::to_string(ta.manifest_keys.size()) + ")");

    w.unname("a");                                                // deleting the name frees nothing yet
    w.age_all(hours(3));
    const CollectReport r = collect(w.io(), settings());
    check(r.ok && r.names == 1 && r.roots == 1, "a collection with only b named: ok, one name, one root");

    // Manifests deduplicate too: a's first data manifest lists the same four chunks with
    // the same sizes as b's, so it is the same text -- one chunk, used by both files.
    std::set<std::string> b_manifests(tb.manifest_keys.begin(), tb.manifest_keys.end()), only_a_manifests;
    for (const std::string &k : ta.manifest_keys)
        if (!b_manifests.count(k)) only_a_manifests.insert(k);
    check(only_a_manifests.size() == ta.manifest_keys.size() - 1,
          "and one manifest is shared too: a's first data manifest is byte for byte b's");

    bool shared_kept = true, only_a_gone = true, b_manifests_kept = true, a_manifests_gone = true;
    for (const std::string &k : shared) shared_kept = shared_kept && w.everywhere(k);
    for (const std::string &k : only_a) only_a_gone = only_a_gone && w.nowhere(k);
    for (const std::string &k : tb.manifest_keys) b_manifests_kept = b_manifests_kept && w.everywhere(k);
    for (const std::string &k : only_a_manifests) a_manifests_gone = a_manifests_gone && w.nowhere(k);
    check(shared_kept, "every chunk shared with b survives on every node");
    check(only_a_gone && a_manifests_gone, "every chunk and manifest only a used is gone from every node");
    check(b_manifests_kept, "b's manifests survive, the shared one included");
    check(r.dropped == 3 * (only_a.size() + only_a_manifests.size()) && r.garbage == r.dropped,
          "dropped = garbage = 3 copies of each of a's own chunks and manifests (" + std::to_string(r.dropped) + ")");
    check(resolve_manifest(rb, w.io().get).ok, "and b still reads whole");
}

void test_collect_refusals() {
    std::printf("collector: what makes it drop nothing, or less\n");
    std::mt19937_64 rng(22);
    {
        World w(3);
        const std::string r1 = w.upload(bytes_of(rng, 5000));
        w.name("good", r1);
        w.name("broken", chunk_key("a manifest nobody stored"));
        const std::string orphan = w.upload(bytes_of(rng, 3000));   // garbage, unnamed
        w.age_all(hours(3));
        const CollectReport r = collect(w.io(), settings());
        check(!r.ok && r.dropped == 0 && r.message.find("broken") != std::string::npos,
              "a name whose manifest cannot be read stops the collection, naming it: " + r.message.substr(0, 80));
        check(w.everywhere(orphan), "and nothing at all was dropped, garbage included");
    }
    {
        World w(3);
        const std::string orphan = w.upload(bytes_of(rng, 3000));
        w.age_all(hours(3));
        CollectSettings s = settings();
        s.dry_run = true;
        const CollectReport r = collect(w.io(), s);
        check(r.ok && r.garbage > 0 && r.dropped == 0 && w.everywhere(orphan), "a dry run counts the garbage and drops nothing");
    }
    {
        World w(3);
        const std::string orphan = w.upload(bytes_of(rng, 3000));
        // Young: written "now". The nodes keep it.
        const CollectReport r = collect(w.io(), settings());
        check(r.ok && r.garbage > 0 && r.dropped == 0 && w.everywhere(orphan),
              "garbage written within the grace is listed as garbage and kept by every node");
    }
    {
        World w(3);
        const std::string orphan = w.upload(bytes_of(rng, 3000));
        w.age_all(hours(3));
        w.nodes[1]->gc_grace = std::chrono::seconds(60);          // shorter than U + W + D
        const CollectReport r = collect(w.io(), settings());
        check(r.ok && !r.nodes[1].skipped.empty() && r.nodes[1].dropped == 0 && store_has(*w.stores[1], orphan) &&
                  r.nodes[0].dropped > 0,
              "a node whose grace is shorter than the upload limit + tracker wait + deadline is not swept; the others are");
    }
    {
        World w(3);
        std::vector<std::string> roots;
        for (int i = 0; i < 6; ++i) roots.push_back(w.upload(bytes_of(rng, 12000)));   // ~78 chunks of garbage per node
        w.age_all(hours(3));
        CollectSettings s = settings();
        s.deadline_s = 30;                                         // each exchange advances the clock 1 s
        const CollectReport r = collect(w.io(), s);
        check(r.ok && r.deadline_hit && r.dropped > 0 && r.dropped < r.garbage,
              "past the deadline no more DROPs are sent: " + std::to_string(r.dropped) + " of " + std::to_string(r.garbage) +
                  " dropped, the rest left");
    }
    {
        World w(3);
        const std::string orphan = w.upload(bytes_of(rng, 3000));
        w.age_all(hours(3));
        CollectSettings s = settings();
        s.gc_key = std::string(32, '9');
        const CollectReport r = collect(w.io(), s);
        bool all_refused = true;
        for (const NodeSweep &n : r.nodes) all_refused = all_refused && n.dropped == 0 && !n.skipped.empty();
        check(all_refused && w.everywhere(orphan), "with the wrong key every node refuses, and says so");
    }
}

// The race the collector must survive (D-091), at the scale of whole uploads: an
// upload that has stored its chunks but not yet registered, and one that reuses old
// orphans, while a collection runs.
void test_collect_upload_race() {
    std::printf("collector: an upload in flight, and one reusing old orphans, survive a collection\n");
    std::mt19937_64 rng(23);
    World w(3);
    const std::string content = bytes_of(rng, 7000);

    // An earlier upload of the same content, named and then deleted long ago: old orphans.
    const std::string old_root = w.upload(content);
    w.age_all(hours(3));

    // The same content uploaded again now: every chunk is written again -- already
    // held, renewed -- but the name is not registered yet.
    const std::string new_root = w.upload(content);
    check(new_root == old_root, "the same content: the same chunks and the same root");
    const std::string fresh = w.upload(bytes_of(rng, 4000));     // a different upload, also not yet named

    const CollectReport r = collect(w.io(), settings());
    Resolved t = resolve_manifest(new_root, w.io().get);
    bool kept = t.ok;
    for (const ChunkRef &c : t.chunks) kept = kept && w.everywhere(c.key);
    for (const std::string &k : t.manifest_keys) kept = kept && w.everywhere(k);
    check(r.ok && r.garbage > 0 && r.dropped == 0 && kept && w.everywhere(fresh),
          "nothing is dropped: the reused orphans were renewed by the new upload, and the new chunks are young");
    w.name("again", new_root);                                    // the registration lands after the collection
    check(resolve_manifest(new_root, w.io().get).ok, "the late registration names a whole file");

    // Control: the same chunks with no renewing write after they aged -- which is what
    // the re-upload above would leave if a duplicate write renewed nothing -- are dropped.
    World v(3);
    const std::string root2 = v.upload(content);
    v.age_all(hours(3));                                          // ... and no renewing write since
    const CollectReport r2 = collect(v.io(), settings());
    Resolved t2 = resolve_manifest(root2, v.io().get);
    bool lost = false;
    for (const ChunkRef &c : t2.chunks) lost = lost || v.nowhere(c.key);
    check(r2.ok && r2.dropped > 0 && lost,
          "control: without the renewal, an unnamed upload of old chunks loses them (" + std::to_string(r2.dropped) + " dropped)");
}

void test_parsers() {
    std::printf("collector: the replies it reads are parsed strictly\n");
    std::vector<std::pair<std::string, std::string>> page;
    const std::string k = chunk_key("x");
    check(parse_entries_reply("ENTRIES 2 a " + k + " b " + k, page) && page.size() == 2 && page[1].first == "b",
          "ENTRIES with two pairs");
    check(parse_entries_reply("ENTRIES 0", page) && page.empty(), "ENTRIES 0: the end");
    for (const std::string &bad : std::vector<std::string>{"ENTRIES", "ENTRIES 1 a", "ENTRIES 2 a " + k, "ENTRIES 01 a " + k,
                                                           "ENTRIES 1 a nothex", "NAMES 1 a", "ENTRIES 1  a " + k})
        check(!parse_entries_reply(bad, page), "refused: " + bad.substr(0, 40));
    std::vector<std::string> keys;
    long g = 0;
    const std::string k2 = chunk_key("y"), lo = std::min(k, k2), hi = std::max(k, k2);
    check(parse_keys_reply("KEYS 2 7200 " + lo + " " + hi, keys, g) && keys.size() == 2 && g == 7200, "KEYS with two keys");
    check(!parse_keys_reply("KEYS 2 7200 " + hi + " " + lo, keys, g), "KEYS out of order is refused");
    check(!parse_keys_reply("KEYS 2 7200 " + lo + " " + lo, keys, g), "KEYS with a repeat is refused");
    check(!parse_keys_reply("KEYS 1 -5 " + lo, keys, g) && !parse_keys_reply("KEYS 3 1 " + lo, keys, g),
          "a negative grace, or a count that does not match, is refused");
    std::string bits;
    std::uint64_t b = 0;
    check(parse_dropped_reply("DROPPED 101 4096", 3, bits, b) && bits == "101" && b == 4096, "DROPPED with three bits");
    check(!parse_dropped_reply("DROPPED 10 4096", 3, bits, b) && !parse_dropped_reply("DROPPED 1x1 0", 3, bits, b) &&
              !parse_dropped_reply("DROPPED 101", 3, bits, b),
          "the wrong number of bits, a bad bit, or no byte count is refused");
}

}  // namespace

int main() {
    std::printf("test-gc: dropping chunks, KEYS and DROP, the ring walk, whole collections\n");
    test_store_drop();
    test_store_restart_is_a_write();
    test_store_forget_old();
    test_store_drop_races();
    test_requests();
    test_walk_ring();
    test_collect_shared_chunk();
    test_collect_refusals();
    test_collect_upload_race();
    test_parsers();
    remove_tmpdirs();
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
