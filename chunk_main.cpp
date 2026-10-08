// chunk -- write and read chunks through a ring (Phase 4; decisions D-046, D-049).
//
//   ./chunk put <ip:port> [--w <1..3>] < file        write the file as one chunk
//   ./chunk get <ip:port> <key> > file               read a chunk; its bytes go to stdout
//   ./chunk batch <ip:port>  < commands              many operations, one result line each
//
// <ip:port> is any member of the ring to start lookups from. `put` prints the
// chunk's key and how many copies were confirmed. `get` writes the chunk's bytes,
// already checked against the key, to standard output. Both describe every
// replica tried on standard error.
//
// `batch` reads one command per line, so a benchmark does not start a process per
// operation, and prints one line per command, flushed as it is printed:
//
//   put <w> <file>                  -> put <key> ok <copies> <microseconds> <coordinator> attempts <a>
//   putrand <w> <bytes> <seed>      -> the same, for <bytes> pseudo-random bytes made from <seed>
//   get <key>                       -> get <key> ok <microseconds> <served-by> tried <n>
//   keyrand <bytes> <seed>          -> keyrand <key> ok -   (the key putrand would write; nothing is sent)
//   any failure                     -> <verb> <key or -> fail <microseconds> : <message>
//
// The microseconds are the whole operation as the client sees it: the lookup, the
// transfer and, for a put, the wait for <w> copies.
//
// Exit status: 0 if every operation succeeded, 1 if any failed, 2 on a usage error.

#include "chord.h"
#include "chunk.h"
#include "lookup.h"
#include "net.h"
#include "store.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int CLIENT_TIMEOUT_MS = 5000;     // a hang guard, as for lookups (D-024)

Delivery client_frames(const Peer &to, const std::string &line, const std::string &body, Frame &reply) {
    return exchange_frame(to.ip, to.port, line, body, reply.line, reply.body, CLIENT_TIMEOUT_MS);
}

std::string where(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

long long micros_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

// <bytes> pseudo-random bytes from <seed>: the same seed always gives the same chunk.
std::string generated(std::size_t bytes, unsigned long long seed) {
    std::mt19937_64 rng(seed);
    std::string s(bytes, '\0');
    for (char &c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}

bool read_file(std::istream &in, std::string &out) {
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return out.size() <= MAX_CHUNK;
}

std::string put_line(const std::string &body, int w, const Peer &start) {
    const auto t0 = std::chrono::steady_clock::now();
    const PutOutcome p = put_chunk(start, body, w, socket_transport, client_frames);
    const long long us = micros_since(t0);
    if (p.ok)
        return "put " + chunk_key(body) + " ok " + std::to_string(p.copies) + " " + std::to_string(us) + " " +
               where(p.coordinator) + " attempts " + std::to_string(p.attempts);
    return "put " + chunk_key(body) + " fail " + std::to_string(us) + " : " + p.message;
}

std::string get_line(const std::string &key, const Peer &start) {
    const auto t0 = std::chrono::steady_clock::now();
    const GetOutcome g = get_chunk(start, key, socket_transport, client_frames);
    const long long us = micros_since(t0);
    if (g.ok)
        return "get " + key + " ok " + std::to_string(us) + " " + where(g.served_by) + " tried " +
               std::to_string(g.tried.size());
    return "get " + key + " fail " + std::to_string(us) + " : " + g.message;
}

int usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s put <ip:port> [--w <1..3>] < file\n"
                 "       %s get <ip:port> <key> > file\n"
                 "       %s batch <ip:port> < commands\n", argv0, argv0, argv0);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) return usage(argv[0]);
    std::signal(SIGPIPE, SIG_IGN);      // D-011
    const std::string mode = argv[1];

    std::vector<Peer> start;
    std::string err;
    if (!parse_members(argv[2], start, err) || start.size() != 1) {
        std::fprintf(stderr, "chunk: invalid start node %s\n", argv[2]);
        return 2;
    }

    if (mode == "put" && (argc == 3 || (argc == 5 && std::string(argv[3]) == "--w"))) {
        int w = DEFAULT_W;
        if (argc == 5) {
            const std::string t = argv[4];
            if (t.size() != 1 || t[0] < '1' || t[0] > '3') return usage(argv[0]);
            w = t[0] - '0';
        }
        std::string body;
        if (!read_file(std::cin, body)) { std::fprintf(stderr, "chunk: input is larger than %zu bytes\n", MAX_CHUNK); return 1; }
        const std::string out = put_line(body, w, start[0]);
        std::printf("%s\n", out.c_str());
        return out.find(" ok ") != std::string::npos ? 0 : 1;
    }

    if (mode == "get" && argc == 4) {
        const GetOutcome g = get_chunk(start[0], argv[3], socket_transport, client_frames);
        for (const std::string &t : g.tried) std::fprintf(stderr, "chunk: %s\n", t.c_str());
        if (!g.ok) { std::fprintf(stderr, "chunk: %s\n", g.message.c_str()); return 1; }
        std::fwrite(g.body.data(), 1, g.body.size(), stdout);
        return 0;
    }

    if (mode == "batch" && argc == 3) {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);    // flush each line as it is printed
        bool any_failed = false;
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream in(line);
            std::string verb;
            if (!(in >> verb)) continue;
            std::string out;
            if (verb == "put") {
                int w;
                std::string path;
                std::ifstream f;
                std::string body;
                if (in >> w >> path && w >= 1 && w <= 3 && (f.open(path, std::ios::binary), f) && read_file(f, body))
                    out = put_line(body, w, start[0]);
                else out = "put - fail 0 : bad command or unreadable file";
            } else if (verb == "putrand") {
                int w;
                std::size_t bytes;
                unsigned long long seed;
                if (in >> w >> bytes >> seed && w >= 1 && w <= 3 && bytes <= MAX_CHUNK)
                    out = put_line(generated(bytes, seed), w, start[0]);
                else out = "putrand - fail 0 : bad command";
            } else if (verb == "keyrand") {
                std::size_t bytes;
                unsigned long long seed;
                if (in >> bytes >> seed && bytes <= MAX_CHUNK) out = "keyrand " + chunk_key(generated(bytes, seed)) + " ok -";
                else out = "keyrand - fail 0 : bad command";
            } else if (verb == "get") {
                std::string key;
                if (in >> key) out = get_line(key, start[0]);
                else out = "get - fail 0 : bad command";
            } else {
                out = verb + " - fail 0 : unknown command";
            }
            any_failed = any_failed || out.find(" ok ") == std::string::npos;
            std::printf("%s\n", out.c_str());
        }
        return any_failed ? 1 : 0;
    }

    return usage(argv[0]);
}
