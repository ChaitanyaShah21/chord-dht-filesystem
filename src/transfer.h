#ifndef TRANSFER_H
#define TRANSFER_H

#include "chunk.h"
#include "store.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Moving whole files through the ring (Phase 5).
//
//   upload:   file -> chunks -> put_chunk each, in parallel -> manifests -> root key
//   download: root key -> manifests (checked) -> get_chunk each, in parallel
//             -> part file -> fsync -> rename over the destination
//
// Like chunk.cpp and stabilize.cpp, nothing here opens a socket: storing and
// fetching one chunk are passed in (PutFn, GetFn), so the tests run whole
// transfers against a ring that is a map in memory -- including a ring that loses
// chunks, corrupts them, or lies in its manifests. The tracker is not involved at
// all: registering the root key under a name is the caller's job (D-017).
// ---------------------------------------------------------------------------

constexpr std::uint64_t DEFAULT_CHUNK_SIZE = 512 * 1024;   // D-016; at most MAX_CHUNK
constexpr std::size_t   MANIFEST_FANOUT    = 8192;         // entries per manifest at most (D-054)
constexpr int           MAX_MANIFEST_DEPTH = 4;            // levels of manifests at most (D-054)
constexpr int           DEFAULT_WORKERS    = 8;            // D-056: set from the measured curve
constexpr int           MAX_WORKERS        = 64;
constexpr int           CHUNK_ATTEMPTS     = 3;            // tries per chunk before the transfer fails (D-056)

// ---- manifests (D-054) ------------------------------------------------------

enum class ManifestKind { Data, Index };

struct ManifestEntry {
    std::string   key;      // a data chunk's key, or a child manifest's
    std::uint64_t bytes;    // bytes of the file it covers
};

struct Manifest {
    std::uint64_t              size = 0;        // bytes of the file this manifest covers
    std::uint64_t              chunk = 0;       // the chunk size the file was cut with
    ManifestKind               kind = ManifestKind::Data;
    std::vector<ManifestEntry> entries;
};

// The text form:
//   chordfs-manifest 1\n size <n>\n chunk <n>\n kind data|index\n entries <n>\n <key> <bytes>\n ...
std::string format_manifest(const Manifest &m);

// Strict: the exact header lines, canonical numbers, entries equal to the count
// and at most MANIFEST_FANOUT, valid keys, the byte counts summing to `size`
// without overflow, and the shape rule for the kind (D-054). On failure returns
// false with `why` set, and `m` is not to be used.
bool parse_manifest(const std::string &text, Manifest &m, std::string &why);

// Every manifest needed for a file whose chunks are `chunks` (in file order),
// child manifests before their parents and the root last. `fanout` (2 to
// MANIFEST_FANOUT) is the most entries per manifest; tests pass a small one to
// make a ten-chunk file need a tree.
struct ManifestTree {
    std::vector<std::string> blobs;   // every manifest's text; blobs.back() is the root
    std::string              root_key;
    int                      depth = 0;
};
ManifestTree build_manifests(const std::vector<ManifestEntry> &chunks, std::uint64_t size,
                             std::uint64_t chunk, std::size_t fanout = MANIFEST_FANOUT);

// ---- one chunk, stored or fetched -------------------------------------------

// Store one chunk through the ring (the real one is put_chunk with the client's
// entry points and W), and fetch one (get_chunk, which verifies the hash). Both
// may be called from many workers at once.
using PutFn = std::function<PutOutcome(const std::string &body)>;
using GetFn = std::function<GetOutcome(const std::string &key)>;

// One chunk in a file: where it goes, and how long it is.
struct ChunkRef {
    std::string   key;
    std::uint64_t offset;
    std::uint64_t bytes;
};

