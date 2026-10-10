#ifndef CHUNK_H
#define CHUNK_H

#include "chord.h"
#include "lookup.h"
#include "metrics.h"
#include "net.h"
#include "stabilize.h"
#include "store.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The chunk plane (Phase 4): storing, replicating and reading chunks.
//
//   PUT <key> <w> <length>  + body   coordinate a replicated write   (D-046)
//   STORE <key> <length>    + body   keep one copy, forward nothing  (D-046)
//   GET <key>                        -> CHUNK <length> + body | MISSING  (D-049)
//   HAVE <key> [<key> ...]           -> HAVE <0/1 per key>           (D-050)
//   DIGEST <from-id> <to-id>         -> DIGEST <n> <sha1>            (D-093: the keys held in (from, to])
//   KEYS <after|->                   -> KEYS <n> <grace-s> <key> ... (D-090: a page of what is held)
//   DROP <gc-key> <key> [<key> ...]  -> DROPPED <0/1 per key> <bytes> (D-090, D-091, D-092)
//
// Chunk requests are for a PROCESS, not a ring member: the store belongs to the
// process (D-047). Like stabilize.cpp, nothing here opens a socket. Calls to
// other processes go through a FrameTransport passed in, so the tests run
// several processes' stores and members in one test binary.
// ---------------------------------------------------------------------------

constexpr int RF               = 3;      // copies of every chunk, on distinct processes (D-015, D-048)
constexpr int DEFAULT_W        = 2;      // copies confirmed before a write is acknowledged (D-015)
constexpr int MAX_BODIES       = 32;     // chunk bodies held in memory at once, per process (D-052)
constexpr int MAX_HAVE_KEYS    = 20;     // keys in one HAVE request; 20 x 41 characters fits MAX_LINE
                                         // (also per KEYS page and per DROP)
constexpr long DEFAULT_GC_GRACE_S = 7200; // a chunk is dropped only if not written for this long (D-091)

// One message of the chunk plane: a header line and, for PUT, STORE and CHUNK,
// a body (net.h's body_length decides which).
struct Frame {
    std::string line;
    std::string body;
};

// Send a header line and its body to the PROCESS at `to`'s address and put its
// answer in `reply`. The body is passed by reference, not inside a Frame, so a
// coordinator can send one buffer to two replicas without copying it (D-052).
// The real one is exchange_frame with the node's deadline. The same lifetime rule
// as Transport (lookup.h): valid only while the call it is passed to is running,
// and never kept after it -- a coordinator's copies finish before its request's
// work is done.
using FrameTransport = std::function<Delivery(const Peer &to, const std::string &line,
                                              const std::string &body, Frame &reply)>;

// Everything one process needs to answer chunk requests.
struct ChunkNode {
    ChunkStore *store = nullptr;                   // null: this process keeps no chunks
    std::vector<NodeState *> members;              // this process's ring members (D-047)
    FrameTransport send;                           // to another process
    std::function<void(const std::string &)> log;  // called without any lock held

    // Bodies held in memory right now (D-052). Changed only by hold_body() and
    // the Hold it returns.
    std::atomic<int> bodies{0};

    // What the coordinator measured, for the process's metrics (D-084): the time
    // from a write's acknowledgement to its last copy -- the window with fewer than
    // RF copies -- and the writes whose copies ended short of RF.
    Histogram replication_lag{latency_buckets()};
    Counter   copies_short;

    // Collection (D-090 to D-092). Set once before any request thread starts.
    // An empty gc_key refuses every DROP: deletion is off unless a key is given.
    std::string               gc_key;
    std::chrono::milliseconds gc_grace{DEFAULT_GC_GRACE_S * 1000};
    Counter dropped_chunks, dropped_bytes, kept_young;     // what DROP did
};

// A claim on one of MAX_BODIES body slots. While the returned pointer (or a copy
// of it) exists, the slot is held; destroying the last copy gives it back. Null
// if every slot is taken: the request must then be refused before its body is read.
std::shared_ptr<void> hold_body(ChunkNode &n);

// The answer to one chunk request: a reply line, a body if the line declares one,
// work to do after the reply has been sent (a coordinator waits for its remaining
// copies there), and the body slot to keep until all of that is done.
struct ChunkHandled {
    std::string           reply;
    std::string           body;
    std::function<void()> after;
    std::shared_ptr<void> hold;
};

// Is this line a chunk request (PUT, STORE, GET, HAVE, DIGEST, KEYS or DROP)?
bool is_chunk_request(const std::string &line);

// Answer one chunk request whose body, if it has one, has already been read.
// Takes the request by value so a PUT's body can be moved, not copied, into the
// buffer its copies share. Never throws on anything a peer sends (I5).
ChunkHandled handle_chunk_request(ChunkNode &n, Frame request);

// The replica set of keys owned by `member` (D-048): `member` itself, then each
// entry of its successor list whose process is not already in the set, up to RF
// processes. Fewer if the list holds fewer distinct processes. Never two members
// of one process.
std::vector<Peer> replica_set(const NodeState &member, int rf = RF);

// The same rule applied to a list that starts with the owner -- for example the
// successor list of the member just before a key, which a reader gets from
// NEIGHBOURS (D-049).
std::vector<Peer> distinct_processes(const std::vector<Peer> &from_owner, int rf = RF);

// A set of keys in one line (D-093): "<count> <SHA-1 of the keys in order, each
// followed by a newline>". Two sets that give the same digest are the same set.
// `keys` must be sorted, as store_keys_in_arc returns them.
std::string keys_digest(const std::vector<std::string> &keys);

// Equal, in time that does not depend on where the first difference is (D-092): a
// secret cannot be guessed one character at a time by timing the answers. Only the
// length is compared in the ordinary way; it is not secret.
bool same_secret(const std::string &a, const std::string &b);

// Of a process's members, the one that owns `k` if this process owns it at all:
// the first member at or after k, clockwise. `members` must not be empty.
NodeState *member_for(const std::vector<NodeState *> &members, Id k);

// ---- The client side ------------------------------------------------------

struct PutOutcome {
    bool        ok = false;
    int         copies = 0;          // copies the coordinator confirmed
    std::string message;             // on failure: what went wrong, naming the node
    Peer        coordinator;         // who answered (or failed last)
    int         attempts = 0;        // coordinators tried
    std::string key;                 // the chunk's key, SHA-1 of the body (set even on failure)
};

// Write `body` under its key with `w` confirmed copies (D-046). Looks up the
// key's owner from `start` and sends it PUT. If the owner cannot be reached, the
// member that answered the lookup is sent SUSPECT, and asked for its successor
// list, and the next distinct processes are tried as coordinators in turn (D-049's
// fall-through, applied to writes). `route` carries lookups, `send` chunk frames.
PutOutcome put_chunk(const Peer &start, const std::string &body, int w, const Transport &route,
                     const FrameTransport &send, int retries = 2);

struct GetOutcome {
    bool                     ok = false;
    std::string              body;       // verified: chunk_key(body) == key
    std::string              message;    // on failure: every replica tried and what it did
    Peer                     served_by;
    std::vector<std::string> tried;      // "ip:port: <what happened>", in order
};

// Read the chunk with this key (D-049): look up its owner, GET from it, hash the
// bytes; on any failure ask the member that answered the lookup for its successor
// list and try the other distinct processes in turn.
GetOutcome get_chunk(const Peer &start, const std::string &key, const Transport &route,
                     const FrameTransport &send, int retries = 2);

#endif
