#include "chunk.h"
#include "log.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

static_assert(MAX_BODY == MAX_CHUNK, "the framing limit and the store limit must be the same 512 KiB");

namespace {

std::string address(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

long long ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// One body slot (D-052). Constructed only by hold_body() after it has taken the
// slot; its destructor gives the slot back. Not copyable: a copy would give the
// slot back twice.
struct Slot {
    explicit Slot(ChunkNode *n) : node(n) {}
    ~Slot() { node->bodies.fetch_sub(1); }
    Slot(const Slot &) = delete;
    Slot &operator=(const Slot &) = delete;
    ChunkNode *node;
};

// ---- the coordinator's bookkeeping (D-046) ---------------------------------

// What the copies of one write have done so far. Shared by the coordinator and
// every copy's thread through a shared_ptr, so it lives as long as any of them.
struct Tally {
    std::mutex              mu;
    std::condition_variable cv;              // signalled whenever a copy finishes
    int                     confirmed = 0;
    int                     failed = 0;
    std::string             failures;        // "ip:port refused; ..." for the error reply
};

// The copies' threads. Owned by the coordinator and then by its `after` work --
// never by a copy's own thread -- so the joins in the destructor always run on a
// thread that is not one of them. (A thread that joined itself would throw.)
struct Workers {
    std::vector<std::thread> threads;
    ~Workers() {
        for (std::thread &t : threads)
            if (t.joinable()) t.join();
    }
};

ChunkHandled reply(const std::string &line) { return ChunkHandled{line, "", {}, {}}; }

// A whole number 1..3, one digit: the `w` of a PUT.
bool parse_w(const std::string &word, int &w) {
    if (word.size() != 1 || word[0] < '1' || word[0] > '3') return false;
    w = word[0] - '0';
    return true;
}

ChunkHandled coordinate(ChunkNode &n, const std::string &key, int w, std::string &&body) {
    const auto start = std::chrono::steady_clock::now();

    // Copy 1: our own. The store checks the key, the size and the hash, so a body
    // that does not hash to its key is refused here, before it is sent anywhere.
    const PutResult local = store_put(*n.store, key, body);
    if (local == PutResult::BadKey || local == PutResult::TooLarge || local == PutResult::HashMismatch)
        return reply(std::string("ERR ") + put_result_name(local));

    Id k = 0;
    id_from_key(key, k);
    const std::vector<Peer> set = replica_set(*member_for(n.members, k));   // set[0] is us

    auto tally = std::make_shared<Tally>();
    if (local == PutResult::Stored || local == PutResult::AlreadyHeld) tally->confirmed = 1;
    else { tally->failed = 1; tally->failures = "local disk error"; }

    // The body, moved (not copied) into one buffer that every copy's thread reads.
    // const: nothing may change it while two threads are sending it.
    auto shared = std::make_shared<const std::string>(std::move(body));
    const std::string store_line = "STORE " + key + " " + std::to_string(shared->size());

    // Copies 2 and 3, in parallel: one thread each. A thread receives copies of
    // everything it uses -- the shared pointers keep the buffer and the tally alive
    // for as long as it runs, and `send` is copied -- never a reference into this
    // function's stack frame, which is gone before the last copy finishes.
    // The request id is thread_local (D-087): a new thread starts without it, so
    // each copy's thread is handed it and opens its own scope.
    const std::string rid = current_request_id();
    auto workers = std::make_shared<Workers>();
    for (std::size_t i = 1; i < set.size(); ++i) {
        const Peer to = set[i];
        auto job = [tally, shared, to, store_line, rid, send = n.send]() {
            RequestScope scope(rid);
            Frame answer;
            const Delivery d = send(to, store_line, *shared, answer);
            const bool good = d == Delivery::Ok && answer.line == "OK";
            {
                std::lock_guard<std::mutex> lock(tally->mu);
                if (good) ++tally->confirmed;
                else {
                    ++tally->failed;
                    tally->failures += (tally->failures.empty() ? "" : "; ") + address(to) + " " +
                                       (d == Delivery::Ok ? answer.line : delivery_name(d));
                }
            }
            tally->cv.notify_all();            // after unlocking, so the waiter can take the lock
        };
        try {
            workers->threads.emplace_back(job);
        } catch (const std::system_error &) {          // could not start a thread: a failed copy
            std::lock_guard<std::mutex> lock(tally->mu);
            ++tally->failed;
            tally->failures += (tally->failures.empty() ? "" : "; ") + address(to) + " no thread";
        }
    }

    // Wait until `w` copies are confirmed, or until so many have failed that `w`
    // can no longer be reached. The predicate is re-checked after every wake-up,
    // so a spurious wake-up, or a notify that arrived before the wait, is harmless.
    const int total = static_cast<int>(set.size());
    int confirmed;
    std::string failures;
    {
        std::unique_lock<std::mutex> lock(tally->mu);
        tally->cv.wait(lock, [&] {
            const int pending = total - tally->confirmed - tally->failed;
            return tally->confirmed >= w || tally->confirmed + pending < w;
        });
        confirmed = tally->confirmed;
        failures  = tally->failures;
    }
    const long long acked_ms = ms_since(start);
    const auto acked = std::chrono::steady_clock::now();

    ChunkHandled h;
    h.reply = confirmed >= w ? "OK " + std::to_string(confirmed)
                             : "ERR only " + std::to_string(confirmed) + " of " + std::to_string(w) +
                               " copies stored: " + failures;
    if (h.reply.size() > MAX_LINE) h.reply.resize(MAX_LINE);   // a reply is one line, bounded

    // After the reply has gone: wait for the remaining copies, then log how long
    // after the answer the last one landed -- the window with fewer than RF copies.
    // `node` is used after this function returns: the same lifetime rule as a
    // Transport -- `after` runs while the ChunkNode it came from is alive.
    ChunkNode *node = &n;
    h.after = [workers, tally, node, key, total, acked_ms, acked, start]() {
        for (std::thread &t : workers->threads) t.join();
        node->replication_lag.observe(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - acked).count());
        int c, f;
        {
            std::lock_guard<std::mutex> lock(tally->mu);
            c = tally->confirmed;
            f = tally->failed;
        }
        if (c < RF) node->copies_short.add();
        if (!node->log) return;
        node->log("put " + key + ": " + std::to_string(c) + " of " + std::to_string(total) + " copies, answered after " +
            std::to_string(acked_ms) + " ms, last copy after " + std::to_string(ms_since(start)) + " ms" +
            (f ? ", " + std::to_string(f) + " failed" : ""));
    };
    return h;
}

}  // namespace