// The flat list of a file's chunks, from its root manifest (D-054). Every
// manifest is fetched through `get` -- so it is checked against its key -- then
// parsed, and each child's own size must equal what its parent said, its chunk
// size the root's, and its depth at most MAX_MANIFEST_DEPTH.
struct Resolved {
    bool                  ok = false;
    std::string           message;          // on failure: what was wrong, and where
    std::uint64_t         size = 0;
    std::uint64_t         chunk = 0;
    std::vector<ChunkRef> chunks;           // tiling [0, size), each chunk once
    int                   depth = 0;
    std::size_t           manifests = 0;
};
Resolved resolve_manifest(const std::string &root_key, const GetFn &get, int attempts = CHUNK_ATTEMPTS,
                          int backoff_ms = 250);

// ---- whole transfers (D-055, D-056) -----------------------------------------

struct ChunkTiming;                                // below: how one chunk went

struct TransferOptions {
    int           workers = DEFAULT_WORKERS;       // 1..MAX_WORKERS
    std::uint64_t chunk = DEFAULT_CHUNK_SIZE;      // upload only: 1..MAX_CHUNK
    std::size_t   fanout = MANIFEST_FANOUT;        // upload only: 2..MANIFEST_FANOUT
    int           backoff_ms = 250;                // wait before attempt 2; twice that before 3
    bool          for_signals = false;             // download: make the part file removable by
                                                   // remove_part_file() from a signal handler
    std::function<void(const std::string &)> log;  // progress and fall-through notes; may be empty
    // Told about every chunk as soon as it is done or has failed (for a client's
    // live metrics, D-085). Called from the workers, so it must be thread-safe.
    std::function<void(const ChunkTiming &)> on_chunk;
};

// How one chunk went: for the latency percentiles (D-066 item 3).
struct ChunkTiming {
    std::uint64_t index = 0;
    std::uint64_t bytes = 0;
    long long     micros = 0;      // first attempt's start to bytes stored (or verified and written)
    int           attempts = 0;
    int           tried = 0;       // replicas or coordinators tried in the last attempt
    bool          ok = false;
};

struct UploadOutcome {
    bool                     ok = false;
    std::string              message;
    std::string              root_key;
    std::uint64_t            size = 0;
    std::size_t              chunks = 0;
    std::size_t              manifests = 0;
    int                      depth = 0;
    long long                chunk_micros = 0;      // the data chunks, all workers
    long long                manifest_micros = 0;   // building and storing the manifests
    std::vector<ChunkTiming> timings;
};

// Cut the regular file at `path` into opt.chunk-byte chunks, store them with
// opt.workers workers, then build and store the manifests. Fails -- and says why --
// if any chunk fails CHUNK_ATTEMPTS times, or if the file changes while it is read.
UploadOutcome upload_file(const std::string &path, const PutFn &put, const TransferOptions &opt);

struct DownloadOutcome {
    bool                     ok = false;
    std::string              message;
    std::uint64_t            size = 0;
    std::size_t              chunks = 0;
    std::size_t              manifests = 0;
    int                      depth = 0;
    long long                resolve_micros = 0;    // fetching and checking the manifests
    long long                fetch_micros = 0;      // every chunk verified and written
    long long                sync_micros = 0;       // fsync, rename, fsync of the directory
    std::vector<ChunkTiming> timings;
};

// Fetch the file whose root manifest is `root_key` into `dest` (D-055): manifests
// first, then a part file beside `dest`, chunks written at their offsets by
// opt.workers workers, fsync, rename over `dest`, fsync of the directory. On any
// failure the part file is removed and `dest` is untouched.
DownloadOutcome download_file(const std::string &root_key, const std::string &dest, const GetFn &get,
                              const TransferOptions &opt);

// For a SIGINT or SIGTERM handler: remove the part file of the download running
// with opt.for_signals, if there is one. Async-signal-safe: it reads one lock-free
// atomic and calls unlink(2), which POSIX lists as safe in a handler.
void remove_part_file();

// ---- the worker pool (D-056) --------------------------------------------------

// Run job(i) for every i in [0, n) on min(workers, n) threads that take the next i
// from one atomic counter. A job that returns false (or throws) sets `stop`; no
// worker takes a new i after that, and run_pool returns false. A job may read
// `stop` to give up early. Returns true when every job returned true.
bool run_pool(std::size_t n, int workers, std::atomic<bool> &stop, const std::function<bool(std::size_t)> &job);

#endif
