#include "transfer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

long long micros_since(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
}

constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();

// A canonical unsigned decimal: digits only, no sign, no leading zero (a lone "0"
// is zero), and no larger than 2^64 - 1. Strict for the same reason body_length
// is: two spellings of one number would let two different manifests mean the same.
bool parse_u64(const std::string &s, std::uint64_t &out) {
    if (s.empty() || s.size() > 20) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (v > (U64_MAX - d) / 10) return false;      // v * 10 + d would wrap
        v = v * 10 + d;
    }
    out = v;
    return true;
}

// "<name> <number>" exactly, as a manifest header line.
bool header_number(const std::string &line, const std::string &name, std::uint64_t &out) {
    if (line.compare(0, name.size() + 1, name + " ") != 0) return false;
    return parse_u64(line.substr(name.size() + 1), out);
}

// The directory a path is in: "a/b/f" -> "a/b", "f" -> ".".
std::string parent_of(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// A rename changes the directory, not the file: flush the directory too (D-045).
bool fsync_dir(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

// pread and pwrite take the offset as an argument instead of moving a shared file
// position, so many workers can use one descriptor at once without a lock. Both
// may move fewer bytes than asked -- loop, as for a socket.
bool pread_all(int fd, char *data, std::size_t len, std::uint64_t offset, std::size_t &got) {
    got = 0;
    while (got < len) {
        const ssize_t n = pread(fd, data + got, len - got, static_cast<off_t>(offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (n == 0) return true;                       // end of file: `got` says how far
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool pwrite_all(int fd, const char *data, std::size_t len, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = pwrite(fd, data + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Up to `attempts` tries, waiting backoff_ms after the first failure and twice that
// after the second (D-056) -- about half and one stabilisation period, so a node
// that has just died can be evicted in between. Gives up early once `stop` is set:
// another chunk has already failed the transfer.
GetOutcome get_attempts(const GetFn &get, const std::string &key, int attempts, int backoff_ms,
                        const std::atomic<bool> *stop, int &used) {
    GetOutcome g;
    for (used = 1;; ++used) {
        g = get(key);
        if (g.ok || used >= attempts || (stop && stop->load())) return g;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * used));
    }
}

PutOutcome put_attempts(const PutFn &put, const std::string &body, int attempts, int backoff_ms,
                        const std::atomic<bool> *stop, int &used) {
    PutOutcome p;
    for (used = 1;; ++used) {
        p = put(body);
        if (p.ok || used >= attempts || (stop && stop->load())) return p;
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms * used));
    }
}

// The first failure of a transfer, kept for its message. Workers report through it
// from many threads, so it has its own lock.
struct FirstFailure {
    std::mutex  mu;
    std::string message;
    bool set(const std::string &why) {           // always returns false, for `return f.set(...)`
        std::lock_guard<std::mutex> lock(mu);
        if (message.empty()) message = why;
        return false;
    }
};

std::string tried_list(const GetOutcome &g) {
    std::string s;
    for (const std::string &t : g.tried) s += (s.empty() ? "" : "; ") + t;
    return s;
}

// The part file a signal handler may remove (D-055). The path is written into a
// fixed buffer BEFORE the flag is set, and the flag is a lock-free atomic: a
// handler that sees the flag set therefore sees the whole path, and reading a
// lock-free atomic is safe inside a signal handler, where taking a lock is not.
char              g_part_path[4096];
std::atomic<bool> g_part_live{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the part-file flag must be safe to read in a signal handler");

}  // namespace

void remove_part_file() {
    if (g_part_live.load()) unlink(g_part_path);
}

// ---------------------------------------------------------------------------
// Manifests (D-054)
// ---------------------------------------------------------------------------

std::string format_manifest(const Manifest &m) {
    std::string s = "chordfs-manifest 1\n";
    s += "size " + std::to_string(m.size) + "\n";
    s += "chunk " + std::to_string(m.chunk) + "\n";
    s += std::string("kind ") + (m.kind == ManifestKind::Data ? "data" : "index") + "\n";
    s += "entries " + std::to_string(m.entries.size()) + "\n";
    for (const ManifestEntry &e : m.entries) s += e.key + " " + std::to_string(e.bytes) + "\n";
    return s;
}

bool parse_manifest(const std::string &text, Manifest &m, std::string &why) {
    // Every line ends in '\n', the last included, so a manifest cut short is caught.
    // The first line first: something that is not a manifest at all says so.
    if (text.compare(0, 19, "chordfs-manifest 1\n") != 0) { why = "not a manifest: the first line is not 'chordfs-manifest 1'"; return false; }
    if (text.back() != '\n') { why = "the manifest does not end with a newline"; return false; }
    // Count lines before splitting, so a hostile manifest of half a million empty
    // lines is refused without building half a million strings.
    const std::size_t lines_total = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    if (lines_total < 5) { why = "the manifest header is incomplete"; return false; }
    if (lines_total - 5 > MANIFEST_FANOUT) { why = "the manifest has more than " + std::to_string(MANIFEST_FANOUT) + " entries"; return false; }

    std::vector<std::string> lines;
    lines.reserve(lines_total);
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }

    // The header: five lines, in this order, nothing else.
    std::uint64_t size = 0, chunk = 0, count = 0;
    if (lines[0] != "chordfs-manifest 1") { why = "not a manifest: the first line is not 'chordfs-manifest 1'"; return false; }
    if (!header_number(lines[1], "size", size)) { why = "bad size line"; return false; }
    if (!header_number(lines[2], "chunk", chunk) || chunk == 0 || chunk > MAX_CHUNK) {
        why = "bad chunk line (the chunk size must be 1 to " + std::to_string(MAX_CHUNK) + ")";
        return false;
    }
    ManifestKind kind;
    if (lines[3] == "kind data") kind = ManifestKind::Data;
    else if (lines[3] == "kind index") kind = ManifestKind::Index;
    else { why = "bad kind line"; return false; }
    if (!header_number(lines[4], "entries", count) || count != lines.size() - 5) {
        why = "the entries line does not match the number of entries";
        return false;
    }

    // The entries: "<40-hex key> <bytes>", summed without wrapping.
    std::vector<ManifestEntry> entries;
    entries.reserve(static_cast<std::size_t>(count));
    std::uint64_t sum = 0;
    for (std::size_t i = 5; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        ManifestEntry e;
        if (l.size() < 42 || l[40] != ' ' || !valid_key(e.key = l.substr(0, 40)) || !parse_u64(l.substr(41), e.bytes)) {
            why = "bad entry on line " + std::to_string(i + 1);
            return false;
        }
        if (e.bytes > U64_MAX - sum) { why = "the entries' byte counts overflow"; return false; }
        sum += e.bytes;
        entries.push_back(std::move(e));
    }
    if (sum != size) { why = "the entries add up to " + std::to_string(sum) + " bytes, not " + std::to_string(size); return false; }

    // The shape rule. Data: every chunk is full except the last, which is 1 to
    // `chunk` bytes -- with the sum checked, that also fixes how many there are.
    // Index: at least one child, none empty, every child but the last a whole
    // number of chunks, so offsets agree at every level.
    const std::size_t n = entries.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t b = entries[i].bytes;
        const bool last = i + 1 == n;
        const bool fits = kind == ManifestKind::Data ? (last ? (b >= 1 && b <= chunk) : b == chunk)
                                                     : (b >= 1 && (last || b % chunk == 0));
        if (!fits) { why = "entry " + std::to_string(i + 1) + " has the wrong byte count for a " +
                           (kind == ManifestKind::Data ? "data" : "index") + " manifest"; return false; }
    }
    if (kind == ManifestKind::Index && n == 0) { why = "an index manifest lists no manifests"; return false; }

    m.size = size;
    m.chunk = chunk;
    m.kind = kind;
    m.entries = std::move(entries);
    return true;
}

ManifestTree build_manifests(const std::vector<ManifestEntry> &chunks, std::uint64_t size,
                             std::uint64_t chunk, std::size_t fanout) {
    // A fan-out of 1 would never shrink a level, and the loop below would not end.
    fanout = std::max<std::size_t>(2, std::min(fanout, MANIFEST_FANOUT));

    ManifestTree tree;
    std::vector<ManifestEntry> level = chunks;
    ManifestKind kind = ManifestKind::Data;
    while (true) {
        ++tree.depth;
        // A level that fits in one manifest is the root -- including a level of
        // zero entries, the manifest of a zero-byte file.
        if (level.size() <= fanout) {
            tree.blobs.push_back(format_manifest(Manifest{size, chunk, kind, level}));
            tree.root_key = chunk_key(tree.blobs.back());
            return tree;
        }
        // Otherwise cut it into manifests of `fanout` entries; each becomes one entry
        // of the next level, carrying the bytes it covers.
        std::vector<ManifestEntry> next;
        for (std::size_t i = 0; i < level.size(); i += fanout) {
            const std::size_t end = std::min(level.size(), i + fanout);
            Manifest m{0, chunk, kind, std::vector<ManifestEntry>(level.begin() + i, level.begin() + end)};
            for (const ManifestEntry &e : m.entries) m.size += e.bytes;
            tree.blobs.push_back(format_manifest(m));
            next.push_back(ManifestEntry{chunk_key(tree.blobs.back()), m.size});
        }
        level = std::move(next);
        kind = ManifestKind::Index;
    }
}

Resolved resolve_manifest(const std::string &root_key, const GetFn &get, int attempts, int backoff_ms) {
    Resolved r;
    if (!valid_key(root_key)) { r.message = "the root key is not 40 lowercase hexadecimal characters"; return r; }

    // One level of the tree at a time. Each manifest to read carries where its bytes
    // start in the file, how many its parent says it covers, and its depth.
    struct Pending { std::string key; std::uint64_t offset; std::uint64_t bytes; int depth; };
    std::vector<Pending> level{Pending{root_key, 0, 0, 1}};
    while (!level.empty()) {
        std::vector<Pending> next;
        for (const Pending &p : level) {
            if (p.depth > MAX_MANIFEST_DEPTH) {
                r.message = "manifest " + p.key + " is deeper than " + std::to_string(MAX_MANIFEST_DEPTH) + " levels";
                return r;
            }
            int used = 0;
            const GetOutcome g = get_attempts(get, p.key, attempts, backoff_ms, nullptr, used);
            if (!g.ok) { r.message = "manifest " + p.key + ": " + g.message; return r; }
            Manifest m;
            std::string why;
            if (!parse_manifest(g.body, m, why)) { r.message = "manifest " + p.key + ": " + why; return r; }
            ++r.manifests;
            if (p.depth == 1) {
                r.size = m.size;
                r.chunk = m.chunk;
            } else if (m.size != p.bytes) {
                r.message = "manifest " + p.key + " covers " + std::to_string(m.size) + " bytes; its parent says " +
                            std::to_string(p.bytes);
                return r;
            } else if (m.chunk != r.chunk) {
                r.message = "manifest " + p.key + " uses a different chunk size from the root's";
                return r;
            }
            r.depth = std::max(r.depth, p.depth);
            // Every entry's bytes start where the previous entry's ended. The sums were
            // checked inside each manifest, and each child covers exactly what its
            // parent said, so by induction the chunks tile [0, size) once each.
            std::uint64_t offset = p.offset;
            for (const ManifestEntry &e : m.entries) {
                if (m.kind == ManifestKind::Data) r.chunks.push_back(ChunkRef{e.key, offset, e.bytes});
                else next.push_back(Pending{e.key, offset, e.bytes, p.depth + 1});
                offset += e.bytes;
            }
        }
        level = std::move(next);
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// The worker pool (D-056)
// ---------------------------------------------------------------------------

bool run_pool(std::size_t n, int workers, std::atomic<bool> &stop, const std::function<bool(std::size_t)> &job) {
    std::atomic<std::size_t> next{0};
    auto work = [&]() {
        while (!stop.load()) {
            // fetch_add returns the value before adding, in one indivisible step: no
            // two workers ever get the same i, and no i is skipped.
            const std::size_t i = next.fetch_add(1);
            if (i >= n) return;
            bool ok = false;
            try {
                ok = job(i);
            } catch (...) {          // an exception escaping a thread would end the process
                ok = false;
            }
            if (!ok) stop.store(true);
        }
    };

    const std::size_t count = std::min<std::size_t>(n, static_cast<std::size_t>(std::max(1, workers)));
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < count; ++t) {
        try {
            threads.emplace_back(work);
        } catch (const std::system_error &) {   // the system would not start another thread:
            break;                              // carry on with the ones that did start
        }
    }
    if (threads.empty()) work();                // not even one: do the work on this thread
    for (std::thread &t : threads) t.join();
    return !stop.load();
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

UploadOutcome upload_file(const std::string &path, const PutFn &put, const TransferOptions &opt) {
    UploadOutcome out;
    if (opt.workers < 1 || opt.workers > MAX_WORKERS) { out.message = "workers must be 1 to " + std::to_string(MAX_WORKERS); return out; }
    if (opt.chunk < 1 || opt.chunk > MAX_CHUNK) { out.message = "the chunk size must be 1 to " + std::to_string(MAX_CHUNK); return out; }
    if (opt.fanout < 2 || opt.fanout > MANIFEST_FANOUT) { out.message = "the fan-out must be 2 to " + std::to_string(MANIFEST_FANOUT); return out; }

    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { out.message = "cannot open " + path + ": " + std::strerror(errno); return out; }
    struct stat before{};
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) {
        out.message = path + " is not a regular file";
        close(fd);
        return out;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(before.st_size);
    const std::uint64_t chunk = opt.chunk;
    const std::size_t n = static_cast<std::size_t>((size + chunk - 1) / chunk);
    out.size = size;
    out.chunks = n;

    // One slot per chunk. Each worker writes only the slots of the chunks it took,
    // and two different elements of a vector are two different objects in memory,
    // so the workers need no lock for these -- only for the shared failure message.
    std::vector<std::string> keys(n);
    out.timings.resize(n);
    FirstFailure failure;
    std::atomic<bool> stop{false};

    const auto t0 = Clock::now();
    const bool chunks_ok = run_pool(n, opt.workers, stop, [&](std::size_t i) -> bool {
        const auto c0 = Clock::now();
        const std::uint64_t offset = static_cast<std::uint64_t>(i) * chunk;
        const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - offset));
        std::string body(len, '\0');
        std::size_t got = 0;
        if (!pread_all(fd, &body[0], len, offset, got))
            return failure.set("reading chunk " + std::to_string(i) + ": " + std::strerror(errno));
        if (got != len) return failure.set(path + " shrank while it was being read");

        int used = 0;
        const PutOutcome p = put_attempts(put, body, CHUNK_ATTEMPTS, opt.backoff_ms, &stop, used);
        out.timings[i] = ChunkTiming{i, len, micros_since(c0), used, p.attempts, p.ok};
        if (!p.ok) return failure.set("chunk " + std::to_string(i) + ": " + p.message);
        keys[i] = p.key;
        if (opt.log && (used > 1 || p.attempts > 1))
            opt.log("chunk " + std::to_string(i) + " stored after " + std::to_string(used) + " attempt(s) and " +
                    std::to_string(p.attempts) + " coordinator(s)");
        return true;
    });
    out.chunk_micros = micros_since(t0);

    // A file that changed while it was read would be stored as a mixture of two
    // versions. The size and modification time are compared before and after.
    struct stat after{};
    const bool unchanged = fstat(fd, &after) == 0 && after.st_size == before.st_size &&
                           after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
                           after.st_mtim.tv_nsec == before.st_mtim.tv_nsec;
    close(fd);
    if (!chunks_ok) { out.message = failure.message.empty() ? "a worker failed" : failure.message; return out; }
    if (!unchanged) { out.message = path + " changed while it was being read"; return out; }

    // The manifests: built from the keys, then stored like any chunk (D-054, D-017).
    const auto m0 = Clock::now();
    std::vector<ManifestEntry> entries(n);
    for (std::size_t i = 0; i < n; ++i)
        entries[i] = ManifestEntry{keys[i], std::min<std::uint64_t>(chunk, size - static_cast<std::uint64_t>(i) * chunk)};
    const ManifestTree tree = build_manifests(entries, size, chunk, opt.fanout);
    // Only a fan-out far below the default can need more levels than a reader
    // accepts; storing such a tree would make a file nobody can read.
    if (tree.depth > MAX_MANIFEST_DEPTH) {
        out.message = "at a fan-out of " + std::to_string(opt.fanout) + " this file needs " + std::to_string(tree.depth) +
                      " levels of manifests; a reader accepts " + std::to_string(MAX_MANIFEST_DEPTH);
        return out;
    }
    std::atomic<bool> stop_manifests{false};
    const bool manifests_ok = run_pool(tree.blobs.size(), opt.workers, stop_manifests, [&](std::size_t i) -> bool {
        int used = 0;
        const PutOutcome p = put_attempts(put, tree.blobs[i], CHUNK_ATTEMPTS, opt.backoff_ms, &stop_manifests, used);
        return p.ok || failure.set("manifest " + std::to_string(i + 1) + " of " + std::to_string(tree.blobs.size()) +
                                   ": " + p.message);
    });
    out.manifest_micros = micros_since(m0);
    if (!manifests_ok) { out.message = failure.message.empty() ? "a worker failed" : failure.message; return out; }

    out.ok = true;
    out.root_key = tree.root_key;
    out.manifests = tree.blobs.size();
    out.depth = tree.depth;
    return out;
}

// ---------------------------------------------------------------------------
// Download (D-055)
// ---------------------------------------------------------------------------

DownloadOutcome download_file(const std::string &root_key, const std::string &dest, const GetFn &get,
                              const TransferOptions &opt) {
    DownloadOutcome out;
    if (opt.workers < 1 || opt.workers > MAX_WORKERS) { out.message = "workers must be 1 to " + std::to_string(MAX_WORKERS); return out; }
    struct stat st{};
    if (stat(dest.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { out.message = dest + " is a directory"; return out; }

    // 1. Every manifest, fetched and checked, before anything touches the disk.
    const auto r0 = Clock::now();
    const Resolved r = resolve_manifest(root_key, get, CHUNK_ATTEMPTS, opt.backoff_ms);
    out.resolve_micros = micros_since(r0);
    if (!r.ok) { out.message = r.message; return out; }
    out.size = r.size;
    out.chunks = r.chunks.size();
    out.manifests = r.manifests;
    out.depth = r.depth;
    if (opt.log) opt.log("manifest " + root_key + ": " + std::to_string(r.size) + " bytes, " +
                         std::to_string(r.chunks.size()) + " chunks, " + std::to_string(r.manifests) + " manifest(s)");

    // 2. The part file, beside the destination: rename is atomic only within one
    // filesystem. O_EXCL: never open a file that is already there.
    static std::atomic<unsigned> counter{0};
    const std::string part = dest + ".part-" + std::to_string(getpid()) + "-" + std::to_string(counter.fetch_add(1));
    if (opt.for_signals) {
        if (part.size() >= sizeof g_part_path) { out.message = "the destination path is too long"; return out; }
        std::memcpy(g_part_path, part.c_str(), part.size() + 1);
        g_part_live.store(true);             // after the path is complete: see g_part_path
    }
    int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    auto abandon = [&](const std::string &why) {
        if (fd >= 0) close(fd);
        unlink(part.c_str());                // nothing that looks like the file is left behind
        if (opt.for_signals) g_part_live.store(false);
        out.message = why;
        return out;
    };
    if (fd < 0) return abandon("cannot create " + part + ": " + std::strerror(errno));
    // Sized up front: the bytes in between stay holes until their chunks land. The
    // part file's NAME says it is incomplete; its size says nothing either way.
    if (ftruncate(fd, static_cast<off_t>(r.size)) != 0) return abandon("cannot size " + part + ": " + std::strerror(errno));

    // 3. The chunks, verified (get_chunk hashes them) and written at their offsets.
    FirstFailure failure;
    std::atomic<bool> stop{false};
    out.timings.resize(r.chunks.size());
    const auto f0 = Clock::now();
    const bool ok = run_pool(r.chunks.size(), opt.workers, stop, [&](std::size_t i) -> bool {
        const ChunkRef &c = r.chunks[i];
        const auto c0 = Clock::now();
        int used = 0;
        const GetOutcome g = get_attempts(get, c.key, CHUNK_ATTEMPTS, opt.backoff_ms, &stop, used);
        ChunkTiming &t = out.timings[i];
        t = ChunkTiming{i, c.bytes, 0, used, static_cast<int>(g.tried.size()), false};
        if (!g.ok) return failure.set("chunk " + std::to_string(i) + " (" + c.key + "): " + g.message);
        // A chunk can hash to its key and still be the wrong length for its place, if
        // the manifest lies about it. Its length is checked against the manifest.
        if (g.body.size() != c.bytes)
            return failure.set("chunk " + std::to_string(i) + " is " + std::to_string(g.body.size()) +
                               " bytes; the manifest says " + std::to_string(c.bytes));
        if (!pwrite_all(fd, g.body.data(), g.body.size(), c.offset))
            return failure.set("writing chunk " + std::to_string(i) + ": " + std::strerror(errno));
        t.micros = micros_since(c0);
        t.ok = true;
        if (opt.log && (used > 1 || g.tried.size() > 1))
            opt.log("chunk " + std::to_string(i) + " after " + std::to_string(used) + " attempt(s): " + tried_list(g));
        return true;
    });
    out.fetch_micros = micros_since(f0);
    if (!ok) return abandon(failure.message.empty() ? "a worker failed" : failure.message);

    // 4. Flush, then rename into place, then flush the directory (D-055, D-045).
    const auto s0 = Clock::now();
    if (fsync(fd) != 0) return abandon("cannot flush " + part + ": " + std::strerror(errno));
    const int closing = fd;
    fd = -1;
    if (close(closing) != 0) return abandon("cannot close " + part + ": " + std::strerror(errno));
    if (rename(part.c_str(), dest.c_str()) != 0) return abandon("cannot rename " + part + " to " + dest + ": " + std::strerror(errno));
    // A handler that runs after the rename and before this store unlinks a name that
    // no longer exists, which is harmless -- so the order of these two lines is free.
    if (opt.for_signals) g_part_live.store(false);
    if (!fsync_dir(parent_of(dest)) && opt.log)
        opt.log("warning: " + parent_of(dest) + " could not be flushed; the new name may not survive a power cut");
    out.sync_micros = micros_since(s0);
    out.ok = true;
    return out;
}
