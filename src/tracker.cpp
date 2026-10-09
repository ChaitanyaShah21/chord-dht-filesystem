// tracker -- the namespace, replicated by Raft: file name -> manifest key, and nothing else
// (D-017, D-058, D-069 to D-082).
//
//   ./tracker <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]
//                    [--group <ip:port>,<ip:port>,... --id <i>] [--election <ms>]
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
// the state machine names.cpp, the framing net.cpp. This file is only sockets and the
// command line.

#include "group.h"
#include "net.h"
#include "raft_log.h"
#include "replica.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
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

// One connection: answer requests until the peer closes, a send fails, a line is
// too long or a body malformed (the stream cannot be trusted after either), or the
// next line or body does not arrive in time. Runs on its own detached thread.
void serve(int fd) {
    std::string line, body;
    while (recv_line_by(fd, line, deadline_in(g_read_deadline_ms))) {
        body.clear();
        std::size_t length = 0;
        const Body b = body_length(line, length);       // only APPEND carries one here
        if (b == Body::Malformed || b == Body::TooLarge) {
            send_line(fd, b == Body::Malformed ? "ERR malformed body length" : "ERR body too large");
            break;
        }
        if (b == Body::Present && !recv_exact_by(fd, length, body, deadline_in(g_read_deadline_ms))) break;
        if (!send_line(fd, g_replica->handle(line, body))) break;
    }
    close(fd);
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
                 "          [--group <ip:port>,<ip:port>,... --id <i>] [--election <ms>]\n",
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
        else return usage(argv[0]);
    }

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
            std::fprintf(stderr, "[tracker] --group: %s\n", err.c_str());
            return 2;
        }
        if (id < 0 || id >= static_cast<int>(cfg.group.size()) || cfg.group[id].port != port) {
            std::fprintf(stderr, "[tracker] --id must be this replica's position in --group, whose port must be %d\n", port);
            return 2;
        }
        cfg.id = id;
    }

    // Recovery before listening: no client or replica can see a half-replayed log.
    FileStorage storage;
    Durable recovered;
    std::string report, err;
    if (!storage.open(dir + "/raft_" + std::to_string(port) + ".log", recovered, report, err)) {
        std::fprintf(stderr, "[tracker] %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[tracker] %s\n", report.c_str());

    // Every call to another replica is a fresh connection (D-071), bounded by the
    // call deadline the core passes in.
    const std::vector<Address> group = cfg.group;
    auto replica = std::make_unique<Replica>(cfg, storage, std::move(recovered),
        [group](int to, const std::string &line, const std::string &body, std::string &reply, int timeout_ms) {
            std::string reply_body;
            return exchange_frame(group[to].ip, group[to].port, line, body, reply, reply_body, timeout_ms);
        });
    g_replica = replica.get();

    const int listener = listen_on(ip, port);
    if (listener < 0) {
        std::fprintf(stderr, "[tracker] cannot listen on %s:%d: %s\n", ip.c_str(), port, std::strerror(errno));
        return 1;
    }
    replica->start();
    std::fprintf(stderr, "[tracker] replica %d of %zu (group %s), election timeout %d ms, listening on %s:%d\n", cfg.id,
                 cfg.group.size(), replica->fingerprint().c_str(), election, ip.c_str(), port);

    while (true) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR) std::fprintf(stderr, "[tracker] accept: %s\n", std::strerror(errno));
            continue;
        }
        try {
            std::thread(serve, fd).detach();
        } catch (const std::system_error &e) {       // no thread for this one: drop it, keep serving
            std::fprintf(stderr, "[tracker] cannot start a thread: %s\n", e.what());
            close(fd);
        }
    }
}
