// client -- store files in the ring and name them in the tracker (Phase 5).
//
//   ./client --tracker <ip:port>[,<ip:port>...] --ring <ip:port>[,<ip:port>...] [options] <command>
//
// --tracker lists every replica of the tracker group (D-081): requests go to the first
// that answers, following REDIRECT to the leader.
//
// Commands:
//   put <file> <name>        cut the file into chunks, store them and its manifests in
//                            the ring, then register <name> -> root manifest key
//   get <name> <file>        resolve <name>, fetch and check its manifests, fetch every
//                            chunk in parallel, verified, into a part file renamed onto <file>
//   get-key <key> <file>     the same from a manifest key, without the tracker (D-057)
//   info <name>              resolve <name> and check its whole manifest tree
//   resolve <name>           print the manifest key a name stands for
//   ls                       list every name
//   delete <name> [<token>]  unname a file with the token its upload printed (D-089);
//                            its chunks are freed later by `gc` if no other file uses them
//
// Options:
//   --workers <n>       chunks moved at once, 1..64 (default DEFAULT_WORKERS, D-056)
//   --w <1..3>          copies confirmed before a chunk counts as stored (default 2, D-015)
//   --chunk-size <b>    upload only: bytes per chunk (default 524288, D-016)
//   --fanout <n>        upload only: entries per manifest, 2..8192 (default 8192, D-054)
//   --stats <file>      write one CSV line per chunk: op,index,bytes,micros,attempts,tried,ok
//   --tracker-wait <ms> how long to keep trying the tracker group (default 10000, D-081)
//   --quiet             no progress or fall-through notes on standard error
//   --request-id <id>   16 lowercase hex characters carried on every request (D-087);
//                       by default a random one is made for this command
//   --log <file>        append structured log lines here (D-086); none without it
//   --log-level <level> debug, info (default), warn or error
//   --metrics <port>    serve GET /metrics on this port while the command runs (D-085)
//   --token-file <file> put: own the name with this secret instead of a random one;
//                       delete: read the token from this file (keeps it out of `ps`)
//   --upload-limit <s>  put: register only within this many seconds of starting (default 3600, D-091)
//   --register-delay <ms>  put: wait this long before registering -- for testing the
//                       collector's race with an upload in flight (D-091)
//
// put and get print one line on standard output, for scripts:
//   put <name> <key> ok bytes <n> chunks <n> manifests <n> depth <n> chunk_ms <t> manifest_ms <t> total_ms <t> token <secret|->
// where the token is the secret that owns the name (D-089) -- printed once, never stored --
// or "-" when the same content was already registered under the name by an earlier upload.
//   get <name> <key> ok bytes <n> chunks <n> manifests <n> depth <n> resolve_ms <t> fetch_ms <t>
//       sync_ms <t> ready_ms <t> total_ms <t>
// where ready_ms is the time from the start of the program to every chunk verified
// and written, and total_ms adds the fsync and the rename (D-055, D-066).
//
// Exit status: 0 on success, 1 on failure, 2 on a usage error.
//
// Everything that matters is in transfer.cpp (chunking, manifests, workers, the part
// file), chunk.cpp (one chunk through the ring) and net.cpp (framing): this file is
// the command line, the tracker exchange and the signal handler. It replaced, in
// Phase 5, a coursework peer-to-peer client; that code is in the history (D-060).

#include "chord.h"
#include "chunk.h"
#include "group.h"
#include "http.h"
#include "log.h"
#include "lookup.h"
#include "metrics.h"
#include "net.h"
#include "names.h"
#include "store.h"
#include "transfer.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int CLIENT_TIMEOUT_MS = 5000;     // a hang guard on every socket (D-024), as in `chunk`
// One exchange with one tracker replica. Longer than a replica waits for a commit
// (2 s, D-075), so a replica that is merely busy answers before this fires; short
// enough that a frozen replica costs 3 s, not 5, before the next is tried.
constexpr int TRACKER_CALL_MS = 3000;

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();   // the program's start: ready_ms and total_ms count from here

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
std::string ms(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}
std::string ms(long long micros) { return ms(micros / 1000.0); }

