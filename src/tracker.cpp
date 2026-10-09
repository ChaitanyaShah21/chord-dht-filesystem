// tracker -- the namespace, replicated by Raft: file name -> manifest key, and nothing else
// (D-017, D-058, D-069 to D-082).
//
//   ./tracker <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]
//                    [--group <ip:port>,<ip:port>,... --id <i>] [--election <ms>]
//                    [--metrics <port>] [--log-level debug|info|warn|error]
//
// Clients:
//   REGISTER <name> <manifest-key>  -> OK | EXISTS <key> | ERR <reason>
//   RESOLVE <name>                  -> FILE <key> | MISSING | ERR <reason>
//   LIST [<after>]                  -> NAMES <n> <name> ...
//   PING                            -> PONG
//   and from a replica that is not the leader: REDIRECT <ip:port> | UNAVAILABLE <reason>
// Replicas (raft_wire.h):
//   VOTE ...                        -> VOTED <term> <0|1>
//   APPEND ... <length> + body      -> APPENDED <term> <0|1> <index>
// Harnesses:
//   STATUS                          -> STATUS <id> <role> <term> <leader|-> <commit> <applied> <last-index>
//
// Without --group this is a group of one: the single tracker, on the same code (D-080).
// With it, every replica is given the same list and its own position in it (D-079); the
// port must be the one the list gives this replica. The log is <dir>/raft_<port>.log.
//
// The protocol is raft.cpp, its file raft_log.cpp, the threads and the lock replica.cpp,
// the state machine names.cpp, the framing net.cpp. This file is sockets, the command
// line, and what the replica reports: its metrics (D-083) and its log lines (D-086).

#include "group.h"
#include "http.h"
#include "log.h"
#include "lookup.h"     // split_single_spaces
#include "metrics.h"
#include "net.h"
#include "raft_log.h"
#include "replica.h"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace {

// A request line, and a body, must arrive whole within this (D-061).
constexpr int DEFAULT_READ_DEADLINE_MS = 10000;

// Set once in main() before any thread starts, only read afterwards.
Replica *g_replica = nullptr;
int g_read_deadline_ms = DEFAULT_READ_DEADLINE_MS;

// ---- Metrics (D-083, D-084): registered in register_metrics() before any thread
// that reads them starts; the values are atomics. ----
Registry g_metrics;
std::atomic<int> g_connections_open{0};
Counter *g_connections_total = nullptr;
Histogram *g_flush_seconds = nullptr;
std::map<std::string, Counter *> g_requests;        // by "verb/result"
std::map<std::string, Histogram *> g_seconds;       // by verb
std::map<std::string, Counter *> g_errors;          // by type

const char *const VERBS[] = {"register", "unregister", "resolve", "list", "entries", "ping",
                             "vote", "append", "status", "other"};
const char *const RESULTS[] = {"ok", "exists", "missing", "denied", "redirect", "unavailable", "error"};

void count(std::map<std::string, Counter *> &family, const std::string &which) {
    const auto it = family.find(which);
    if (it != family.end()) it->second->add();
}

// The Raft log's flushes, timed (D-082): a Storage that times the one it wraps.
class TimedStorage : public Storage {
public:
    explicit TimedStorage(Storage &inner) : inner_(inner) {}
    bool save(Term term, int voted_for, Index from, const std::vector<Entry> &log) override {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = inner_.save(term, voted_for, from, log);
        if (g_flush_seconds) g_flush_seconds->observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return ok;
    }

private:
    Storage &inner_;
};

// "REGISTER a b" -> "register"; anything not in VERBS -> "other".
std::string verb_of(const std::string &line) {
    std::string v = line.substr(0, line.find(' '));
    for (char &c : v) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char *known : VERBS)
        if (v == known) return v;
    return "other";
}

// A reply's meaning, as one of RESULTS: "FILE k", "NAMES ...", "PONG", "VOTED ..." are ok.
const char *result_of(const std::string &reply) {
    const std::string w = reply.substr(0, reply.find(' '));
    if (w == "EXISTS") return "exists";
    if (w == "MISSING") return "missing";
    if (w == "DENIED") return "denied";
    if (w == "REDIRECT") return "redirect";
    if (w == "UNAVAILABLE") return "unavailable";
    if (w == "ERR") return "error";
    return "ok";
}

// Count, time and log one request once it has been answered.
void record(const std::string &line, const std::string &reply, std::chrono::steady_clock::time_point t0) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::string verb = verb_of(line);
    count(g_requests, verb + "/" + result_of(reply));
    const auto h = g_seconds.find(verb);
    if (h != g_seconds.end()) h->second->observe(seconds);
    // A registration, a deletion and a resolve are one per file: info. Replication
    // traffic, LIST, ENTRIES and PING are many: debug. Only the name is logged from a
    // request line -- an UNREGISTER's secret never reaches a log (D-089).
    const Level level = verb == "register" || verb == "unregister" || verb == "resolve" ? Level::Info : Level::Debug;
    if (!log_enabled(level)) return;
    const std::vector<std::string> w = split_single_spaces(line);
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.3f", seconds * 1000);
    log_event(level, ("tracker." + verb).c_str(),
              {{"name", w.size() > 1 && verb != "vote" && verb != "append" ? w[1] : ""},
               {"result", reply.substr(0, reply.find(' '))}, {"reply", reply.substr(0, 120)}, {"ms", ms}});
}

