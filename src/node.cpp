// node -- one process in a Chord ring: one or more ring members, and optionally a chunk store.
//
//   ./node <ip> <port> <membership-file>        fixed ring, never changes (Phase 2, D-020)
//   ./node <ip> <port> --create     [options]   a new ring of this process's members
//   ./node <ip> <port> --join <ip:port> [opts]  join through a node already in a ring
//   ./node <ip> <port> --wired <file> [options] start from the fixed-ring state, then maintain it
//
// Options (D-032, D-039, D-047, D-045, D-050):
//   --period <ms>    time between maintenance rounds              (default DEFAULT_PERIOD_MS)
//   --timeout <ms>   deadline on every call to another node       (default DEFAULT_TIMEOUT_MS)
//   --succ <r>       successor-list length, 1..16                 (default 8)
//   --vnodes <v>     ring members in this process, 1..64          (default DEFAULT_VNODES = 8)
//   --store <dir>    keep chunks in <dir>; without it, chunk requests are refused
//   --scan <rounds>  rounds between full repair scans, 0 = never  (default DEFAULT_SCAN_ROUNDS)
//   --read-deadline <ms>  a request line, or a body, must arrive whole within this (D-061)
//                         (default DEFAULT_READ_DEADLINE_MS)
//   --metrics <port>      serve GET /metrics on this port (D-083); none without it
//   --gc-key <file>       accept DROP from a collector holding this key (D-092); without it, never
//   --gc-grace <s>        drop only chunks not written for this long (D-091; default DEFAULT_GC_GRACE_S)
//   --log-level <level>   debug, info (default), warn or error (D-086)
//
// Ring requests are answered by handle_request() in stabilize.cpp and chunk
// requests by handle_chunk_request() in chunk.cpp, both shared with the
// in-process tests, so what the tests prove is what the node does. This file is
// only sockets, threads and the command line.

#include "chord.h"
#include "chunk.h"
#include "http.h"
#include "log.h"
#include "lookup.h"
#include "metrics.h"
#include "names.h"      // valid_secret: the collection key is written as owner secrets are
#include "net.h"
#include "repair.h"
#include "stabilize.h"
#include "store.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

// The defaults chosen from the reconvergence benchmark by the rules of D-039 and
// D-042, at commit b171554 (BENCHMARKS.md §4): the longest period at which 9 of 10
// runs repaired the ring within 1 s, and the smallest deadline at least ten times the
// 99.9th-percentile exchange and above the slowest one observed.
constexpr int DEFAULT_PERIOD_MS  = 1000;
constexpr int DEFAULT_TIMEOUT_MS = 200;

// Members per process, chosen by D-051's rule at commit 7d99fc0 (BENCHMARKS.md §6):
// the smallest power of two for which the copies stored per process vary by at most
// 0.25 of the mean (coefficient of variation), at 16 and at 64 processes.
constexpr int DEFAULT_VNODES     = 8;
constexpr int MAX_VNODES         = 64;

// Rounds between full repair scans (D-050): once a minute at the default period.
constexpr int DEFAULT_SCAN_ROUNDS = 60;

// How long a peer may take to send a whole request line, or a whole body (D-061):
// about a 512 KiB body at 0.4 Mbit/s. A legitimate sender writes at once.
constexpr int DEFAULT_READ_DEADLINE_MS = 10000;

// Set once in main() before any thread starts, and only read afterwards. That
// ordering -- all writes before the first reader exists -- is what makes them
// safe to read without a lock.
int g_timeout_ms = DEFAULT_TIMEOUT_MS;
int g_read_deadline_ms = DEFAULT_READ_DEADLINE_MS;
std::vector<std::unique_ptr<NodeState>> g_members;     // this process's ring members (D-047)
ChunkStore g_store;
ChunkNode  g_chunks;

// ---- Metrics (D-083, D-084) ----------------------------------------------------
// Every series is registered by register_metrics() before the metrics thread or any
// connection thread starts, and these maps and pointers are only read after that:
// the same "all writes before the first reader" rule as the settings above. The
// values themselves are atomics, updated from any thread.
Registry g_metrics;
std::atomic<int> g_connections_open{0};
std::atomic<int> g_repair_pending{0};
Counter *g_connections_total = nullptr;
Counter *g_bytes_in = nullptr;
Counter *g_bytes_out = nullptr;
Histogram *g_lookup_hops = nullptr;
Histogram *g_lookup_seconds = nullptr;
std::map<std::string, Counter *> g_errors;            // by type: deadline, malformed, busy, ...
std::map<std::string, Counter *> g_chunk_requests;    // by "verb/result"
std::map<std::string, Histogram *> g_chunk_seconds;   // by verb
std::map<std::string, Counter *> g_lookup_failures;   // by lookup_error_name
std::map<std::string, Counter *> g_repair;            // by what a repair round did