// Every chunk exchange carries this command's request id (D-087).
Delivery client_frames(const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
    return exchange_frame(to.ip, to.port, with_request_id(line), body, reply.line, reply.body, CLIENT_TIMEOUT_MS);
}

// ---- Metrics (D-085): served while the command runs, if --metrics is given. Every
// series is registered in register_metrics() before any worker or the metrics
// thread starts; the values are atomics. ----
Registry g_metrics;
Histogram *g_lookup_hops = nullptr;
Histogram *g_lookup_seconds = nullptr;
Counter *g_lookup_failures = nullptr;
Counter *g_chunks[2][2] = {};          // [put, get][failed, ok]
Counter *g_bytes[2] = {};              // [put, get]
Histogram *g_chunk_seconds[2] = {};    // [put, get]

void observe_lookup(const LookupResult &r, double seconds) {
    if (!r.ok()) { g_lookup_failures->add(); return; }
    g_lookup_hops->observe(r.hops());
    g_lookup_seconds->observe(seconds);
}

void register_metrics(double started) {
    Registry &r = g_metrics;
    r.gauge("process_start_time_seconds", "When this process started, in seconds since the Unix epoch.",
            [started] { return started; });
    g_lookup_hops = &r.histogram("chordfs_lookup_hops", "Fingers followed per lookup (D-026).", hop_buckets());
    g_lookup_seconds = &r.histogram("chordfs_lookup_seconds", "Time per successful lookup.", latency_buckets());
    g_lookup_failures = &r.counter("chordfs_lookup_failures_total", "Lookups that failed.");
    const char *ops[] = {"put", "get"};
    for (int op = 0; op < 2; ++op) {
        for (int ok = 0; ok < 2; ++ok)
            g_chunks[op][ok] = &r.counter("chordfs_client_chunks_total", "Chunks moved by this client, by result.",
                                          labels({{"op", ops[op]}, {"result", ok ? "ok" : "failed"}}));
        g_bytes[op] = &r.counter("chordfs_client_bytes_total", "Chunk bytes moved by this client.", labels({{"op", ops[op]}}));
        g_chunk_seconds[op] = &r.histogram("chordfs_client_chunk_seconds",
                                           "Per chunk, from its first attempt to stored (put) or verified and written (get).",
                                           latency_buckets(), labels({{"op", ops[op]}}));
    }
}

// Told by the transfer's workers about each chunk as it finishes (D-085).
std::function<void(const ChunkTiming &)> chunk_observer(int op) {
    return [op](const ChunkTiming &t) {
        g_chunks[op][t.ok ? 1 : 0]->add();
        if (!t.ok) return;
        g_bytes[op]->add(t.bytes);
        g_chunk_seconds[op]->observe(static_cast<double>(t.micros) / 1e6);
        if (log_enabled(Level::Debug))
            log_event(Level::Debug, op == 0 ? "client.chunk.put" : "client.chunk.get",
                      {{"index", std::to_string(t.index)}, {"bytes", std::to_string(t.bytes)},
                       {"ms", ms(t.micros)}, {"attempts", std::to_string(t.attempts)}});
    };
}

// The ring's entry points (D-057). Every lookup starts at the preferred one; if a
// put or get fails in its lookup, the next listed node is tried, and becomes the
// preferred one for the chunks after it. Workers share this, hence the atomic.
struct Ring {
    std::vector<Peer>        seeds;
    std::atomic<std::size_t> preferred{0};
    int                      w = DEFAULT_W;
};

bool lookup_failed(const std::string &message) { return message.compare(0, 13, "lookup failed") == 0; }

PutOutcome ring_put(Ring &ring, const std::string &body) {
    const std::size_t first = ring.preferred.load();
    PutOutcome p;
    for (std::size_t k = 0; k < ring.seeds.size(); ++k) {
        const std::size_t i = (first + k) % ring.seeds.size();
        p = put_chunk(ring.seeds[i], body, ring.w, socket_transport, client_frames);
        if (p.ok || !lookup_failed(p.message)) {
            if (k > 0) ring.preferred.store(i);
            return p;
        }
    }
    return p;
}

