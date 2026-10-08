// node -- one member of a Chord ring.
//
//   ./node <ip> <port> <membership-file>        fixed ring, never changes (Phase 2, D-020)
//   ./node <ip> <port> --create     [options]   a new ring of one
//   ./node <ip> <port> --join <ip:port> [opts]  join through a node already in a ring
//   ./node <ip> <port> --wired <file> [options] start from the fixed-ring state, then maintain it
//
// Options (D-032, D-039):
//   --period <ms>    time between maintenance rounds              (default DEFAULT_PERIOD_MS)
//   --timeout <ms>   deadline on every call to another node       (default DEFAULT_TIMEOUT_MS)
//   --succ <r>       successor-list length, 1..16                 (default 8)
//
// Requests are answered by handle_request() in stabilize.cpp, shared with the
// in-process tests, so what the tests prove is what the node does. This file
// is only sockets, threads and the command line.

#include "chord.h"
#include "lookup.h"
#include "net.h"
#include "stabilize.h"

#include <sys/socket.h>
#include <unistd.h>

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

// Set once in main() before any thread starts, and only read afterwards. That
// ordering -- all writes before the first reader exists -- is what makes them
// safe to read without a lock.
int g_timeout_ms = DEFAULT_TIMEOUT_MS;
std::unique_ptr<NodeState> g_node;

// Every call this node makes to another node: one fresh connection, with the
// deadline set before connect (D-025). A plain function, so it lives forever and
// can be referred to from any thread.
Delivery node_transport(const Peer &to, const std::string &request, std::string &reply) {
    return exchange_line(to.ip, to.port, request, reply, g_timeout_ms);
}
const Transport g_transport = node_transport;

// One connection: answer lines until the peer closes, a send fails, or a line
// is too long. Runs on its own detached thread. Work a handler deferred until
// after its reply (Handled::after) runs once the reply has been sent.
void serve(int fd) {
    std::string line;
    while (recv_line(fd, line)) {
        Handled h = handle_request(*g_node, line, g_transport);
        const bool sent = send_line(fd, h.reply);
        if (h.after) h.after();
        if (!sent) break;
    }
    close(fd);
}

// The maintenance thread: a round, then a fixed delay (D-039). The first round
// waits a random fraction of a period, so nodes started together do not run
// their rounds in step.
void maintain(int period_ms) {
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int> offset(0, period_ms - 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(offset(rng)));
    while (true) {
        maintenance_round(*g_node, g_transport);
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

int usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s <ip> <port> <membership-file>\n"
                 "       %s <ip> <port> (--create | --join <ip:port> | --wired <file>)\n"
                 "                      [--period <ms>] [--timeout <ms>] [--succ <1..%d>]\n",
                 argv0, argv0, MAX_SUCCESSORS);
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
    std::string file, known_text;
    int period_ms = DEFAULT_PERIOD_MS, successors = DEFAULT_SUCCESSORS;
    if (argc == 4 && argv[3][0] != '-') {
        file = argv[3];                                   // Phase 2's form, unchanged
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
            else return usage(argv[0]);
        }
        if (!chosen) return usage(argv[0]);
    }

    g_node = std::make_unique<NodeState>(me, successors);
    const std::string tag = "[node " + me.ip + ":" + std::to_string(me.port) + "] ";
    // Every event carries the wall-clock time in milliseconds, so the logs of many
    // nodes -- and a machine-wide record such as `vmstat` -- can be put on one
    // timeline when a failure has to be explained after the fact.
    g_node->log = [tag](const std::string &event) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        std::fprintf(stderr, "%s%lld.%03lld %s\n", tag.c_str(), ms / 1000, ms % 1000, event.c_str());
    };

    // ---- the starting state (D-029) ----
    if (mode == Mode::Fixed || mode == Mode::Wired) {
        std::vector<Peer> members;               // dies at the end of this block (D-020)
        std::string err;
        if (!read_members(file, members, err)) {
            std::fprintf(stderr, "[node] %s\n", err.c_str());
            return 1;
        }
        bool listed = false;
        for (const Peer &p : members) listed = listed || same_node(p, me);   // by address (I8)
        if (!listed) {
            std::fprintf(stderr, "[node] %s:%d is not in %s\n", me.ip.c_str(), me.port, file.c_str());
            return 1;
        }
        init_wired(*g_node, members);
    } else if (mode == Mode::Create) {
        init_alone(*g_node);
    } else {
        Peer known;
        if (!parse_address(known_text, known)) {
            std::fprintf(stderr, "[node] invalid --join address %s\n", known_text.c_str());
            return 2;
        }
        // Not listening yet, on purpose (D-029): until the join succeeds, a
        // connection to this address is refused, which evicts any stale entry.
        bool joined = false;
        for (int attempt = 1; attempt <= JOIN_ATTEMPTS && !joined; ++attempt) {
            std::string why;
            const JoinStatus st = join(*g_node, known, g_transport, why);
            if (st == JoinStatus::Joined) { joined = true; break; }
            std::fprintf(stderr, "%sjoin attempt %d/%d: %s\n", tag.c_str(), attempt, JOIN_ATTEMPTS, why.c_str());
            if (st == JoinStatus::Collision) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        }
        if (!joined) {
            std::fprintf(stderr, "%sgiving up: could not join through %s\n", tag.c_str(), known_text.c_str());
            return 1;
        }
    }

    const int listener = listen_on(me.ip, me.port);
    if (listener < 0) {
        std::fprintf(stderr, "[node] cannot listen on %s:%d: %s\n", me.ip.c_str(), me.port,
                     std::strerror(errno));
        return 1;
    }

    std::string pred_text = "none", succ_text;
    {
        std::lock_guard<std::mutex> lock(g_node->mu);     // copy under the lock, print after it
        if (g_node->has_pred) pred_text = id_to_hex(g_node->pred.id);
        succ_text = id_to_hex(g_node->succs[0].id);
    }
    std::fprintf(stderr, "%sid=%s pred=%s succ=%s\n", tag.c_str(), id_to_hex(me.id).c_str(),
                 pred_text.c_str(), succ_text.c_str());

    // The fixed ring has no maintenance thread: its tables never change (D-029).
    if (mode != Mode::Fixed) {
        try {
            std::thread(maintain, period_ms).detach();
        } catch (const std::system_error &e) {
            std::fprintf(stderr, "[node] cannot start the maintenance thread: %s\n", e.what());
            return 1;
        }
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
