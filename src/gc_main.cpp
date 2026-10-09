// gc -- collect the chunks no name uses any more (decisions D-090 to D-092).
//
//   ./gc --tracker <ip:port>[,...] --ring <ip:port>[,...] --gc-key <file> [options]
//
// Options:
//   --dry-run                  mark and list; report what would be dropped; drop nothing
//   --deadline <s>             send no DROP this long after the names were first listed (default 1800, D-091)
//   --assume-upload-limit <s>  the clients' upload limit; a node whose grace is shorter than this plus
//                              the tracker wait plus the deadline is not swept (default 3600)
//   --tracker-wait <ms>        the clients' tracker wait, which is also this program's (default 10000, D-081)
//   --request-id <id>          16 lowercase hex characters carried on every request (D-087)
//   --log <file>               structured log lines (D-086)
//   --quiet                    no per-node lines on standard error
//
// Prints one line on standard output, for scripts:
//   gc ok names <n> roots <n> live <n> nodes <n> skipped <n> held <n> garbage <n> dropped <n> bytes <n> mark_ms <t> sweep_ms <t>
// Exit status: 0 when the collection ran (whatever it dropped), 1 when it stopped before
// dropping anything (the mark failed), 2 on a usage error.
//
// The collection is collect.cpp; this file is the command line and the real network.

#include "collect.h"
#include "group.h"
#include "log.h"
#include "names.h"
#include "net.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int CALL_MS = 5000;          // a hang guard on every exchange, as the client's

using Clock = std::chrono::steady_clock;

double seconds_now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

bool parse_list(const std::string &text, std::vector<Peer> &out) {
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        std::vector<Peer> one;
        std::string err;
        if (item.empty() || !parse_members(item, one, err) || one.size() != 1) return false;
        out.push_back(one[0]);
    }
    return !out.empty();
}

bool parse_seconds(const std::string &s, double &out) {
    if (s.empty() || s.size() > 7) return false;
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = static_cast<double>(v);
    return true;
}

// The key file: one line, a secret as owner secrets are (32 to 64 lowercase hex).
bool read_key(const std::string &path, std::string &key) {
    std::ifstream f(path);
    if (!f || !std::getline(f, key)) return false;
    while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
    return valid_secret(key);
}