std::shared_ptr<void> hold_body(ChunkNode &n) {
    // fetch_add returns the value BEFORE the increment, in one indivisible step,
    // so two threads can never both see 31 and both take the 32nd slot. A check
    // followed by a separate increment would leave exactly that gap.
    if (n.bodies.fetch_add(1) >= MAX_BODIES) {
        n.bodies.fetch_sub(1);
        return nullptr;
    }
    return std::make_shared<Slot>(&n);
}

bool is_chunk_request(const std::string &line) {
    const std::string verb = line.substr(0, line.find(' '));
    return verb == "PUT" || verb == "STORE" || verb == "GET" || verb == "HAVE" || verb == "DIGEST" || verb == "KEYS" ||
           verb == "DROP";
}

std::string keys_digest(const std::vector<std::string> &keys) {
    std::string all;
    all.reserve(keys.size() * 41);
    for (const std::string &k : keys) all += k + "\n";
    return std::to_string(keys.size()) + " " + chunk_key(all);
}

bool same_secret(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    // Every byte is compared whatever the earlier ones were: OR together the
    // differences, then look once at the end.
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

std::vector<Peer> distinct_processes(const std::vector<Peer> &from_owner, int rf) {
    std::vector<Peer> out;
    for (const Peer &p : from_owner) {
        if (static_cast<int>(out.size()) >= rf) break;
        bool taken = false;
        for (const Peer &q : out) taken = taken || same_process(p, q);   // by address (D-048)
        if (!taken) out.push_back(p);
    }
    return out;
}

std::vector<Peer> replica_set(const NodeState &member, int rf) {
    std::vector<Peer> list{member.self};
    {
        std::lock_guard<std::mutex> lock(member.mu);
        list.insert(list.end(), member.succs.begin(), member.succs.end());
    }
    return distinct_processes(list, rf);
}

NodeState *member_for(const std::vector<NodeState *> &members, Id k) {
    // The member reached first going clockwise from k: smallest (id - k), which
    // unsigned arithmetic computes modulo 2^64. A member AT k has distance 0.
    NodeState *best = members.front();
    for (NodeState *m : members)
        if (m->self.id - k < best->self.id - k) best = m;
    return best;
}

ChunkHandled handle_chunk_request(ChunkNode &n, Frame request) {
    const std::vector<std::string> w = split_single_spaces(request.line);
    if (w.empty() || w[0].empty()) return reply("ERR empty request");
    if (!n.store) return reply("ERR this node keeps no chunks");
    const std::string &verb = w[0];

    if (verb == "PUT" && w.size() == 4) {
        int copies = 0;
        if (!valid_key(w[1])) return reply("ERR key must be 40 lowercase hexadecimal characters");
        if (!parse_w(w[2], copies)) return reply("ERR w must be 1, 2 or 3");
        return coordinate(n, w[1], copies, std::move(request.body));
    }

    if (verb == "STORE" && w.size() == 3) {
        const PutResult r = store_put(*n.store, w[1], request.body);
        if (r == PutResult::Stored || r == PutResult::AlreadyHeld) return reply("OK");
        return reply(std::string("ERR ") + put_result_name(r));
    }

    if (verb == "GET" && w.size() == 2) {
        ChunkHandled h;
        h.hold = hold_body(n);                         // a read holds a body in memory too
        if (!h.hold) return reply("ERR busy");
        switch (store_get(*n.store, w[1], h.body)) {
            case GetResult::Found:   h.reply = "CHUNK " + std::to_string(h.body.size()); return h;
            case GetResult::Missing: return reply("MISSING");
            case GetResult::BadKey:  return reply("ERR key must be 40 lowercase hexadecimal characters");
            case GetResult::IoError: return reply("ERR disk error");
        }
        return reply("ERR disk error");
    }

    if (verb == "HAVE" && w.size() >= 2 && w.size() <= 1 + static_cast<std::size_t>(MAX_HAVE_KEYS)) {
        std::string bits;
        for (std::size_t i = 1; i < w.size(); ++i) {
            if (!valid_key(w[i])) return reply("ERR key must be 40 lowercase hexadecimal characters");
            bits += store_has(*n.store, w[i]) ? '1' : '0';
        }
        return reply("HAVE " + bits);
    }

    // What this process holds in one arc, as one line (D-093): repair's owner compares
    // it with its own before listing anything.
    if (verb == "DIGEST" && w.size() == 3) {
        Id from = 0, to = 0;
        if (!id_from_hex(w[1], from) || !id_from_hex(w[2], to)) return reply("ERR malformed DIGEST");
        return reply("DIGEST " + keys_digest(store_keys_in_arc(*n.store, from, to)));
    }

    // A page of what this process holds, and how long its grace is, for the
    // collector (D-090): it checks the grace against its own settings (D-091).
    if (verb == "KEYS" && w.size() == 2) {
        if (w[1] != "-" && !valid_key(w[1])) return reply("ERR key must be 40 lowercase hexadecimal characters");
        // A listing from the start begins a collection: a good moment to forget write
        // times that no longer matter, so they do not pile up between collections.
        if (w[1] == "-") store_forget_old(*n.store, n.gc_grace);
        const std::vector<std::string> page = store_keys_after(*n.store, w[1] == "-" ? "" : w[1], MAX_HAVE_KEYS);
        std::string line = "KEYS " + std::to_string(page.size()) + " " + std::to_string(n.gc_grace.count() / 1000);
        for (const std::string &k : page) line += " " + k;
        return reply(line);
    }

    // Drop what the collector found unreferenced -- each only if old enough (D-091),
    // and only for a caller holding the key (D-092).
    if (verb == "DROP" && w.size() >= 3 && w.size() <= 2 + static_cast<std::size_t>(MAX_HAVE_KEYS)) {
        if (n.gc_key.empty()) return reply("ERR deletion is not enabled on this node");
        if (!same_secret(w[1], n.gc_key)) return reply("ERR wrong collection key");
        for (std::size_t i = 2; i < w.size(); ++i)
            if (!valid_key(w[i])) return reply("ERR key must be 40 lowercase hexadecimal characters");
        std::string bits;
        std::uint64_t freed = 0;
        for (std::size_t i = 2; i < w.size(); ++i) {
            std::uint64_t bytes = 0;
            const DropResult r = store_drop(*n.store, w[i], n.gc_grace, bytes);
            if (r == DropResult::Dropped) { n.dropped_chunks.add(); n.dropped_bytes.add(bytes); freed += bytes; }
            if (r == DropResult::TooYoung) n.kept_young.add();
            bits += r == DropResult::Dropped ? '1' : '0';
        }
        return reply("DROPPED " + bits + " " + std::to_string(freed));
    }

    return reply("ERR malformed chunk request");
}

// ---------------------------------------------------------------------------
// The client side
// ---------------------------------------------------------------------------

namespace {

// After the owner failed: tell the member that named it, then ask that member for
// its successor list and return the distinct processes after the owner's -- the
// remaining replica set, from a node that has just answered (D-049).
std::vector<Peer> fallbacks(const LookupResult &r, bool owner_unreachable, const Transport &route) {
    const Peer &namer = r.path.back();               // the member that answered OWNER
    std::string ignored;
    if (owner_unreachable)
        route(namer, "SUSPECT " + r.owner.ip + " " + std::to_string(r.owner.port), ignored);

    std::string line, why;
    Neighbours nb;
    if (route(namer, "NEIGHBOURS", line) != Delivery::Ok || !parse_neighbours(line, nb, why)) return {};
    std::vector<Peer> from_owner;
    bool seen_owner = false;
    for (const Peer &p : nb.succs) {
        seen_owner = seen_owner || same_node(p, r.owner);
        if (seen_owner) from_owner.push_back(p);
    }
    if (!seen_owner) from_owner = nb.succs;          // the namer's view has moved on: use it as it is
    std::vector<Peer> out;
    for (const Peer &p : distinct_processes(from_owner, RF))
        if (!same_process(p, r.owner)) out.push_back(p);
    return out;
}

bool unreachable(Delivery d) { return d == Delivery::Refused || d == Delivery::Timeout || d == Delivery::Closed; }

}  // namespace

PutOutcome put_chunk(const Peer &start, const std::string &body, int w, const Transport &route,
                     const FrameTransport &send, int retries) {
    PutOutcome out;
    const std::string key = chunk_key(body);
    out.key = key;
    Id k = 0;
    id_from_key(key, k);

    const LookupResult r = lookup_with_recovery(start, k, route, retries);
    if (!r.ok()) {
        out.message = std::string("lookup failed (") + lookup_error_name(r.error) + "): " + r.message;
        return out;
    }

    const std::string line = "PUT " + key + " " + std::to_string(w) + " " + std::to_string(body.size());
    std::vector<Peer> candidates{r.owner};
    bool asked_for_more = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Peer &to = candidates[i];
        Frame answer;
        const Delivery d = send(to, line, body, answer);
        ++out.attempts;
        out.coordinator = to;
        if (d == Delivery::Ok && answer.line.compare(0, 3, "OK ") == 0) {
            out.ok = true;
            out.copies = std::atoi(answer.line.c_str() + 3);
            out.message.clear();
            return out;
        }
        out.message += (out.message.empty() ? "" : "; ") + address(to) + ": " +
                       (d == Delivery::Ok ? answer.line : delivery_name(d));
        // A coordinator that answered, and could not reach w, has given the answer:
        // the copies it could place exist, and another coordinator would face the
        // same replicas. Fall through only past one that could not be reached or
        // had no memory free -- to the rest of the replica set, once.
        if (d == Delivery::Ok && answer.line != "ERR busy") return out;
        if (!asked_for_more) {
            asked_for_more = true;
            for (const Peer &p : fallbacks(r, i == 0 && unreachable(d), route)) candidates.push_back(p);
        }
    }
    return out;
}