// One connection: answer requests until the peer closes, a send fails, a line is
// too long or a body malformed (the stream cannot be trusted after either), or the
// next line or body does not arrive in time. Runs on its own detached thread.
void serve(int fd) {
    g_connections_open.fetch_add(1);
    g_connections_total->add();
    std::string raw, rid, line, body;
    while (true) {
        if (!recv_line_by(fd, raw, deadline_in(g_read_deadline_ms))) {
            if (errno == EAGAIN) count(g_errors, "deadline");          // errno 0: the peer closed
            else if (errno == EMSGSIZE) count(g_errors, "line_too_long");
            break;
        }
        const auto t0 = std::chrono::steady_clock::now();
        // The request id comes off first (D-087), and stays this thread's while the
        // request is handled.
        const RidPrefix rp = split_request_id(raw, rid, line);
        if (rp == RidPrefix::Malformed) {
            count(g_errors, "malformed");
            send_line(fd, "ERR malformed request id");
            break;
        }
        if (rp == RidPrefix::None) rid.clear();
        RequestScope scope(rid);

        body.clear();
        std::size_t length = 0;
        const Body b = body_length(line, length);       // only APPEND carries one here
        if (b == Body::Malformed || b == Body::TooLarge) {
            count(g_errors, "malformed");
            send_line(fd, b == Body::Malformed ? "ERR malformed body length" : "ERR body too large");
            break;
        }
        if (b == Body::Present && !recv_exact_by(fd, length, body, deadline_in(g_read_deadline_ms))) {
            count(g_errors, errno == EAGAIN ? "deadline" : "closed_mid_body");
            break;
        }
        const std::string reply = g_replica->handle(line, body);
        const bool sent = send_line(fd, reply);
        record(line, reply, t0);
        if (!sent) break;
    }
    close(fd);
    g_connections_open.fetch_sub(1);
}

// Every series this replica exposes (D-084; docs/observability.md says why each is
// there). Called once, before the metrics thread and the request threads start.
void register_metrics(Replica &rep, double started) {
    Registry &r = g_metrics;
    r.gauge("process_start_time_seconds", "When this process started, in seconds since the Unix epoch.",
            [started] { return started; });
    r.gauge("chordfs_connections_open", "Connections being served now, each on its own thread.",
            [] { return static_cast<double>(g_connections_open.load()); });
    g_connections_total = &r.counter("chordfs_connections_total", "Connections accepted.");
    for (const char *type : {"deadline", "line_too_long", "malformed", "closed_mid_body"})
        g_errors[type] = &r.counter("chordfs_request_errors_total",
                                    "Requests dropped or refused before they could be answered, by why.",
                                    labels({{"type", type}}));
    for (const char *verb : VERBS) {
        for (const char *result : RESULTS)
            g_requests[std::string(verb) + "/" + result] =
                &r.counter("chordfs_tracker_requests_total", "Requests answered, by verb and result.",
                           labels({{"verb", verb}, {"result", result}}));
        g_seconds[verb] = &r.histogram("chordfs_tracker_request_seconds",
                                       "From a request read to its reply: a write waits for commit and apply (D-075).",
                                       latency_buckets(), labels({{"verb", verb}}));
    }

    // Raft's state, read under the replica's lock at each scrape. A role is a gauge per
    // role, 1 for the one this replica holds -- so sum(role="leader") counts leaders.
    for (Raft::Role role : {Raft::Role::Follower, Raft::Role::Candidate, Raft::Role::Leader})
        r.gauge("chordfs_raft_role", "1 for the role this replica holds now.",
                [&rep, role] { return rep.view().role == role ? 1.0 : 0.0; }, labels({{"role", role_name(role)}}));
    r.gauge("chordfs_raft_term", "The current term.", [&rep] { return static_cast<double>(rep.view().term); });
    r.gauge("chordfs_raft_leader", "The leader this replica knows of, by position in the group; -1 for none.",
            [&rep] { const int l = rep.view().leader; return l == NOBODY ? -1.0 : static_cast<double>(l); });
    r.gauge("chordfs_raft_commit_index", "The highest log index known committed.",
            [&rep] { return static_cast<double>(rep.view().commit); });
    r.gauge("chordfs_raft_applied_index", "The highest log index applied to the namespace.",
            [&rep] { return static_cast<double>(rep.view().applied); });
    r.gauge("chordfs_raft_last_index", "The last index in this replica's log.",
            [&rep] { return static_cast<double>(rep.view().last); });
    r.gauge("chordfs_names", "Names in the namespace, as applied here.", [&rep] { return static_cast<double>(rep.view().names); });
    r.expose("chordfs_raft_elections_total", "Elections this replica started.", rep.elections);
    r.expose("chordfs_raft_leader_changes_total", "Times this replica learned of a different leader.", rep.leader_changes);
    g_flush_seconds = &r.histogram("chordfs_raft_flush_seconds",
                                   "One step's batch written and flushed (fdatasync) before any reply (D-074, D-082).",
                                   latency_buckets());
}