const char *const CHUNK_VERBS[] = {"put", "store", "get", "have", "digest", "keys", "drop"};
const char *const CHUNK_RESULTS[] = {"ok", "missing", "busy", "error"};
const char *const ERROR_TYPES[] = {"deadline", "line_too_long", "malformed", "too_large", "busy", "closed_mid_body"};

void count(std::map<std::string, Counter *> &family, const std::string &which, std::uint64_t n = 1) {
    const auto it = family.find(which);
    if (it != family.end()) it->second->add(n);
}

// Every lookup this process makes -- finger repair, joins, hand-offs -- ends here.
void observe_lookup(const LookupResult &r, double seconds) {
    if (!r.ok()) { count(g_lookup_failures, lookup_error_name(r.error)); return; }
    g_lookup_hops->observe(r.hops());
    g_lookup_seconds->observe(seconds);
}

// "PUT ab12... 2 524288" -> "put"; "HAVE k1 k2" -> "have".
std::string verb_of(const std::string &line) {
    std::string v = line.substr(0, line.find(' '));
    for (char &c : v) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return v;
}

// What a chunk reply means, as one of CHUNK_RESULTS.
const char *result_of(const std::string &reply) {
    if (reply == "OK" || reply.compare(0, 3, "OK ") == 0 || reply.compare(0, 6, "CHUNK ") == 0 ||
        reply.compare(0, 5, "HAVE ") == 0 || reply.compare(0, 5, "KEYS ") == 0 || reply.compare(0, 8, "DROPPED ") == 0 ||
        reply.compare(0, 7, "DIGEST ") == 0)
        return "ok";
    if (reply == "MISSING") return "missing";
    if (reply == "ERR busy") return "busy";
    return "error";
}

// Count, time and log one chunk request once its reply has been sent (D-084, D-086).
void record_chunk(const std::string &line, const std::string &reply, std::uint64_t bytes,
                  std::chrono::steady_clock::time_point t0) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::string verb = verb_of(line);
    const char *result = result_of(reply);
    count(g_chunk_requests, verb + "/" + result);
    const auto h = g_chunk_seconds.find(verb);
    if (h != g_chunk_seconds.end()) h->second->observe(seconds);
    // HAVE and KEYS are bookkeeping, many per round: debug. The rest is one line per
    // chunk, or per DROP batch. A DROP's first word after the verb is the collection
    // key, so it is never logged (D-092): its "key" field is left empty.
    const Level level = verb == "have" || verb == "keys" || verb == "digest" ? Level::Debug : Level::Info;
    if (!log_enabled(level)) return;
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.3f", seconds * 1000);
    const std::size_t a = line.find(' ');
    const std::string key =
        a == std::string::npos || verb == "drop" ? "" : line.substr(a + 1, line.find(' ', a + 1) - a - 1);
    log_event(level, ("chunk." + verb).c_str(),
              {{"key", key}, {"result", result}, {"reply", reply.substr(0, 120)}, {"bytes", std::to_string(bytes)},
               {"ms", ms}});
}

// Every call this node makes to a ring member: one fresh connection, with the
// deadline set before connect (D-025), addressed to that member (D-047). A plain
// function, so it lives forever and can be referred to from any thread.
Delivery node_transport(const Peer &to, const std::string &request, std::string &reply) {
    // The request id of whatever this thread is doing on someone's behalf goes first (D-087).
    return exchange_line(to.ip, to.port, with_request_id(addressed(to, request)), reply, g_timeout_ms);
}
const Transport g_transport = node_transport;

// Every chunk call this node makes to another process (D-044): a header line, a
// body if the line declares one, and the same deadline on each system call.
Delivery frame_transport(const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
    return exchange_frame(to.ip, to.port, with_request_id(line), body, reply.line, reply.body, g_timeout_ms);
}

