// In-process tests for Phase 5's file transfer (transfer.cpp): the manifest
// format and its tree, the worker pool, and whole uploads and downloads.
//
// The ring here is a map in memory behind the same PutFn and GetFn the client
// passes in, so a test can make a chunk vanish, fail twice and then succeed, or
// come back the wrong length -- and a manifest can be constructed to lie. Files
// are real files in a temporary directory, because what a failed download leaves
// on disk is the thing being tested (D-055).

#include "store.h"
#include "transfer.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
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

// ---- temporary directories, removed at exit ---------------------------------

std::vector<std::string> g_tmpdirs;

std::string make_tmpdir() {
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/test-transfer-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    g_tmpdirs.emplace_back(buf.data());
    return g_tmpdirs.back();
}

int remove_entry(const char *path, const struct stat *, int, struct FTW *) { return std::remove(path); }

void remove_tmpdirs() {
    for (const std::string &d : g_tmpdirs) nftw(d.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string random_bytes(std::size_t n, std::mt19937_64 &rng) {
    std::string s(n, '\0');
    for (char &c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}

void write_file(const std::string &path, const std::string &bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool exists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

// Every entry of a directory -- to prove that no part file was left behind.
std::set<std::string> listing(const std::string &dir) {
    std::set<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") names.insert(n);
        }
        closedir(d);
    }
    return names;
}

// ---- a ring that is a map ---------------------------------------------------

// Stores by key and answers with the same outcome types put_chunk and get_chunk
// return. `fail_get` makes a key fail a given number of times (-1: for ever).
struct MemoryRing {
    std::mutex mu;
    std::map<std::string, std::string> chunks;
    std::map<std::string, int> fail_get;
    std::atomic<int> puts{0}, gets{0};

    PutFn put() {
        return [this](const std::string &body) {
            ++puts;
            PutOutcome p;
            p.key = chunk_key(body);
            std::lock_guard<std::mutex> lock(mu);
            chunks[p.key] = body;
            p.ok = true;
            p.copies = 3;
            p.attempts = 1;
            return p;
        };
    }

    GetFn get() {
        return [this](const std::string &key) {
            ++gets;
            GetOutcome g;
            std::lock_guard<std::mutex> lock(mu);
            auto f = fail_get.find(key);
            if (f != fail_get.end() && f->second != 0) {
                if (f->second > 0) --f->second;
                g.message = "injected failure";
                g.tried = {"127.0.0.1:1: injected failure"};
                return g;
            }
            auto it = chunks.find(key);
            if (it == chunks.end()) { g.message = "missing"; g.tried = {"127.0.0.1:1: MISSING"}; return g; }
            g.ok = true;
            g.body = it->second;
            g.tried = {"127.0.0.1:1: ok"};
            return g;
        };
    }

    // Put a constructed manifest straight in, as a hostile uploader could.
    std::string add(const std::string &blob) {
        std::lock_guard<std::mutex> lock(mu);
        const std::string k = chunk_key(blob);
        chunks[k] = blob;
        return k;
    }
};

std::string fake_key(std::size_t i) {
    std::string k = chunk_key("chunk " + std::to_string(i));
    return k;
}

TransferOptions fast(int workers = 4, std::uint64_t chunk = 4096) {
    TransferOptions o;
    o.workers = workers;
    o.chunk = chunk;
    o.backoff_ms = 1;              // retries in microseconds, not half-seconds
    return o;
}

// ---- manifests --------------------------------------------------------------

const std::string K1 = std::string(40, 'a');
const std::string K2 = std::string(40, 'b');

std::string header(const std::string &size, const std::string &chunk, const std::string &kind, const std::string &entries) {
    return "chordfs-manifest 1\nsize " + size + "\nchunk " + chunk + "\nkind " + kind + "\nentries " + entries + "\n";
}

bool parses(const std::string &text) {
    Manifest m;
    std::string why;
    return parse_manifest(text, m, why);
}

void test_manifest_format() {
    Manifest m{5000, 4096, ManifestKind::Data, {{K1, 4096}, {K2, 904}}};
    const std::string text = format_manifest(m);
    check(text == header("5000", "4096", "data", "2") + K1 + " 4096\n" + K2 + " 904\n", "the text format is exactly as documented");
    Manifest back;
    std::string why;
    check(parse_manifest(text, back, why) && back.size == 5000 && back.chunk == 4096 && back.kind == ManifestKind::Data &&
              back.entries.size() == 2 && back.entries[1].key == K2 && back.entries[1].bytes == 904,
          "a manifest survives format then parse");

    // Accepted boundaries.
    check(parses(header("0", "4096", "data", "0")), "a zero-byte file is a data manifest with no entries");
    check(parses(header("1", "4096", "data", "1") + K1 + " 1\n"), "a one-byte file");
    check(parses(header("4096", "4096", "data", "1") + K1 + " 4096\n"), "exactly one chunk");
    check(parses(header("4097", "4096", "data", "2") + K1 + " 4096\n" + K2 + " 1\n"), "one chunk and one byte");
    check(parses(header("12288", "4096", "index", "2") + K1 + " 8192\n" + K2 + " 4096\n"), "an index of two manifests");
    check(parses(header("9000", "4096", "index", "2") + K1 + " 8192\n" + K2 + " 808\n"), "an index whose last child is short");

    // Refused: every header and entry rule, one at a time.
    const std::string ok_tail = K1 + " 1\n";
    check(!parses(""), "an empty manifest");
    check(!parses(header("1", "4096", "data", "1") + K1 + " 1"), "no newline at the end");
    check(!parses("chordfs-manifest 2\nsize 1\nchunk 4096\nkind data\nentries 1\n" + ok_tail), "a different version");
    check(!parses(header("01", "4096", "data", "1") + ok_tail), "a size with a leading zero");
    check(!parses(header("+1", "4096", "data", "1") + ok_tail), "a size with a sign");
    check(!parses(header("1 ", "4096", "data", "1") + ok_tail), "a size with a trailing space");
    check(!parses("chordfs-manifest 1\nsize 1\r\nchunk 4096\nkind data\nentries 1\n" + ok_tail), "a carriage return");
    check(!parses(header("18446744073709551616", "4096", "index", "1") + K1 + " 1\n"), "a size of 2^64 overflows");
    check(!parses(header("1", "0", "data", "1") + ok_tail), "a chunk size of zero");
    check(!parses(header("1", std::to_string(MAX_CHUNK + 1), "data", "1") + ok_tail), "a chunk size above the limit");
    check(!parses(header("1", "4096", "blob", "1") + ok_tail), "an unknown kind");
    check(!parses(header("1", "4096", "data", "2") + ok_tail), "an entries count larger than the entries");
    check(!parses(header("1", "4096", "data", "0") + ok_tail), "an entries count smaller than the entries");
    check(!parses(header("1", "4096", "data", "1") + std::string(40, 'A') + " 1\n"), "an uppercase key");
    check(!parses(header("1", "4096", "data", "1") + std::string(39, 'a') + " 1\n"), "a 39-character key");
    check(!parses(header("1", "4096", "data", "1") + K1 + "  1\n"), "two spaces in an entry");
    check(!parses(header("1", "4096", "data", "1") + K1 + " 01\n"), "an entry with a leading zero");
    check(!parses(header("2", "4096", "data", "1") + ok_tail), "entries that do not add up to the size");
    check(!parses(header("4097", "4096", "data", "2") + K1 + " 4095\n" + K2 + " 2\n"), "a data chunk short in the middle");
    check(!parses(header("0", "4096", "data", "1") + K1 + " 0\n"), "a zero-byte data chunk");
    check(!parses(header("4097", "4096", "data", "1") + K1 + " 4097\n"), "a data chunk larger than the chunk size");
    check(!parses(header("0", "4096", "index", "0")), "an index with no children");
    check(!parses(header("4096", "4096", "index", "2") + K1 + " 0\n" + K2 + " 4096\n"), "an empty child");
    check(!parses(header("8193", "4096", "index", "2") + K1 + " 4097\n" + K2 + " 4096\n"), "a child not a whole number of chunks");
    check(!parses(header("18446744073709551615", "1", "index", "2") + K1 + " 18446744073709551615\n" + K2 + " 1\n"),
          "byte counts that wrap past 2^64");

    // The 8,192-entry cap, checked before the lines are split.
    std::string many = header("8193", "1", "data", "8193");
    for (int i = 0; i < 8193; ++i) many += K1 + " 1\n";
    Manifest m2;
    std::string why2;
    check(!parse_manifest(many, m2, why2) && why2.find("more than 8192") != std::string::npos, "8,193 entries are refused: " + why2);

    // The worst case still fits in one chunk: 8,192 entries with 20-digit counts and
    // a 20-digit size (formatted only -- such counts could never add up).
    Manifest worst{18446744073709551615ULL, MAX_CHUNK, ManifestKind::Index, {}};
    for (std::size_t i = 0; i < MANIFEST_FANOUT; ++i) worst.entries.push_back({K1, 18446744073709551615ULL});
    const std::size_t worst_len = format_manifest(worst).size();
    check(worst_len <= MAX_CHUNK, "the largest possible manifest is " + std::to_string(worst_len) + " bytes, within one chunk");
}

void test_manifest_tree() {
    auto entries = [](std::size_t n, std::uint64_t chunk, std::uint64_t last) {
        std::vector<ManifestEntry> v;
        for (std::size_t i = 0; i < n; ++i) v.push_back({fake_key(i), i + 1 == n ? last : chunk});
        return v;
    };

    ManifestTree t0 = build_manifests({}, 0, 4096);
    Manifest m;
    std::string why;
    check(t0.blobs.size() == 1 && t0.depth == 1 && parse_manifest(t0.blobs[0], m, why) && m.size == 0 && m.entries.empty(),
          "a zero-byte file has one manifest, with no entries");
    check(t0.root_key == chunk_key(t0.blobs.back()), "the root key is the hash of the last manifest");

    ManifestTree t1 = build_manifests(entries(10, 4096, 100), 9 * 4096 + 100, 4096, 10);
    check(t1.blobs.size() == 1 && t1.depth == 1, "ten chunks at a fan-out of ten fit in one manifest");

    ManifestTree t2 = build_manifests(entries(10, 4096, 100), 9 * 4096 + 100, 4096, 4);
    check(t2.blobs.size() == 4 && t2.depth == 2, "ten chunks at a fan-out of four: three data manifests and a root");
    check(parse_manifest(t2.blobs.back(), m, why) && m.kind == ManifestKind::Index && m.entries.size() == 3 &&
              m.size == 9 * 4096 + 100 && m.entries[0].bytes == 4 * 4096 && m.entries[2].bytes == 4096 + 100,
          "the root lists the three, with the bytes each covers");

    ManifestTree t4 = build_manifests(entries(9, 4096, 1), 8 * 4096 + 1, 4096, 2);
    check(t4.depth == 4 && t4.blobs.size() == 5 + 3 + 2 + 1, "nine chunks at a fan-out of two: four levels (9 -> 5 -> 3 -> 2 -> 1)");

    ManifestTree t5 = build_manifests(entries(17, 4096, 1), 16 * 4096 + 1, 4096, 2);
    check(t5.depth == 5, "seventeen chunks at a fan-out of two would need five levels");

    ManifestTree tf = build_manifests(entries(10, 4096, 1), 9 * 4096 + 1, 4096, 1);
    check(tf.depth >= 1 && tf.depth <= 4, "a fan-out of one is raised to two rather than looping for ever");

    // The real fan-out: 8,193 chunks of 512 KiB need two data manifests and a root.
    ManifestTree big = build_manifests(entries(8193, DEFAULT_CHUNK_SIZE, 1), 8192ULL * DEFAULT_CHUNK_SIZE + 1, DEFAULT_CHUNK_SIZE);
    bool all_fit = true;
    for (const std::string &b : big.blobs) all_fit = all_fit && b.size() <= MAX_CHUNK;
    check(big.depth == 2 && big.blobs.size() == 3 && all_fit, "8,193 chunks: two data manifests and an index, each within one chunk");
    ManifestTree edge = build_manifests(entries(8192, DEFAULT_CHUNK_SIZE, 1), 8191ULL * DEFAULT_CHUNK_SIZE + 1, DEFAULT_CHUNK_SIZE);
    check(edge.depth == 1 && edge.blobs.size() == 1 && edge.blobs[0].size() <= MAX_CHUNK, "exactly 8,192 chunks still fit in one manifest");

    // Identical chunk lists give identical manifests: the name is not inside.
    check(build_manifests(entries(3, 4096, 9), 2 * 4096 + 9, 4096).root_key ==
              build_manifests(entries(3, 4096, 9), 2 * 4096 + 9, 4096).root_key,
          "the same content gives the same root key");
}

void test_resolve() {
    MemoryRing ring;
    std::vector<ManifestEntry> es;
    for (std::size_t i = 0; i < 10; ++i) es.push_back({ring.add("data " + std::to_string(i) + std::string(4091, 'x')), 4096});
    es.back() = {ring.add("tail"), 4};
    const std::uint64_t size = 9 * 4096 + 4;
    // Note: these data chunks are 4,096 + a few bytes; resolve only checks manifests.
    ManifestTree t = build_manifests(es, size, 4096, 4);
    for (const std::string &b : t.blobs) ring.add(b);

    Resolved r = resolve_manifest(t.root_key, ring.get(), 3, 1);
    bool tiled = r.ok && r.chunks.size() == 10;
    std::uint64_t expect = 0;
    for (std::size_t i = 0; tiled && i < r.chunks.size(); ++i) {
        tiled = r.chunks[i].offset == expect && r.chunks[i].key == es[i].key && r.chunks[i].bytes == es[i].bytes;
        expect += r.chunks[i].bytes;
    }
    check(tiled && expect == size && r.depth == 2 && r.manifests == 4, "a two-level tree resolves to the chunks in order, tiling the file");

    ManifestTree deep = build_manifests(std::vector<ManifestEntry>(17, ManifestEntry{K1, 1}), 17, 1, 2);
    for (const std::string &b : deep.blobs) ring.add(b);
    Resolved rd = resolve_manifest(deep.root_key, ring.get(), 1, 1);
    check(!rd.ok && rd.message.find("deeper than 4") != std::string::npos, "a five-level tree is refused: " + rd.message);

    ManifestTree four = build_manifests(std::vector<ManifestEntry>(9, ManifestEntry{K1, 1}), 9, 1, 2);
    for (const std::string &b : four.blobs) ring.add(b);
    check(resolve_manifest(four.root_key, ring.get(), 1, 1).ok, "a four-level tree is accepted");

    // A parent that lies about a child's size.
    const std::string child = ring.add(format_manifest(Manifest{4096, 4096, ManifestKind::Data, {{K1, 4096}}}));
    const std::string liar = ring.add(format_manifest(Manifest{8192, 4096, ManifestKind::Index, {{child, 8192}}}));
    Resolved rl = resolve_manifest(liar, ring.get(), 1, 1);
    check(!rl.ok && rl.message.find("its parent says") != std::string::npos, "a child smaller than its parent says is refused");

    const std::string other = ring.add(format_manifest(Manifest{4096, 2048, ManifestKind::Data, {{K1, 2048}, {K2, 2048}}}));
    const std::string mixed = ring.add(format_manifest(Manifest{4096, 4096, ManifestKind::Index, {{other, 4096}}}));
    check(!resolve_manifest(mixed, ring.get(), 1, 1).ok, "a child with a different chunk size is refused");

    const std::string garbage = ring.add("not a manifest at all\n");
    Resolved rg = resolve_manifest(garbage, ring.get(), 1, 1);
    check(!rg.ok && rg.message.find("not a manifest") != std::string::npos, "a chunk that is not a manifest is refused");

    check(!resolve_manifest(std::string(40, 'c'), ring.get(), 1, 1).ok, "a root key nobody holds fails");
    check(!resolve_manifest("../etc/passwd", ring.get(), 1, 1).ok, "a malformed root key is refused before any fetch");
}

// ---- the worker pool --------------------------------------------------------

void test_pool() {
    std::atomic<bool> stop{false};
    int calls = 0;
    check(run_pool(0, 8, stop, [&](std::size_t) { ++calls; return true; }) && calls == 0, "no work: no job runs");

    std::vector<std::atomic<int>> seen(1000);
    for (auto &s : seen) s = 0;
    std::atomic<bool> stop1{false};
    const bool ok = run_pool(1000, 8, stop1, [&](std::size_t i) { ++seen[i]; return true; });
    bool once = true;
    for (auto &s : seen) once = once && s == 1;
    check(ok && once, "1,000 jobs on 8 workers: every index exactly once");

    std::mutex mu;
    std::set<std::thread::id> threads;
    std::atomic<bool> stop2{false};
    run_pool(3, 64, stop2, [&](std::size_t) {
        std::lock_guard<std::mutex> lock(mu);
        threads.insert(std::this_thread::get_id());
        return true;
    });
    check(threads.size() <= 3, "64 workers for 3 jobs start at most 3 threads");

    std::atomic<int> started{0};
    std::atomic<bool> stop3{false};
    const bool failed = !run_pool(200, 4, stop3, [&](std::size_t i) {
        ++started;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return i != 5;
    });
    check(failed && started < 20, "one failing job stops the others taking new work (" + std::to_string(started.load()) + " of 200 started)");

    std::atomic<bool> stop4{false};
    check(!run_pool(10, 2, stop4, [&](std::size_t i) -> bool {
              if (i == 3) throw std::runtime_error("boom");
              return true;
          }),
          "a job that throws fails the pool instead of ending the process");
}

// ---- whole transfers --------------------------------------------------------

void round_trip(const std::string &dir, std::size_t bytes, int workers, std::uint64_t chunk, std::mt19937_64 &rng,
                const std::string &label) {
    MemoryRing ring;
    const std::string src = dir + "/src-" + label, dst = dir + "/dst-" + label;
    const std::string data = random_bytes(bytes, rng);
    write_file(src, data);
    const UploadOutcome u = upload_file(src, ring.put(), fast(workers, chunk));
    std::string got;
    const DownloadOutcome d = u.ok ? download_file(u.root_key, dst, ring.get(), fast(workers, chunk)) : DownloadOutcome{};
    const std::size_t expect_chunks = (bytes + chunk - 1) / chunk;
    check(u.ok && d.ok && read_file(dst, got) && got == data && u.chunks == expect_chunks && d.chunks == expect_chunks &&
              d.size == bytes,
          label + ": " + std::to_string(bytes) + " bytes in " + std::to_string(expect_chunks) + " chunks, " +
              std::to_string(workers) + " workers, round trip" + (u.ok ? (d.ok ? "" : " -- download: " + d.message) : " -- upload: " + u.message));
}

void test_round_trips() {
    const std::string dir = make_tmpdir();
    std::mt19937_64 rng(20261008);
    round_trip(dir, 0, 4, 4096, rng, "empty");
    round_trip(dir, 1, 4, 4096, rng, "one");
    round_trip(dir, 4095, 4, 4096, rng, "minus1");
    round_trip(dir, 4096, 4, 4096, rng, "exact");
    round_trip(dir, 4097, 4, 4096, rng, "plus1");
    round_trip(dir, 3 * 4096 + 17, 1, 4096, rng, "multi-1worker");
    round_trip(dir, 100 * 4096 + 5, 16, 4096, rng, "multi-16workers");
    round_trip(dir, 2 * DEFAULT_CHUNK_SIZE + 3, 4, DEFAULT_CHUNK_SIZE, rng, "default-chunk");
    bool no_parts = true;
    for (const std::string &n : listing(dir)) no_parts = no_parts && n.find(".part-") == std::string::npos;
    check(no_parts, "no part file is left after successful downloads");

    // A tree on a small file: fan-out 2, nine chunks, four levels.
    MemoryRing ring;
    const std::string data = random_bytes(8 * 4096 + 1, rng);
    write_file(dir + "/tree-src", data);
    TransferOptions o = fast(3);
    o.fanout = 2;
    const UploadOutcome u = upload_file(dir + "/tree-src", ring.put(), o);
    std::string got;
    const DownloadOutcome d = download_file(u.root_key, dir + "/tree-dst", ring.get(), fast(3));
    check(u.ok && u.depth == 4 && u.manifests == 11 && d.ok && d.depth == 4 && read_file(dir + "/tree-dst", got) && got == data,
          "a nine-chunk file at fan-out two: eleven manifests in four levels, round trip");

    write_file(dir + "/deep-src", random_bytes(16 * 4096 + 1, rng));
    const UploadOutcome deep = upload_file(dir + "/deep-src", ring.put(), o);
    check(!deep.ok && deep.message.find("levels") != std::string::npos, "an upload that would need five levels is refused: " + deep.message);

    // Deduplication: the same bytes twice store nothing new.
    MemoryRing dedup;
    write_file(dir + "/dup", data);
    const UploadOutcome a = upload_file(dir + "/dup", dedup.put(), fast());
    const std::size_t held = dedup.chunks.size();
    const UploadOutcome b = upload_file(dir + "/dup", dedup.put(), fast());
    check(a.ok && b.ok && a.root_key == b.root_key && dedup.chunks.size() == held, "the same file twice: the same root key, nothing new stored");
}

void test_failures() {
    const std::string dir = make_tmpdir();
    std::mt19937_64 rng(7);
    MemoryRing ring;
    const std::string data = random_bytes(10 * 4096 + 9, rng);
    write_file(dir + "/src", data);
    const UploadOutcome u = upload_file(dir + "/src", ring.put(), fast());
    Resolved r = resolve_manifest(u.root_key, ring.get(), 1, 1);
    check(u.ok && r.ok && r.chunks.size() == 11, "setup: an 11-chunk file stored");

    // A chunk that fails twice and then answers: retried, and the download succeeds.
    ring.fail_get[r.chunks[4].key] = 2;
    DownloadOutcome d1 = download_file(u.root_key, dir + "/retry", ring.get(), fast());
    std::string got;
    check(d1.ok && read_file(dir + "/retry", got) && got == data && d1.timings[4].attempts == 3,
          "a chunk that fails twice is fetched on its third attempt");

    // A chunk that never answers: the download fails, and leaves nothing behind.
    write_file(dir + "/keep", "the previous version");
    ring.fail_get[r.chunks[7].key] = -1;
    const std::set<std::string> before = listing(dir);
    DownloadOutcome d2 = download_file(u.root_key, dir + "/fresh", ring.get(), fast());
    check(!d2.ok && d2.message.find("chunk 7") != std::string::npos, "a lost chunk fails the download, naming it: " + d2.message);
    check(!exists(dir + "/fresh") && listing(dir) == before, "and leaves no file and no part file");
    DownloadOutcome d3 = download_file(u.root_key, dir + "/keep", ring.get(), fast());
    check(!d3.ok && read_file(dir + "/keep", got) && got == "the previous version", "a failed download leaves an existing file untouched");
    ring.fail_get.clear();

    // A manifest that lies about a chunk's length.
    const std::string abc = ring.add("abc");
    const std::string liar = ring.add(format_manifest(Manifest{4, 4096, ManifestKind::Data, {{abc, 4}}}));
    DownloadOutcome d4 = download_file(liar, dir + "/liar", ring.get(), fast());
    check(!d4.ok && d4.message.find("the manifest says 4") != std::string::npos && !exists(dir + "/liar"),
          "a chunk whose length disagrees with its manifest fails the download");

    // Destinations that cannot work fail before or without leaving anything.
    check(!download_file(u.root_key, dir, ring.get(), fast()).ok, "a destination that is a directory is refused");
    check(!download_file(u.root_key, dir + "/no/such/dir/f", ring.get(), fast()).ok, "a destination in a missing directory fails");
    check(!upload_file(dir, ring.put(), fast()).ok, "uploading a directory is refused");
    check(!upload_file(dir + "/nothing-here", ring.put(), fast()).ok, "uploading a missing file is refused");
    TransferOptions bad = fast();
    bad.workers = 0;
    check(!upload_file(dir + "/src", ring.put(), bad).ok && !download_file(u.root_key, dir + "/x", ring.get(), bad).ok,
          "zero workers is refused");
    bad = fast();
    bad.chunk = MAX_CHUNK + 1;
    check(!upload_file(dir + "/src", ring.put(), bad).ok, "a chunk size above the limit is refused");

    // A file that grows while it is being uploaded.
    write_file(dir + "/growing", random_bytes(6 * 4096, rng));
    std::atomic<bool> grown{false};
    PutFn growing_put = [&](const std::string &body) {
        if (!grown.exchange(true)) {
            std::ofstream f(dir + "/growing", std::ios::binary | std::ios::app);
            f << "more";
        }
        return ring.put()(body);
    };
    const UploadOutcome g = upload_file(dir + "/growing", growing_put, fast(1));
    check(!g.ok && g.message.find("changed") != std::string::npos, "a file that grows during its upload is refused: " + g.message);

    // A put that keeps failing fails the upload, naming the chunk.
    PutFn failing = [](const std::string &) { PutOutcome p; p.message = "injected"; return p; };
    const UploadOutcome f = upload_file(dir + "/src", failing, fast());
    check(!f.ok && f.message.find("injected") != std::string::npos, "a chunk that cannot be stored fails the upload");
}

void test_signal_cleanup() {
    const std::string dir = make_tmpdir();
    std::mt19937_64 rng(11);
    MemoryRing ring;
    write_file(dir + "/src", random_bytes(5 * 4096, rng));
    const UploadOutcome u = upload_file(dir + "/src", ring.put(), fast());

    // As if SIGINT arrived while chunk 2 was in flight: the handler's function runs,
    // the part file goes, and the download cannot then produce the destination.
    GetFn base = ring.get();
    Resolved r = resolve_manifest(u.root_key, base, 1, 1);
    GetFn interrupted = [&](const std::string &key) {
        if (key == r.chunks[2].key) remove_part_file();
        return base(key);
    };
    TransferOptions o = fast(1);
    o.for_signals = true;
    DownloadOutcome d = download_file(u.root_key, dir + "/dst", interrupted, o);
    bool part_left = false;
    for (const std::string &n : listing(dir)) part_left = part_left || n.find(".part-") != std::string::npos;
    check(!d.ok && !exists(dir + "/dst") && !part_left, "the signal handler's cleanup removes the part file mid-download");

    DownloadOutcome ok = download_file(u.root_key, dir + "/dst2", base, o);
    remove_part_file();                            // after success: nothing to remove
    check(ok.ok && exists(dir + "/dst2"), "after a download completes, the cleanup leaves the file alone");
}

}  // namespace

int main() {
    test_manifest_format();
    test_manifest_tree();
    test_resolve();
    test_pool();
    test_round_trips();
    test_failures();
    test_signal_cleanup();
    remove_tmpdirs();
    if (failures == 0) {
        std::printf("PASS  %d/%d checks passed\n", checks, checks);
        return 0;
    }
    std::printf("FAIL  %d of %d checks failed\n", failures, checks);
    return 1;
}