GetOutcome ring_get(Ring &ring, const std::string &key) {
    const std::size_t first = ring.preferred.load();
    GetOutcome g;
    for (std::size_t k = 0; k < ring.seeds.size(); ++k) {
        const std::size_t i = (first + k) % ring.seeds.size();
        g = get_chunk(ring.seeds[i], key, socket_transport, client_frames);
        if (g.ok || !lookup_failed(g.message)) {
            if (k > 0) ring.preferred.store(i);
            return g;
        }
    }
    return g;
}

// One request to the tracker group: to the leader, wherever it is (group.cpp, D-081).
// Each exchange is on a fresh connection, as every exchange here is.
bool ask_tracker(GroupClient &tracker, const std::string &request, std::string &reply) {
    if (ask_group(tracker, request, reply)) return true;
    reply = "the tracker could not be reached in " + std::to_string(tracker.wait_ms) + " ms (" + reply + ")";
    return false;
}

// RESOLVE <name> -> the manifest key, or false with `err` set.
bool resolve(GroupClient &tracker, const std::string &name, std::string &key, std::string &err) {
    if (!valid_name(name)) { err = "a name is 1 to 200 visible ASCII characters, no spaces"; return false; }
    std::string reply;
    if (!ask_tracker(tracker, "RESOLVE " + name, reply)) { err = reply; return false; }
    if (reply == "MISSING") { err = "no file is named " + name; return false; }
    if (reply.compare(0, 5, "FILE ") != 0 || !valid_key(reply.substr(5))) { err = "the tracker answered: " + reply; return false; }
    key = reply.substr(5);
    return true;
}

// "ip:port", validated exactly as every other address is.
bool parse_one(const std::string &text, Peer &out) {
    std::vector<Peer> one;
    std::string err;
    if (text.empty() || text.find('\n') != std::string::npos || !parse_members(text, one, err) || one.size() != 1) return false;
    out = one[0];
    return true;
}

// "a:1,b:2,..." in the order given: the first is tried first.
bool parse_list(const std::string &text, std::vector<Peer> &out) {
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        Peer p;
        if (!parse_one(item, p)) return false;
        out.push_back(p);
    }
    return !out.empty();
}

bool parse_number(const std::string &s, long long lo, long long hi, long long &out) {
    if (s.empty() || s.size() > 12) return false;
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

bool write_stats(const std::string &path, const char *op, const std::vector<ChunkTiming> &timings) {
    std::ofstream f(path, std::ios::app);
    if (!f) return false;
    for (const ChunkTiming &t : timings)
        f << op << ',' << t.index << ',' << t.bytes << ',' << t.micros << ',' << t.attempts << ',' << t.tried << ','
          << (t.ok ? 1 : 0) << '\n';
    return static_cast<bool>(f);
}

// SIGINT and SIGTERM: remove the part file of a download in progress, then end
// the process with the conventional status, 128 + the signal number. Only
// async-signal-safe calls: remove_part_file() is one atomic read and unlink(2),
// and _exit(2) -- unlike exit(3) -- runs no destructors and flushes no streams.
void on_signal(int sig) {
    remove_part_file();
    _exit(128 + sig);
}

int usage() {
    std::fprintf(stderr,
                 "usage: client --tracker <ip:port>[,<ip:port>...] --ring <ip:port>[,<ip:port>...] [options] <command>\n"
                 "  commands: put <file> <name> | get <name> <file> | get-key <key> <file> | info <name>\n"
                 "            | resolve <name> | ls | delete <name> [<token>]\n"
                 "  options:  --workers <1..%d> --w <1..3> --chunk-size <bytes> --fanout <2..%zu>\n"
                 "            --stats <file> --quiet --tracker-wait <ms> --request-id <16 hex>\n"
                 "            --log <file> --log-level debug|info|warn|error --metrics <port>\n"
                 "            --token-file <file> --upload-limit <s> --register-delay <ms>\n",
                 MAX_WORKERS, MANIFEST_FANOUT);
    return 2;
}

// A note for the person running the command, on standard error -- and, with --log,
// the same words as a structured line, so the log tells the whole story too.
// A fresh owner secret: 128 bits from the operating system's random source, as 32
// lowercase hexadecimal characters (D-089).
std::string new_secret() {
    std::random_device rd;
    std::string out;
    char buf[9];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(rd()));
        out += buf;
    }
    return out;
}