// A whole number in [lo, hi]; digits only.
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

int usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]\n"
                 "          [--group <ip:port>,<ip:port>,... --id <i>] [--election <ms>]\n"
                 "          [--metrics <port>] [--log-level debug|info|warn|error]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage(argv[0]);
    // D-011: a peer that hangs up mid-reply is an error value from send(), not a
    // signal that ends the process.
    std::signal(SIGPIPE, SIG_IGN);

    int port = 0, id = -1, election = 150;           // E: D-073's rule chose 150 ms (BENCHMARKS.md §7)
    int metrics_port = 0;
    Level level = Level::Info;
    if (!parse_int(argv[1], 1, 65535, port)) return usage(argv[0]);
    std::string dir = ".", ip = "0.0.0.0", group_text;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--dir" && has_value) dir = argv[++i];
        else if (a == "--ip" && has_value) ip = argv[++i];
        else if (a == "--read-deadline" && has_value) { if (!parse_int(argv[++i], 10, 600000, g_read_deadline_ms)) return usage(argv[0]); }
        else if (a == "--group" && has_value) group_text = argv[++i];
        else if (a == "--id" && has_value) { if (!parse_int(argv[++i], 0, 1000, id)) return usage(argv[0]); }
        else if (a == "--election" && has_value) { if (!parse_int(argv[++i], 20, 600000, election)) return usage(argv[0]); }
        else if (a == "--metrics" && has_value) { if (!parse_int(argv[++i], 1, 65535, metrics_port)) return usage(argv[0]); }
        else if (a == "--log-level" && has_value) { if (!parse_level(argv[++i], level)) return usage(argv[0]); }
        else return usage(argv[0]);
    }
    log_setup("tracker:" + std::to_string(port), level);
    const double started = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();

    // The group: given, or this replica alone.
    ReplicaConfig cfg;
    cfg.election = election;
    if (group_text.empty()) {
        if (id > 0) return usage(argv[0]);
        cfg.group = {Address{ip == "0.0.0.0" ? "127.0.0.1" : ip, port}};
        cfg.id = 0;
    } else {
        std::string err;
        if (!parse_address_list(group_text, cfg.group, err)) {
            log_event(Level::Error, "tracker.start", {{"msg", "--group: " + err}});
            return 2;
        }
        if (id < 0 || id >= static_cast<int>(cfg.group.size()) || cfg.group[id].port != port) {
            log_event(Level::Error, "tracker.start",
                      {{"msg", "--id must be this replica's position in --group, whose port must be " + std::to_string(port)}});
            return 2;
        }
        cfg.id = id;
    }

    // Recovery before listening: no client or replica can see a half-replayed log.
    FileStorage file;
    Durable recovered;
    std::string report, err;
    if (!file.open(dir + "/raft_" + std::to_string(port) + ".log", recovered, report, err)) {
        log_event(Level::Error, "tracker.start", {{"msg", err}});
        return 1;
    }
    log_event(Level::Info, "tracker.start", {{"msg", report}});
    TimedStorage storage(file);                       // every flush timed for the metrics

    // Every call to another replica is a fresh connection (D-071), bounded by the
    // call deadline the core passes in.
    const std::vector<Address> group = cfg.group;
    auto replica = std::make_unique<Replica>(cfg, storage, std::move(recovered),
        [group](int to, const std::string &line, const std::string &body, std::string &reply, int timeout_ms) {
            std::string reply_body;
            return exchange_frame(group[to].ip, group[to].port, line, body, reply, reply_body, timeout_ms);
        });
    g_replica = replica.get();
    register_metrics(*replica, started);
    if (metrics_port != 0) {
        std::string merr;
        if (!serve_metrics(ip, metrics_port, [] { return g_metrics.render(); }, merr)) {
            log_event(Level::Error, "tracker.start", {{"msg", merr}});
            return 1;
        }
    }

    const int listener = listen_on(ip, port);
    if (listener < 0) {
        log_event(Level::Error, "tracker.start",
                  {{"msg", "cannot listen on " + ip + ":" + std::to_string(port) + ": " + std::strerror(errno)}});
        return 1;
    }
    replica->start();
    log_event(Level::Info, "tracker.start",
              {{"replica", std::to_string(cfg.id)}, {"of", std::to_string(cfg.group.size())}, {"group", replica->fingerprint()},
               {"election_ms", std::to_string(election)}, {"listen", ip + ":" + std::to_string(port)}});

    while (true) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR) {
                log_event(Level::Error, "tracker.accept", {{"msg", std::strerror(errno)}});
                std::this_thread::sleep_for(std::chrono::milliseconds(10));      // EMFILE: do not spin
            }
            continue;
        }
        try {
            std::thread(serve, fd).detach();
        } catch (const std::system_error &e) {       // no thread for this one: drop it, keep serving
            log_event(Level::Error, "tracker.accept", {{"msg", std::string("cannot start a thread: ") + e.what()}});
            close(fd);
        }
    }
}
