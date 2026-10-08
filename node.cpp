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
//
// Ring requests are answered by handle_request() in stabilize.cpp and chunk
// requests by handle_chunk_request() in chunk.cpp, both shared with the
// in-process tests, so what the tests prove is what the node does. This file is
// only sockets, threads and the command line.

#include "chord.h"
#include "chunk.h"
#include "lookup.h"
#include "net.h"
#include "repair.h"
#include "stabilize.h"
#include "store.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
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

// Set once in main() before any thread starts, and only read afterwards. That
// ordering -- all writes before the first reader exists -- is what makes them
// safe to read without a lock.
int g_timeout_ms = DEFAULT_TIMEOUT_MS;
std::vector<std::unique_ptr<NodeState>> g_members;     // this process's ring members (D-047)
ChunkStore g_store;
ChunkNode  g_chunks;

// Every call this node makes to a ring member: one fresh connection, with the
// deadline set before connect (D-025), addressed to that member (D-047). A plain
// function, so it lives forever and can be referred to from any thread.
Delivery node_transport(const Peer &to, const std::string &request, std::string &reply) {
    return exchange_line(to.ip, to.port, addressed(to, request), reply, g_timeout_ms);
}
const Transport g_transport = node_transport;

// Every chunk call this node makes to another process (D-044): a header line, a
// body if the line declares one, and the same deadline on each system call.
Delivery frame_transport(const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
    return exchange_frame(to.ip, to.port, line, body, reply.line, reply.body, g_timeout_ms);
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
    std::size_t length = 0;
    const Body b = body_length(line, length);
    if (b == Body::Malformed) { send_line(fd, "ERR malformed body length"); return false; }
    if (b == Body::TooLarge)  { send_line(fd, "ERR chunk larger than " + std::to_string(MAX_BODY) + " bytes"); return false; }

    // Take a body slot BEFORE reading the body, so a peer cannot make this node
    // allocate more than MAX_BODIES chunks at once (D-052).
    std::shared_ptr<void> hold;
    std::string body;
    if (b == Body::Present) {
        hold = hold_body(g_chunks);
        if (!hold) {
            // Refuse it without holding it: read the body through a small buffer and
            // drop it, so the answer is not lost. Closing with the body unread would
            // make the kernel reset the connection, which can destroy "ERR busy"
            // before the sender reads it.
            return recv_discard(fd, length) && send_line(fd, "ERR busy");
        }
        if (!recv_exact(fd, length, body)) return false;        // closed or timed out mid-body
    }

    ChunkHandled h = handle_chunk_request(g_chunks, Frame{line, std::move(body)});
    const bool sent = send_line(fd, h.reply) && send_all(fd, h.body.data(), h.body.size());
    if (h.after) h.after();                    // a coordinator's remaining copies
    return sent;
}                                              // `hold` and h.hold released here