// One line from a file: a token, as `ps` would otherwise show it on the command line.
bool read_token(const std::string &path, std::string &token) {
    std::ifstream f(path);
    if (!f || !std::getline(f, token)) return false;
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) token.pop_back();
    return valid_secret(token);
}

void say(const std::string &what) {
    std::fprintf(stderr, "client: %s\n", what.c_str());
    log_event(Level::Warn, "client.note", {{"msg", what}});
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);      // D-011
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ---- the command line ----
    GroupClient tracker;
    tracker.exchange = [](const Address &to, const std::string &request, std::string &reply) {
        return exchange_line(to.ip, to.port, with_request_id(request), reply, TRACKER_CALL_MS);
    };
    tracker.now = [] {
        return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       Clock::now().time_since_epoch()).count());
    };
    tracker.sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    bool have_tracker = false;
    std::string err;
    Ring ring;
    TransferOptions opt;
    std::string stats, request_id, log_path, token_file;
    bool quiet = false;
    int metrics_port = 0;
    long long upload_limit_s = 3600, register_delay_ms = 0;
    Level level = Level::Info;
    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        long long n = 0;
        if (a == "--tracker" && v)                { if (!parse_address_list(argv[++i], tracker.replicas, err)) return usage(); have_tracker = true; }
        else if (a == "--tracker-wait" && v)      { if (!parse_number(argv[++i], 0, 600000, n)) return usage(); tracker.wait_ms = static_cast<int>(n); }
        else if (a == "--ring" && v)              { if (!parse_list(argv[++i], ring.seeds)) return usage(); }
        else if (a == "--workers" && v)           { if (!parse_number(argv[++i], 1, MAX_WORKERS, n)) return usage(); opt.workers = static_cast<int>(n); }
        else if (a == "--w" && v)                 { if (!parse_number(argv[++i], 1, 3, n)) return usage(); ring.w = static_cast<int>(n); }
        else if (a == "--chunk-size" && v)        { if (!parse_number(argv[++i], 1, static_cast<long long>(MAX_CHUNK), n)) return usage(); opt.chunk = static_cast<std::uint64_t>(n); }
        else if (a == "--fanout" && v)            { if (!parse_number(argv[++i], 2, static_cast<long long>(MANIFEST_FANOUT), n)) return usage(); opt.fanout = static_cast<std::size_t>(n); }
        else if (a == "--stats" && v)             { stats = argv[++i]; }
        else if (a == "--quiet")                  { quiet = true; }
        else if (a == "--request-id" && v)        { request_id = argv[++i]; if (!valid_request_id(request_id)) return usage(); }
        else if (a == "--log" && v)               { log_path = argv[++i]; }
        else if (a == "--log-level" && v)         { if (!parse_level(argv[++i], level)) return usage(); }
        else if (a == "--metrics" && v)           { if (!parse_number(argv[++i], 1, 65535, n)) return usage(); metrics_port = static_cast<int>(n); }
        else if (a == "--token-file" && v)        { token_file = argv[++i]; }
        else if (a == "--upload-limit" && v)      { if (!parse_number(argv[++i], 1, 100000000, upload_limit_s)) return usage(); }
        else if (a == "--register-delay" && v)    { if (!parse_number(argv[++i], 0, 100000000, register_delay_ms)) return usage(); }
        else if (a.compare(0, 2, "--") == 0)      { return usage(); }
        else                                      { words.push_back(a); }
    }
    if (words.empty()) return usage();
    const std::string cmd = words[0];

    // ---- logging, the request id and metrics: all set before any other thread starts ----
    int log_fd = -1;
    if (!log_path.empty()) {
        // O_APPEND: every write lands at the end, so several clients can share one file.
        log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) { std::fprintf(stderr, "client: cannot open %s: %s\n", log_path.c_str(), std::strerror(errno)); return 2; }
    }
    log_setup("client", level, log_fd);
    set_process_request_id(request_id.empty() ? new_request_id() : request_id);
    register_metrics(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    set_lookup_observer(observe_lookup);
    if (metrics_port != 0) {
        if (!serve_metrics("127.0.0.1", metrics_port, [] { return g_metrics.render(); }, err)) { say(err); return 1; }
    }
    {
        std::string all;
        for (const std::string &w : words) all += (all.empty() ? "" : " ") + w;
        log_event(Level::Info, "client.start", {{"command", all}});
    }

    if (!quiet) opt.log = say;                    // fprintf locks stderr per call: safe from many workers

    const bool needs_tracker = cmd == "put" || cmd == "get" || cmd == "info" || cmd == "resolve" || cmd == "ls" || cmd == "delete";
    const bool needs_ring = cmd == "put" || cmd == "get" || cmd == "get-key" || cmd == "info";
    if ((needs_tracker && !have_tracker) || (needs_ring && ring.seeds.empty())) return usage();
    PutFn put = [&ring](const std::string &body) { return ring_put(ring, body); };
    GetFn get = [&ring](const std::string &key) { return ring_get(ring, key); };

    // ---- put <file> <name> ----
    if (cmd == "put" && words.size() == 3) {
        const std::string &file = words[1], &name = words[2];
        if (!valid_name(name)) { say("a name is 1 to 200 visible ASCII characters, no spaces"); return 2; }
        // The owner's secret (D-089): chosen here, before anything is stored, so a
        // retried registration carries the same hash. Only the hash leaves this process.
        std::string secret = new_secret();
        if (!token_file.empty() && !read_token(token_file, secret)) {
            say(token_file + " must hold one token of 32 to 64 lowercase hexadecimal characters");
            return 2;
        }
        opt.on_chunk = chunk_observer(0);
        const UploadOutcome u = upload_file(file, put, opt);
        if (!stats.empty() && !write_stats(stats, "put", u.timings)) say("could not write " + stats);
        if (!u.ok) { say("upload failed: " + u.message); return 1; }
        if (register_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(register_delay_ms));
        // An upload that took longer than the limit must not name its chunks: a
        // collector may already count them as old garbage (D-091's inequality).
        if (ms_since(g_start) > static_cast<double>(upload_limit_s) * 1000) {
            say("the upload took longer than its limit of " + std::to_string(upload_limit_s) + " s, so its chunks may "
                "already be collected; the name was not registered. Run the same put again.");
            return 1;
        }
        // Only now is the name registered: every chunk and manifest is stored, so a
        // name never points at a file that is not all there (D-017).
        std::string reply;
        if (!ask_tracker(tracker, "REGISTER " + name + " " + u.root_key + " " + owner_hash(secret), reply)) {
            say(reply);
            return 1;
        }
        if (reply == "EXISTS " + u.root_key) {
            // The same content under the same name, registered by an earlier upload with
            // its own token: the name already means this file, so this succeeds -- but
            // the earlier upload's token, not a new one, is what can delete it.
            say(name + " is already registered for this same content by an earlier upload; that upload's token owns it");
            secret = "-";
        } else if (reply.compare(0, 7, "EXISTS ") == 0) {
            say(name + " is already registered for another file (" + reply.substr(7) + "). "
                "This upload's chunks stay in the ring, unnamed, until a collection removes them.");
            return 1;
        } else if (reply != "OK") {
            say("the tracker answered: " + reply);
            return 1;
        }
        log_event(Level::Info, "client.put", {{"name", name}, {"key", u.root_key}, {"result", "ok"},
                                              {"bytes", std::to_string(u.size)}, {"chunks", std::to_string(u.chunks)},
                                              {"manifests", std::to_string(u.manifests)}, {"ms", ms(ms_since(g_start))}});
        std::printf("put %s %s ok bytes %llu chunks %zu manifests %zu depth %d chunk_ms %s manifest_ms %s total_ms %s token %s\n",
                    name.c_str(), u.root_key.c_str(), static_cast<unsigned long long>(u.size), u.chunks, u.manifests, u.depth,
                    ms(u.chunk_micros).c_str(), ms(u.manifest_micros).c_str(), ms(ms_since(g_start)).c_str(), secret.c_str());
        return 0;
    }

    // ---- get <name> <file>, get-key <key> <file> ----
    if ((cmd == "get" || cmd == "get-key") && words.size() == 3) {
        std::string key, err;
        const std::string &dest = words[2];
        if (cmd == "get") {
            if (!resolve(tracker, words[1], key, err)) { say(err); return 1; }
        } else {
            key = words[1];
            if (!valid_key(key)) { say("a key is 40 lowercase hexadecimal characters"); return 2; }
        }
        opt.for_signals = true;                    // Ctrl-C removes the part file (D-055)
        opt.on_chunk = chunk_observer(1);
        const DownloadOutcome d = download_file(key, dest, get, opt);
        if (!stats.empty() && !write_stats(stats, "get", d.timings)) say("could not write " + stats);
        if (!d.ok) { say("download failed: " + d.message); return 1; }
        const double total = ms_since(g_start);
        log_event(Level::Info, "client.get", {{"name", words[1]}, {"key", key}, {"result", "ok"},
                                              {"bytes", std::to_string(d.size)}, {"chunks", std::to_string(d.chunks)},
                                              {"manifests", std::to_string(d.manifests)}, {"ms", ms(total)}});
        std::printf("get %s %s ok bytes %llu chunks %zu manifests %zu depth %d resolve_ms %s fetch_ms %s sync_ms %s "
                    "ready_ms %s total_ms %s\n",
                    words[1].c_str(), key.c_str(), static_cast<unsigned long long>(d.size), d.chunks, d.manifests, d.depth,
                    ms(d.resolve_micros).c_str(), ms(d.fetch_micros).c_str(), ms(d.sync_micros).c_str(),
                    ms(total - d.sync_micros / 1000.0).c_str(), ms(total).c_str());
        return 0;
    }

    // ---- info <name> ----
    if (cmd == "info" && words.size() == 2) {
        std::string key, err;
        if (!resolve(tracker, words[1], key, err)) { say(err); return 1; }
        const Resolved r = resolve_manifest(key, get);
        if (!r.ok) { say("manifest check failed: " + r.message); return 1; }
        std::printf("info %s %s bytes %llu chunk %llu chunks %zu manifests %zu depth %d\n", words[1].c_str(), key.c_str(),
                    static_cast<unsigned long long>(r.size), static_cast<unsigned long long>(r.chunk), r.chunks.size(),
                    r.manifests, r.depth);
        return 0;
    }

    // ---- resolve <name> ----
    if (cmd == "resolve" && words.size() == 2) {
        std::string key, err;
        if (!resolve(tracker, words[1], key, err)) { say(err); return 1; }
        std::printf("%s\n", key.c_str());
        return 0;
    }

    // ---- delete <name> [<token>]: unname it; the chunks wait for a collection (D-089) ----
    if (cmd == "delete" && (words.size() == 2 || words.size() == 3)) {
        std::string secret;
        if (words.size() == 3) secret = words[2];
        else if (token_file.empty() || !read_token(token_file, secret)) { say("delete needs the token: <token> or --token-file"); return 2; }
        if (!valid_name(words[1]) || !valid_secret(secret)) { say("a token is 32 to 64 lowercase hexadecimal characters"); return 2; }
        std::string reply;
        if (!ask_tracker(tracker, "UNREGISTER " + words[1] + " " + secret, reply)) { say(reply); return 1; }
        if (reply == "MISSING") { say("no file is named " + words[1] + " (if a retry was needed, an earlier attempt may have deleted it)"); return 1; }
        if (reply == "DENIED") { say("that token does not own " + words[1]); return 1; }
        if (reply != "OK") { say("the tracker answered: " + reply); return 1; }
        log_event(Level::Info, "client.delete", {{"name", words[1]}, {"result", "ok"}});
        std::printf("delete %s ok\n", words[1].c_str());
        return 0;
    }

    // ---- ls: every name, a page at a time ----
    if (cmd == "ls" && words.size() == 1) {
        std::string after;
        while (true) {
            std::string reply;
            if (!ask_tracker(tracker, after.empty() ? "LIST" : "LIST " + after, reply)) { say(reply); return 1; }
            std::istringstream in(reply);
            std::string tag;
            int n = -1;
            if (!(in >> tag >> n) || tag != "NAMES" || n < 0) { say("the tracker answered: " + reply); return 1; }
            if (n == 0) return 0;
            for (int i = 0; i < n && in >> after; ++i) std::printf("%s\n", after.c_str());
        }
    }

    return usage();
}