GetOutcome get_chunk(const Peer &start, const std::string &key, const Transport &route,
                     const FrameTransport &send, int retries) {
    GetOutcome out;
    Id k = 0;
    if (!valid_key(key) || !id_from_key(key, k)) {
        out.message = "key must be 40 lowercase hexadecimal characters";
        return out;
    }
    const LookupResult r = lookup_with_recovery(start, k, route, retries);
    if (!r.ok()) {
        out.message = std::string("lookup failed (") + lookup_error_name(r.error) + "): " + r.message;
        return out;
    }

    std::vector<Peer> candidates{r.owner};
    bool asked_for_more = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Peer &from = candidates[i];
        Frame answer;
        const Delivery d = send(from, "GET " + key, "", answer);
        std::string what;
        if (d != Delivery::Ok)                          what = delivery_name(d);
        else if (answer.line.compare(0, 6, "CHUNK ") != 0) what = answer.line;
        else if (chunk_key(answer.body) != key)         what = "bytes do not hash to the key";   // R = 1: check it ourselves
        else {
            out.ok = true;
            out.body = std::move(answer.body);
            out.served_by = from;
            out.tried.push_back(address(from) + ": ok");
            return out;
        }
        out.tried.push_back(address(from) + ": " + what);
        if (!asked_for_more) {
            asked_for_more = true;
            for (const Peer &p : fallbacks(r, i == 0 && unreachable(d), route)) candidates.push_back(p);
        }
    }
    for (const std::string &t : out.tried) out.message += (out.message.empty() ? "" : "; ") + t;
    return out;
}