// The member a ring request is for, or null. No prefix means member 0 (D-047).
NodeState *member_by_id(Id id) {
    for (const auto &m : g_members)
        if (m->self.id == id) return m.get();
    return nullptr;
}

// One chunk request whose header has been read. Returns false if the connection
// must be closed: after a malformed or oversized length the stream position is
// unknown, because the body's bytes are still arriving and nothing says where
// they end.
bool serve_chunk(int fd, const std::string &line) {
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t length = 0;
    const Body b = body_length(line, length);
    if (b == Body::Malformed) { count(g_errors, "malformed"); send_line(fd, "ERR malformed body length"); return false; }
    if (b == Body::TooLarge)  {
        count(g_errors, "too_large");
        send_line(fd, "ERR chunk larger than " + std::to_string(MAX_BODY) + " bytes");
        return false;
    }

    // Take a body slot BEFORE reading the body, so a peer cannot make this node
    // allocate more than MAX_BODIES chunks at once (D-052).
    std::shared_ptr<void> hold;
    std::string body;
    const Deadline body_due = deadline_in(g_read_deadline_ms);       // the whole body by then (D-061)
    if (b == Body::Present) {
        hold = hold_body(g_chunks);
        if (!hold) {
            // Refuse it without holding it: read the body through a small buffer and
            // drop it, so the answer is not lost. Closing with the body unread would
            // make the kernel reset the connection, which can destroy "ERR busy"
            // before the sender reads it.
            count(g_errors, "busy");
            const bool ok = recv_discard_by(fd, length, body_due) && send_line(fd, "ERR busy");
            record_chunk(line, "ERR busy", 0, t0);
            return ok;
        }
        if (!recv_exact_by(fd, length, body, body_due)) {        // closed, or not whole in time
            count(g_errors, errno == EAGAIN ? "deadline" : "closed_mid_body");
            return false;
        }
        g_bytes_in->add(length);
    }

    ChunkHandled h = handle_chunk_request(g_chunks, Frame{line, std::move(body)});
    const bool sent = send_line(fd, h.reply) && send_all(fd, h.body.data(), h.body.size());
    if (sent) g_bytes_out->add(h.body.size());
    record_chunk(line, h.reply, b == Body::Present ? length : h.body.size(), t0);
    if (h.after) h.after();                    // a coordinator's remaining copies
    return sent;
}                                              // `hold` and h.hold released here

// One connection: answer requests until the peer closes, a send fails, or a line
// is too long. Runs on its own detached thread. Work a handler deferred until
// after its reply (Handled::after) runs once the reply has been sent.
void serve(int fd) {
    g_connections_open.fetch_add(1);
    g_connections_total->add();
    std::string raw, rid, line;
    while (true) {
        // Each request line must arrive whole within the deadline of starting to wait
        // for it (D-061): a peer that sends nothing, or a byte at a time, is cut off.
        // errno says why a read ended: 0 the peer closed (the normal end), EAGAIN the
        // deadline passed, EMSGSIZE the line was too long.
        if (!recv_line_by(fd, raw, deadline_in(g_read_deadline_ms))) {
            if (errno == EAGAIN) count(g_errors, "deadline");
            else if (errno == EMSGSIZE) count(g_errors, "line_too_long");
            break;
        }
        // The request id comes off first (D-087). While `scope` lives, this thread's
        // log lines and its calls on this request's behalf carry it.
        const RidPrefix rp = split_request_id(raw, rid, line);
        if (rp == RidPrefix::Malformed) {
            count(g_errors, "malformed");
            send_line(fd, "ERR malformed request id");
            break;                             // the line may have announced a body: the stream is lost
        }
        if (rp == RidPrefix::None) rid.clear();
        RequestScope scope(rid);

        // Chunk requests, and any line that announces a body: the body must be read
        // (or the connection closed) before the next line, or its bytes would be
        // taken for requests.
        std::size_t announced = 0;
        if (is_chunk_request(line) || body_length(line, announced) != Body::None) {
            if (!serve_chunk(fd, line)) break;
            continue;
        }
        Id id = 0;
        std::string request;
        NodeState *member = nullptr;
        const Target t = split_target(line, id, request);
        if (t == Target::None)        member = g_members.front().get();
        else if (t == Target::Member) member = member_by_id(id);
        Handled h;
        if (t == Target::Malformed) h.reply = "ERR malformed TO prefix";
        else if (!member)           h.reply = "ERR no ring member " + id_to_hex(id) + " here";
        else                        h = handle_request(*member, request, g_transport);
        if (t == Target::Malformed) count(g_errors, "malformed");
        const bool sent = send_line(fd, h.reply);
        // One line per routing request -- every hop of every lookup -- so only at debug.
        if (log_enabled(Level::Debug))
            log_event(Level::Debug, "ring.request",
                      {{"member", member ? id_to_hex(member->self.id) : "-"},
                       {"verb", request.substr(0, request.find(' '))}, {"reply", h.reply.substr(0, 120)}});
        if (h.after) h.after();
        if (!sent) break;
    }
    close(fd);
    g_connections_open.fetch_sub(1);
}