int usage() {
    std::fprintf(stderr,
                 "usage: gc --tracker <ip:port>[,...] --ring <ip:port>[,...] --gc-key <file>\n"
                 "          [--dry-run] [--deadline <s>] [--assume-upload-limit <s>] [--tracker-wait <ms>]\n"
                 "          [--request-id <id>] [--log <file>] [--quiet]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);      // D-011

    GroupClient tracker;
    tracker.exchange = [](const Address &to, const std::string &request, std::string &reply) {
        return exchange_line(to.ip, to.port, with_request_id(request), reply, CALL_MS);
    };
    tracker.now = [] {
        return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    };
    tracker.sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

    std::vector<Peer> seeds;
    std::string key_file, request_id, log_path, err;
    CollectSettings s;
    bool have_tracker = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--tracker" && v)                   { if (!parse_address_list(argv[++i], tracker.replicas, err)) return usage(); have_tracker = true; }
        else if (a == "--ring" && v)                 { if (!parse_list(argv[++i], seeds)) return usage(); }
        else if (a == "--gc-key" && v)               { key_file = argv[++i]; }
        else if (a == "--dry-run")                   { s.dry_run = true; }
        else if (a == "--deadline" && v)             { if (!parse_seconds(argv[++i], s.deadline_s)) return usage(); }
        else if (a == "--assume-upload-limit" && v)  { if (!parse_seconds(argv[++i], s.upload_limit_s)) return usage(); }
        else if (a == "--tracker-wait" && v)         { double ms = 0; if (!parse_seconds(argv[++i], ms)) return usage(); tracker.wait_ms = static_cast<int>(ms); }
        else if (a == "--request-id" && v)           { request_id = argv[++i]; if (!valid_request_id(request_id)) return usage(); }
        else if (a == "--log" && v)                  { log_path = argv[++i]; }
        else if (a == "--quiet")                     { quiet = true; }
        else return usage();
    }
    if (!have_tracker || seeds.empty() || key_file.empty()) return usage();
    if (!read_key(key_file, s.gc_key)) {
        std::fprintf(stderr, "gc: %s must hold one key of 32 to 64 lowercase hexadecimal characters\n", key_file.c_str());
        return 2;
    }
    s.tracker_wait_s = tracker.wait_ms / 1000.0;

    int log_fd = -1;
    if (!log_path.empty() && (log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644)) < 0) {
        std::fprintf(stderr, "gc: cannot open %s: %s\n", log_path.c_str(), std::strerror(errno));
        return 2;
    }
    log_setup("gc", Level::Info, log_fd);
    set_process_request_id(request_id.empty() ? new_request_id() : request_id);

    // ---- the real network ----
    CollectIO io;
    io.entries = [&tracker](const std::string &after, std::vector<std::pair<std::string, std::string>> &page,
                            std::string &why) {
        std::string reply;
        if (!ask_group(tracker, after.empty() ? "ENTRIES" : "ENTRIES " + after, reply)) { why = reply; return false; }
        if (!parse_entries_reply(reply, page)) { why = "the tracker answered: " + reply.substr(0, 100); return false; }
        return true;
    };
    auto frames = [](const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
        return exchange_frame(to.ip, to.port, with_request_id(line), body, reply.line, reply.body, CALL_MS);
    };
    io.get = [&seeds, frames](const std::string &k) {
        GetOutcome g;
        for (const Peer &seed : seeds) {             // the next seed only if a lookup could not start
            g = get_chunk(seed, k, socket_transport, frames);
            if (g.ok || g.message.compare(0, 13, "lookup failed") != 0) break;
        }
        return g;
    };
    io.nodes = [&seeds](std::vector<Peer> &processes, std::string &why) {
        for (const Peer &seed : seeds)
            if (walk_ring(seed, socket_transport, processes, why)) return true;
        return false;
    };
    io.ask = [](const Peer &to, const std::string &line, std::string &reply) {
        return exchange_line(to.ip, to.port, with_request_id(line), reply, CALL_MS);
    };
    io.now = seconds_now;

    log_event(Level::Info, "gc.start", {{"dry_run", s.dry_run ? "1" : "0"}});
    const CollectReport r = collect(io, s);

    std::size_t skipped = 0, held = 0;
    for (const NodeSweep &n : r.nodes) {
        held += n.held;
        if (!n.skipped.empty()) ++skipped;
        if (!quiet)
            std::fprintf(stderr, "gc: %s grace %ld s: %zu held, %zu garbage, %zu dropped, %llu bytes%s%s\n", n.address.c_str(),
                         n.grace_s, n.held, n.garbage, n.dropped, static_cast<unsigned long long>(n.bytes),
                         n.skipped.empty() ? "" : " -- not swept: ", n.skipped.c_str());
        log_event(Level::Info, "gc.node", {{"node", n.address}, {"held", std::to_string(n.held)},
                                           {"garbage", std::to_string(n.garbage)}, {"dropped", std::to_string(n.dropped)},
                                           {"bytes", std::to_string(n.bytes)}, {"skipped", n.skipped}});
    }
    if (!r.message.empty()) std::fprintf(stderr, "gc: %s\n", r.message.c_str());
    if (!r.ok) {
        log_event(Level::Error, "gc.stopped", {{"msg", r.message}});
        return 1;
    }
    std::printf("gc ok names %zu roots %zu live %zu nodes %zu skipped %zu held %zu garbage %zu dropped %zu bytes %llu "
                "mark_ms %.1f sweep_ms %.1f%s\n",
                r.names, r.roots, r.live, r.nodes.size(), skipped, held, r.garbage, r.dropped,
                static_cast<unsigned long long>(r.bytes), r.mark_s * 1000, r.sweep_s * 1000, s.dry_run ? " dry-run" : "");
    log_event(Level::Info, "gc.done", {{"garbage", std::to_string(r.garbage)}, {"dropped", std::to_string(r.dropped)},
                                       {"bytes", std::to_string(r.bytes)}});
    return 0;
}
