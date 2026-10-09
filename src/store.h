#ifndef STORE_H
#define STORE_H

#include "chord.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The chunk store (decision D-045): one file per chunk, on the local disk.
//
//   <dir>/<first two hex characters of the key>/<40-character key>
//   <dir>/tmp/<key>.<suffix>          a write in progress, or left by a crash
//
// A KEY is SHA-1(contents) as 40 lowercase hexadecimal characters (D-014). It
// is the only part of a path that ever comes from the network, so it is checked
// by valid_key() before any path is built.
//
// Nothing here opens a socket, and nothing here knows about the ring: a key's
// routing identifier (its first 16 characters, D-019) is used only to answer
// "which keys lie in this arc?" for repair.
// ---------------------------------------------------------------------------

// The largest chunk a node will store or send (D-016, D-044): 512 KiB. One
// compile-time constant, CHORDFS_MAX_CHUNK, sets both this and net.h's MAX_BODY;
// only the chunk-size sweep overrides it, in a separate build (D-063):
//   make CHUNK_LIMIT=8388608
#ifndef CHORDFS_MAX_CHUNK
#define CHORDFS_MAX_CHUNK (512 * 1024)
#endif
constexpr std::size_t MAX_CHUNK = CHORDFS_MAX_CHUNK;

// Exactly 40 characters, each 0-9 or a-f. Lowercase only, so one chunk cannot
// have two names; "../", "/", uppercase and every other length are refused.
bool valid_key(const std::string &key);

// SHA-1 of `bytes` as 40 lowercase hexadecimal characters: the key the bytes
// must be stored under, and what a reader compares against the key it asked for.
std::string chunk_key(const std::string &bytes);

// The monotonic clock: it never jumps when the wall clock is set, so an age measured
// with it is real elapsed time (D-091).
using StoreClock = std::chrono::steady_clock;

struct ChunkStore {
    std::string dir;                        // set once by store_open, then only read

    mutable std::mutex mu;                  // guards `keys` and `touched`
    std::set<std::string> keys;             // every key held, sorted

    // When each chunk was last written -- stored, or stored again (D-091). Only
    // chunks written since this process started are here; any other chunk counts
    // as written at `opened`, so a restart makes every chunk young. In memory only:
    // a crash can only make chunks younger, never older.
    std::map<std::string, StoreClock::time_point> touched;
    StoreClock::time_point opened;          // set once by store_open

    // A number unique within this process, for temporary file names. Atomic so
    // two request threads never draw the same one.
    std::atomic<unsigned long> next_tmp{0};

    // Bytes of every chunk held, for the metrics (D-084): counted at open, then
    // changed by every store and removal.
    std::atomic<std::uint64_t> bytes{0};
};

// Create <dir>, <dir>/tmp and the 256 fan-out directories if they do not exist;
// delete every temporary file a crash left behind; load the index by listing
// the fan-out directories. `report` says what was found. Call once, before any
// other thread uses the store. Returns false with `err` set on failure.
bool store_open(ChunkStore &s, const std::string &dir, std::string &report, std::string &err);

enum class PutResult {
    Stored,          // written, flushed and renamed into place
    AlreadyHeld,     // the key was already in the store: nothing to do
    BadKey,          // not 40 lowercase hexadecimal characters
    TooLarge,        // longer than MAX_CHUNK
    HashMismatch,    // the bytes do not hash to the key
    IoError,         // the disk refused; nothing was renamed into place
};
const char *put_result_name(PutResult r);

// Keep `body` under `key`. Checks, in order: the key, the size, the hash. Then
// writes <dir>/tmp/<key>.<suffix>, fsyncs it, renames it over the final name and
// fsyncs the directory (D-045). Safe to call from many threads at once. A chunk
// already held is not written again, but counts as written now (D-091): an upload
// that reuses an old chunk renews it.
PutResult store_put(ChunkStore &s, const std::string &key, const std::string &body);

enum class GetResult { Found, Missing, BadKey, IoError };

// Read the chunk into `out`. It is NOT re-hashed here: the reader checks it
// (D-049), and the scrub checks what sits on disk (D-050).
GetResult store_get(const ChunkStore &s, const std::string &key, std::string &out);

bool store_has(const ChunkStore &s, const std::string &key);

// Every key held, sorted.
std::vector<std::string> store_keys(const ChunkStore &s);

// The keys whose routing identifier lies in the arc (from, to] -- the keys a
// ring member at `to` with predecessor `from` owns. (n, n] is the whole ring.
std::vector<std::string> store_keys_in_arc(const ChunkStore &s, Id from, Id to);

// Remove a chunk from disk and from the index. Used only by the scrub, for a
// file that no longer hashes to its name (D-050).
bool store_remove(ChunkStore &s, const std::string &key);

// ---- Collection (D-090, D-091) -------------------------------------------------

enum class DropResult {
    Dropped,     // removed from the index and the disk
    TooYoung,    // written less than `grace` ago: kept
    NotHeld,     // not in the store
    BadKey,
};

// Remove a chunk only if it has not been written for at least `grace`. The age
// check, the removal from the index and the unlink all happen under `mu`, and a
// duplicate write's "already held: renew" takes `mu` too -- so no write can land
// between the check and the delete. `bytes` is set to the size freed.
DropResult store_drop(ChunkStore &s, const std::string &key, std::chrono::milliseconds grace, std::uint64_t &bytes);

// Forget write times older than `grace`: such a chunk is old either way, so its
// entry can go, and `touched` holds only what was written within one grace period.
void store_forget_old(ChunkStore &s, std::chrono::milliseconds grace);

// Up to `limit` keys after `after` ("" for the start), in order: a page of a listing.
std::vector<std::string> store_keys_after(const ChunkStore &s, const std::string &after, std::size_t limit);

// The path a key is stored at. `key` must already be valid. Exposed for tests,
// which corrupt files on purpose.
std::string store_path(const ChunkStore &s, const std::string &key);

#endif