// The maintenance thread: a round for every member, then a fixed delay (D-039).
// The first round waits a random fraction of a period, so nodes started together
// do not run their rounds in step.
void maintain(int period_ms) {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int> offset(0, period_ms - 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(offset(rng)));
    while (true) {
        for (const auto &m : g_members) maintenance_round(*m, g_transport);
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    }
}

// The repair thread (D-050): one repair round per period, on its own thread so
// moving chunks never delays the rounds that keep the ring correct.
void repair(int period_ms, int scan_rounds) {
    RepairState state;
    std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    while (true) {
        {
            // Each round is its own "request" (D-087): the HAVEs and STOREs it sends,
            // and the lines the receivers log, carry one id per round.
            RequestScope scope(new_request_id());
            const RepairReport r = repair_round(g_chunks, state, g_transport, scan_rounds);
            count(g_repair, "rounds");
            count(g_repair, "chunks_sent", static_cast<std::uint64_t>(r.chunks_sent));
            count(g_repair, "failures", static_cast<std::uint64_t>(r.send_failures));
            count(g_repair, "handed_off", static_cast<std::uint64_t>(r.handed_off));
            count(g_repair, "scrubbed", static_cast<std::uint64_t>(r.scrubbed));
            if (r.full_scan) count(g_repair, "full_scans");
            count(g_repair, "digests_sent", static_cast<std::uint64_t>(r.digests_sent));
            count(g_repair, "digests_matched", static_cast<std::uint64_t>(r.digests_matched));
            count(g_repair, "have_sent", static_cast<std::uint64_t>(r.have_sent));
        }
        // The repair queue: arcs this process owns whose last check failed or that
        // changed since, so they are checked again next round.
        int pending = 0;
        for (const auto &m : g_members) {
            bool known;
            {
                std::lock_guard<std::mutex> lock(m->mu);
                known = m->has_pred && !m->orphaned;
            }
            if (known && state.checked.count(m->self.id) == 0) ++pending;
        }
        g_repair_pending = pending;
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    }
}

// Parse "ip:port" through the membership parser, so it is validated and
// canonicalised, and its identifier computed, exactly as every other address is.
bool parse_address(const std::string &text, Peer &out) {
    std::vector<Peer> one;
    std::string err;
    if (text.find('\n') != std::string::npos || !parse_members(text, one, err) || one.size() != 1)
        return false;
    out = one[0];
    return true;
}