// One connection: answer requests until the peer closes, a send fails, or a line
// is too long. Runs on its own detached thread. Work a handler deferred until
// after its reply (Handled::after) runs once the reply has been sent.
void serve(int fd) {
    std::string line;
    while (recv_line(fd, line)) {
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
        const bool sent = send_line(fd, h.reply);
        if (h.after) h.after();
        if (!sent) break;
    }
    close(fd);
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
        repair_round(g_chunks, state, g_transport, scan_rounds);
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

int usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s <ip> <port> <membership-file>\n"
                 "       %s <ip> <port> (--create | --join <ip:port> | --wired <file>)\n"
                 "                      [--period <ms>] [--timeout <ms>] [--succ <1..%d>]\n"
                 "                      [--vnodes <1..%d>] [--store <dir>] [--scan <rounds>]\n",
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
    int vnodes = DEFAULT_VNODES, scan_rounds = DEFAULT_SCAN_ROUNDS;
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
            else if (a == "--scan" && has_value)    { if (!parse_int(argv[++i], 0, 1000000, scan_rounds)) return usage(argv[0]); }
            else if (a == "--store" && has_value && store_dir.empty()) { store_dir = argv[++i]; if (store_dir.empty()) return usage(argv[0]); }
            else return usage(argv[0]);
        }
        if (!chosen) return usage(argv[0]);
    }

    const std::string tag = "[node " + me.ip + ":" + std::to_string(me.port) + "] ";
    // Every event carries the wall-clock time in milliseconds, so the logs of many
    // nodes -- and a machine-wide record such as `vmstat` -- can be put on one
    // timeline when a failure has to be explained after the fact.
    auto log = [tag](const std::string &event) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        std::fprintf(stderr, "%s%lld.%03lld %s\n", tag.c_str(), ms / 1000, ms % 1000, event.c_str());
    };

    // ---- the members (D-047) ----
    for (int i = 0; i < vnodes; ++i) {
        const Peer member{id_of_member(me.ip, me.port, i), me.ip, me.port};
        for (const auto &m : g_members) {
            if (m->self.id == member.id) {
                std::fprintf(stderr, "%smembers %d and another share identifier %s; use fewer --vnodes\n",
                             tag.c_str(), i, id_to_hex(member.id).c_str());
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
            std::fprintf(stderr, "[node] %s\n", err.c_str());
            return 1;
        }
        bool listed = false;
        for (const Peer &p : processes) listed = listed || same_process(p, me);
        if (!listed) {
            std::fprintf(stderr, "[node] %s:%d is not in %s\n", me.ip.c_str(), me.port, file.c_str());
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
            std::fprintf(stderr, "[node] invalid --join address %s\n", known_text.c_str());
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
                std::fprintf(stderr, "%sjoin attempt %d/%d for %s: %s\n", tag.c_str(), attempt, JOIN_ATTEMPTS,
                             id_to_hex(m->self.id).c_str(), why.c_str());
                if (st == JoinStatus::Collision) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
            }
            if (!joined) {
                std::fprintf(stderr, "%sgiving up: could not join through %s\n", tag.c_str(), known_text.c_str());
                return 1;
            }
        }
    }

    // ---- the chunk store (D-045) ----
    g_chunks.send = frame_transport;
    g_chunks.log = log;
    for (const auto &m : g_members) g_chunks.members.push_back(m.get());
    if (!store_dir.empty()) {
        std::string report, err;
        if (!store_open(g_store, store_dir, report, err)) {
            std::fprintf(stderr, "%s%s\n", tag.c_str(), err.c_str());
            return 1;
        }
        g_chunks.store = &g_store;
        std::fprintf(stderr, "%sstore %s: %s\n", tag.c_str(), store_dir.c_str(), report.c_str());
    }

    const int listener = listen_on(me.ip, me.port);
    if (listener < 0) {
        std::fprintf(stderr, "[node] cannot listen on %s:%d: %s\n", me.ip.c_str(), me.port,
                     std::strerror(errno));
        return 1;
    }

    for (const auto &m : g_members) {
        std::string pred_text = "none", succ_text;
        {
            std::lock_guard<std::mutex> lock(m->mu);     // copy under the lock, print after it
            if (m->has_pred) pred_text = id_to_hex(m->pred.id);
            succ_text = id_to_hex(m->succs[0].id);
        }
        std::fprintf(stderr, "%sid=%s pred=%s succ=%s\n", tag.c_str(), id_to_hex(m->self.id).c_str(),
                     pred_text.c_str(), succ_text.c_str());
    }

    // The fixed ring has no maintenance thread: its tables never change (D-029).
    // Repair runs only where there is a store, and a ring that changes.
    try {
        if (mode != Mode::Fixed) std::thread(maintain, period_ms).detach();
        if (mode != Mode::Fixed && g_chunks.store) std::thread(repair, period_ms, scan_rounds).detach();
    } catch (const std::system_error &e) {
        std::fprintf(stderr, "[node] cannot start a background thread: %s\n", e.what());
        return 1;
    }

    while (true) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR)
                std::fprintf(stderr, "[node] accept: %s\n", std::strerror(errno));
            continue;
        }
        // std::thread's constructor throws if the system cannot create a thread.
        // Uncaught, that would terminate the whole node over one connection.
        try {
            std::thread(serve, fd).detach();
        } catch (const std::system_error &e) {
            std::fprintf(stderr, "[node] cannot start a thread: %s\n", e.what());
            close(fd);
        }
    }
}
