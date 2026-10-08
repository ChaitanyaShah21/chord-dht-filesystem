// tracker -- the namespace: file name -> manifest key, and nothing else (D-017, D-058).
//
//   ./tracker <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]
//
//   REGISTER <name> <manifest-key>  -> OK | EXISTS <key> | ERR <reason>
//   RESOLVE <name>                  -> FILE <key> | MISSING | ERR <reason>
//   LIST [<after>]                  -> NAMES <n> <name> ...
//   PING                            -> PONG
//
// The namespace and its log are names.cpp; the framing is net.cpp, the same code
// the node uses (D-060). This file is only sockets, threads and the command line.
// The log is <dir>/names_<port>.log (default: the current directory).
//
// The tracker never sees a chunk, a node address or a user. It replaced, in Phase 5,
// a coursework tracker with users, groups and seeder lists; that code is in the
// history (D-004, D-060).

#include "names.h"
#include "net.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>

namespace {

// A request line must arrive whole within this (D-061). The client sends its
// request as soon as it has connected, so an idle connection is either broken or
// hostile.
constexpr int DEFAULT_READ_DEADLINE_MS = 10000;

// Set once in main() before any thread starts, only read afterwards.
Namespace g_names;
int g_read_deadline_ms = DEFAULT_READ_DEADLINE_MS;

// One connection: answer requests until the peer closes, a send fails, a line is
// too long (MAX_LINE: the stream cannot be trusted after it), or the next line
// does not arrive in time. Runs on its own detached thread.
void serve(int fd) {
    std::string line;
    while (recv_line_by(fd, line, deadline_in(g_read_deadline_ms))) {
        if (!send_line(fd, handle_names_request(g_names, line))) break;
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
    std::fprintf(stderr, "usage: %s <port> [--dir <dir>] [--ip <ip>] [--read-deadline <ms>]\n", argv0);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage(argv[0]);
    // D-011: a client that hangs up mid-reply is an error value from send(), not a
    // signal that ends the process -- and with it every other client's session.
    std::signal(SIGPIPE, SIG_IGN);

    int port = 0;
    if (!parse_int(argv[1], 1, 65535, port)) return usage(argv[0]);
    std::string dir = ".", ip = "0.0.0.0";        // every interface, as the coursework tracker did
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--dir" && has_value) dir = argv[++i];
        else if (a == "--ip" && has_value) ip = argv[++i];
        else if (a == "--read-deadline" && has_value) { if (!parse_int(argv[++i], 10, 600000, g_read_deadline_ms)) return usage(argv[0]); }
        else return usage(argv[0]);
    }

    // Recovery before listening: no client can see a half-replayed namespace.
    std::string report, err;
    if (!names_open(g_names, dir + "/names_" + std::to_string(port) + ".log", report, err)) {
        std::fprintf(stderr, "[tracker] %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "[tracker] %s\n", report.c_str());

    const int listener = listen_on(ip, port);
    if (listener < 0) {
        std::fprintf(stderr, "[tracker] cannot listen on %s:%d: %s\n", ip.c_str(), port, std::strerror(errno));
        return 1;
    }
    std::fprintf(stderr, "[tracker] listening on %s:%d\n", ip.c_str(), port);

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