// A whole number in [lo, hi]; digits only, so "+5", " 5" and "5ms" are refused.
bool parse_int(const std::string &s, int lo, int hi, int &out) {
    if (s.empty() || s.size() > 7) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

// Read and parse a membership file. The list is returned to the caller, which
// lets it die by scope once the state is built (D-020).
bool read_members(const std::string &path, std::vector<Peer> &members, std::string &err) {
    std::ifstream file(path);
    if (!file) { err = "cannot open membership file " + path; return false; }
    std::stringstream text;
    text << file.rdbuf();
    return parse_members(text.str(), members, err);
}

// Every member of every process in `processes`, `v` per process, sorted by
// identifier (D-047). Refuses two members on one identifier.
bool expand_members(const std::vector<Peer> &processes, int v, std::vector<Peer> &out, std::string &err) {
    std::vector<Peer> all;
    for (const Peer &p : processes)
        for (int i = 0; i < v; ++i) all.push_back(Peer{id_of_member(p.ip, p.port, i), p.ip, p.port});
    if (!validate_ring(all, err)) return false;       // sorts, and catches any collision
    out = std::move(all);
    return true;
}

// Every series this process exposes (D-084; docs/observability.md says why each is
// there). Called once, after the members and the store exist and before any thread
// that could read the registry starts.
void register_metrics(double started) {
    Registry &r = g_metrics;
    r.gauge("process_start_time_seconds", "When this process started, in seconds since the Unix epoch.",
            [started] { return started; });

    // Connections and errors: one thread per connection (D-061) is a known limit.
    r.gauge("chordfs_connections_open", "Connections being served now, each on its own thread.",
            [] { return static_cast<double>(g_connections_open.load()); });
    g_connections_total = &r.counter("chordfs_connections_total", "Connections accepted.");
    for (const char *type : ERROR_TYPES)
        g_errors[type] = &r.counter("chordfs_request_errors_total",
                                    "Requests dropped or refused before they could be answered, by why.",
                                    labels({{"type", type}}));

    // Lookups this process makes itself: finger repair, joins, repair's hand-offs.
    g_lookup_hops = &r.histogram("chordfs_lookup_hops", "Fingers followed per lookup (D-026).", hop_buckets());
    g_lookup_seconds = &r.histogram("chordfs_lookup_seconds", "Time per successful lookup.", latency_buckets());
    for (LookupError e : {LookupError::Refused, LookupError::Timeout, LookupError::Closed, LookupError::Unreachable,
                          LookupError::ErrReply, LookupError::Malformed, LookupError::NoProgress,
                          LookupError::TooManyRequests})
        g_lookup_failures[lookup_error_name(e)] = &r.counter("chordfs_lookup_failures_total", "Lookups that failed, by why.",
                                                             labels({{"error", lookup_error_name(e)}}));

    // The ring: summed over this process's members.
    auto sum = [](std::atomic<std::uint64_t> RingCounters::*field) {
        return [field] {
            double total = 0;
            for (const auto &m : g_members) total += static_cast<double>((m->counters.*field).load());
            return total;
        };
    };
    r.gauge("chordfs_ring_members", "Ring members (virtual nodes) in this process.",
            [] { return static_cast<double>(g_members.size()); });
    r.counter_fn("chordfs_ring_rounds_total", "Maintenance rounds run, all members.", sum(&RingCounters::rounds));
    r.counter_fn("chordfs_ring_strikes_total", "Failed calls counted against a peer (D-035).", sum(&RingCounters::strikes));
    r.counter_fn("chordfs_ring_evictions_total", "Members evicted, by the evidence (D-013).",
                 sum(&RingCounters::evicted_refused), labels({{"evidence", "refused"}}));
    r.counter_fn("chordfs_ring_evictions_total", "Members evicted, by the evidence (D-013).",
                 sum(&RingCounters::evicted_strikes), labels({{"evidence", "strikes"}}));
    r.counter_fn("chordfs_ring_evictions_total", "Members evicted, by the evidence (D-013).",
                 sum(&RingCounters::evicted_orphan), labels({{"evidence", "orphan"}}));
    r.counter_fn("chordfs_ring_successor_changes_total", "Closer successors adopted (joins).",
                 sum(&RingCounters::new_successor));
    r.counter_fn("chordfs_ring_rejoins_total", "Orphaned members that rejoined (D-040).", sum(&RingCounters::rejoins));
    r.gauge("chordfs_ring_successor_list_min", "The shortest successor list among this process's members.", [] {
        std::size_t shortest = MAX_SUCCESSORS;
        for (const auto &m : g_members) {
            std::lock_guard<std::mutex> lock(m->mu);
            shortest = std::min(shortest, m->succs.size());
        }
        return static_cast<double>(shortest);
    });
    r.gauge("chordfs_ring_orphaned_members", "Members that lost every node they knew (D-040).", [] {
        int n = 0;
        for (const auto &m : g_members) {
            std::lock_guard<std::mutex> lock(m->mu);
            n += m->orphaned ? 1 : 0;
        }
        return static_cast<double>(n);
    });

    // Chunks: what is held, what is asked, what moves.
    r.gauge("chordfs_store_chunks", "Chunks held by this process.", [] {
        if (!g_chunks.store) return 0.0;
        std::lock_guard<std::mutex> lock(g_store.mu);
        return static_cast<double>(g_store.keys.size());
    });
    r.gauge("chordfs_store_bytes", "Bytes of chunks held by this process.",
            [] { return static_cast<double>(g_store.bytes.load()); });
    r.gauge("chordfs_body_slots_in_use", "Chunk bodies held in memory now, of 32 (D-052).",
            [] { return static_cast<double>(g_chunks.bodies.load()); });
    for (const char *verb : CHUNK_VERBS) {
        for (const char *result : CHUNK_RESULTS)
            g_chunk_requests[std::string(verb) + "/" + result] =
                &r.counter("chordfs_chunk_requests_total", "Chunk requests answered, by verb and result.",
                           labels({{"verb", verb}, {"result", result}}));
        g_chunk_seconds[verb] = &r.histogram("chordfs_chunk_request_seconds",
                                             "From a chunk request read to its reply sent; a PUT is answered once W copies exist.",
                                             latency_buckets(), labels({{"verb", verb}}));
    }
    g_bytes_in = &r.counter("chordfs_chunk_bytes_total", "Chunk bytes received (PUT, STORE) and sent (GET).",
                            labels({{"direction", "in"}}));
    g_bytes_out = &r.counter("chordfs_chunk_bytes_total", "Chunk bytes received (PUT, STORE) and sent (GET).",
                             labels({{"direction", "out"}}));
    r.expose("chordfs_put_replication_lag_seconds",
             "From a write's acknowledgement to its last copy: the window with fewer than three copies.",
             g_chunks.replication_lag);
    r.expose("chordfs_put_copies_short_total", "Coordinated writes whose copies ended below three.", g_chunks.copies_short);

    // Collection (D-090, D-091).
    r.expose("chordfs_gc_dropped_chunks_total", "Chunks dropped at a collector's request.", g_chunks.dropped_chunks);
    r.expose("chordfs_gc_dropped_bytes_total", "Bytes freed by dropping chunks.", g_chunks.dropped_bytes);
    r.expose("chordfs_gc_kept_young_total", "Chunks a collector asked to drop, kept because they were written within the grace.",
             g_chunks.kept_young);
    r.gauge("chordfs_gc_grace_seconds", "How long a chunk must go unwritten before it may be dropped.",
            [] { return static_cast<double>(g_chunks.gc_grace.count()) / 1000; });

    // Repair (D-050).
    for (const char *what : {"rounds", "full_scans", "chunks_sent", "failures", "handed_off", "scrubbed", "digests_sent",
                             "digests_matched", "have_sent"})
        g_repair[what] = &r.counter(std::string("chordfs_repair_") + what + "_total",
                                    std::string("Repair: ") + what + ", summed over rounds (D-050).");
    r.gauge("chordfs_repair_pending_arcs", "Owned arcs to be checked again next round.",
            [] { return static_cast<double>(g_repair_pending.load()); });
}

int usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s <ip> <port> <membership-file>\n"
                 "       %s <ip> <port> (--create | --join <ip:port> | --wired <file>)\n"
                 "                      [--period <ms>] [--timeout <ms>] [--succ <1..%d>]\n"
                 "                      [--vnodes <1..%d>] [--store <dir>] [--scan <rounds>] [--read-deadline <ms>]\n"
                 "                      [--metrics <port>] [--log-level debug|info|warn|error]\n"
                 "                      [--gc-key <file>] [--gc-grace <s>]\n",
                 argv0, argv0, MAX_SUCCESSORS, MAX_VNODES);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) return usage(argv[0]);
    std::signal(SIGPIPE, SIG_IGN);      // D-011: a departed peer is an error value

    Peer me;
    if (!parse_address(std::string(argv[1]) + ":" + argv[2], me)) {
        std::fprintf(stderr, "[node] invalid own address %s:%s\n", argv[1], argv[2]);
        return 2;
    }

    // ---- the command line ----
    enum class Mode { Fixed, Create, Join, Wired } mode = Mode::Fixed;
    std::string file, known_text, store_dir;
    int period_ms = DEFAULT_PERIOD_MS, successors = DEFAULT_SUCCESSORS;
    int vnodes = DEFAULT_VNODES, scan_rounds = DEFAULT_SCAN_ROUNDS, metrics_port = 0, gc_grace_s = DEFAULT_GC_GRACE_S;
    std::string gc_key_file;
    Level level = Level::Info;
    if (argc == 4 && argv[3][0] != '-') {
        file = argv[3];                                   // Phase 2's form, unchanged
        vnodes = 1;                                       // the fixed ring is one member per process
    } else {
        bool chosen = false;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            const bool has_value = i + 1 < argc;
            if (a == "--create" && !chosen)                    { mode = Mode::Create; chosen = true; }
            else if (a == "--join" && has_value && !chosen)    { mode = Mode::Join; known_text = argv[++i]; chosen = true; }
            else if (a == "--wired" && has_value && !chosen)   { mode = Mode::Wired; file = argv[++i]; chosen = true; }
            else if (a == "--period" && has_value)  { if (!parse_int(argv[++i], 10, 600000, period_ms)) return usage(argv[0]); }
            else if (a == "--timeout" && has_value) { if (!parse_int(argv[++i], 10, 600000, g_timeout_ms)) return usage(argv[0]); }
            else if (a == "--succ" && has_value)    { if (!parse_int(argv[++i], 1, MAX_SUCCESSORS, successors)) return usage(argv[0]); }
            else if (a == "--vnodes" && has_value)  { if (!parse_int(argv[++i], 1, MAX_VNODES, vnodes)) return usage(argv[0]); }
            else if (a == "--read-deadline" && has_value) { if (!parse_int(argv[++i], 10, 600000, g_read_deadline_ms)) return usage(argv[0]); }
            else if (a == "--scan" && has_value)    { if (!parse_int(argv[++i], 0, 1000000, scan_rounds)) return usage(argv[0]); }
            else if (a == "--store" && has_value && store_dir.empty()) { store_dir = argv[++i]; if (store_dir.empty()) return usage(argv[0]); }
            else if (a == "--metrics" && has_value) { if (!parse_int(argv[++i], 1, 65535, metrics_port)) return usage(argv[0]); }
            else if (a == "--log-level" && has_value) { if (!parse_level(argv[++i], level)) return usage(argv[0]); }
            else if (a == "--gc-key" && has_value)  { gc_key_file = argv[++i]; }
            else if (a == "--gc-grace" && has_value) { if (!parse_int(argv[++i], 0, 9999999, gc_grace_s)) return usage(argv[0]); }
            else return usage(argv[0]);
        }
        if (!chosen) return usage(argv[0]);
    }

    // Structured lines from here on (D-086), each with the wall-clock time, so the
    // logs of many nodes -- and a machine-wide record such as `vmstat` -- can be put
    // on one timeline when a failure has to be explained after the fact. The ring's
    // and the store's event texts are kept word for word in `msg`.
    log_setup("node:" + std::to_string(me.port), level);
    const double started = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto log = [](const std::string &event) { log_event(Level::Info, "ring", {{"msg", event}}); };

    // ---- the members (D-047) ----
    for (int i = 0; i < vnodes; ++i) {
        const Peer member{id_of_member(me.ip, me.port, i), me.ip, me.port};
        for (const auto &m : g_members) {
            if (m->self.id == member.id) {
                log_event(Level::Error, "node.start", {{"msg", "members " + std::to_string(i) + " and another share identifier " +
                                                               id_to_hex(member.id) + "; use fewer --vnodes"}});
                return 1;
            }
        }
        g_members.push_back(std::make_unique<NodeState>(member, successors));
        g_members.back()->log = log;
    }

    // ---- the starting state (D-029) ----
    if (mode == Mode::Fixed && vnodes != 1) return usage(argv[0]);    // the fixed ring has one member
    if (mode == Mode::Fixed || mode == Mode::Wired) {
        std::vector<Peer> processes, members;     // both die at the end of this block (D-020)
        std::string err;
        if (!read_members(file, processes, err) || !expand_members(processes, vnodes, members, err)) {
            log_event(Level::Error, "node.start", {{"msg", err}});
            return 1;
        }
        bool listed = false;
        for (const Peer &p : processes) listed = listed || same_process(p, me);
        if (!listed) {
            log_event(Level::Error, "node.start", {{"msg", me.ip + ":" + std::to_string(me.port) + " is not in " + file}});
            return 1;
        }
        for (const auto &m : g_members) init_wired(*m, members);
    } else if (mode == Mode::Create) {
        // A new ring of this process's own members: the wired state over just them.
        std::vector<Peer> own;
        for (const auto &m : g_members) own.push_back(m->self);
        std::sort(own.begin(), own.end(), [](const Peer &a, const Peer &b) { return a.id < b.id; });
        if (own.size() == 1) init_alone(*g_members.front());
        else for (const auto &m : g_members) init_wired(*m, own);
    } else {
        Peer known;
        if (!parse_address(known_text, known)) {
            log_event(Level::Error, "node.start", {{"msg", "invalid --join address " + known_text}});
            return 2;
        }
        // Not listening yet, on purpose (D-029): until every member has joined, a
        // connection to this address is refused, which evicts any stale entry.
        for (const auto &m : g_members) {
            bool joined = false;
            for (int attempt = 1; attempt <= JOIN_ATTEMPTS && !joined; ++attempt) {
                std::string why;
                const JoinStatus st = join(*m, known, g_transport, why);
                if (st == JoinStatus::Joined) { joined = true; break; }
                log_event(Level::Warn, "node.join", {{"attempt", std::to_string(attempt) + "/" + std::to_string(JOIN_ATTEMPTS)},
                                                     {"member", id_to_hex(m->self.id)}, {"msg", why}});
                if (st == JoinStatus::Collision) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
            }
            if (!joined) {
                log_event(Level::Error, "node.join", {{"msg", "giving up: could not join through " + known_text}});
                return 1;
            }
        }
    }

    // ---- the chunk store (D-045) and collection (D-091, D-092) ----
    g_chunks.gc_grace = std::chrono::milliseconds(static_cast<long long>(gc_grace_s) * 1000);
    if (!gc_key_file.empty()) {
        std::ifstream f(gc_key_file);
        std::string key;
        if (!f || !std::getline(f, key)) key.clear();
        while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
        if (!valid_secret(key)) {
            log_event(Level::Error, "node.start", {{"msg", gc_key_file + " must hold one key of 32 to 64 lowercase hexadecimal characters"}});
            return 1;
        }
        g_chunks.gc_key = key;
    }
    g_chunks.send = frame_transport;
    g_chunks.log = [](const std::string &event) { log_event(Level::Info, "storage", {{"msg", event}}); };
    for (const auto &m : g_members) g_chunks.members.push_back(m.get());
    if (!store_dir.empty()) {
        std::string report, err;
        if (!store_open(g_store, store_dir, report, err)) {
            log_event(Level::Error, "node.start", {{"msg", err}});
            return 1;
        }
        g_chunks.store = &g_store;
        log_event(Level::Info, "node.start", {{"msg", "store " + store_dir + ": " + report}});
    }

    // ---- metrics (D-083): every series registered before any thread can read them ----
    register_metrics(started);
    set_lookup_observer(observe_lookup);
    if (metrics_port != 0) {
        std::string err;
        if (!serve_metrics(me.ip, metrics_port, [] { return g_metrics.render(); }, err)) {
            log_event(Level::Error, "node.start", {{"msg", err}});
            return 1;
        }
    }

    const int listener = listen_on(me.ip, me.port);
    if (listener < 0) {
        log_event(Level::Error, "node.start", {{"msg", "cannot listen on " + me.ip + ":" + std::to_string(me.port) + ": " +
                                                       std::strerror(errno)}});
        return 1;
    }

    for (const auto &m : g_members) {
        std::string pred_text = "none", succ_text;
        {
            std::lock_guard<std::mutex> lock(m->mu);     // copy under the lock, print after it
            if (m->has_pred) pred_text = id_to_hex(m->pred.id);
            succ_text = id_to_hex(m->succs[0].id);
        }
        log_event(Level::Info, "node.member", {{"id", id_to_hex(m->self.id)}, {"pred", pred_text}, {"succ", succ_text}});
    }

    // The fixed ring has no maintenance thread: its tables never change (D-029).
    // Repair runs only where there is a store, and a ring that changes.
    try {
        if (mode != Mode::Fixed) std::thread(maintain, period_ms).detach();
        if (mode != Mode::Fixed && g_chunks.store) std::thread(repair, period_ms, scan_rounds).detach();
    } catch (const std::system_error &e) {
        log_event(Level::Error, "node.start", {{"msg", std::string("cannot start a background thread: ") + e.what()}});
        return 1;
    }

    while (true) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR) {
                log_event(Level::Error, "node.accept", {{"msg", std::strerror(errno)}});
                // Out of descriptors (EMFILE) would otherwise make this loop spin at full speed.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        // std::thread's constructor throws if the system cannot create a thread.
        // Uncaught, that would terminate the whole node over one connection.
        try {
            std::thread(serve, fd).detach();
        } catch (const std::system_error &e) {
            log_event(Level::Error, "node.accept", {{"msg", std::string("cannot start a thread: ") + e.what()}});
            close(fd);
        }
    }
}
