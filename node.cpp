// node -- one member of a fixed Chord ring (Phase 2).
//
//   ./node <ip> <port> <membership-file>
//
// Answers one routing question, one step at a time (iterative routing, D-012):
//
//   FIND_SUCCESSOR <16-hex id>  ->  OWNER <id> <ip> <port>   my successor owns it
//                               ->  NEXT  <id> <ip> <port>   ask this node instead
//   PING                        ->  PONG <id>
//   anything else               ->  ERR <reason>
//
// In step 2.1c, NEXT always names the successor, so a lookup walks the ring in
// O(N) hops. Step 2.2 changes that one choice to the closest preceding finger.

#include "chord.h"
#include "net.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

// Everything this node knows. Written once by bootstrap(), before the first
// connection is accepted, and never again in Phase 2. That -- and only that --
// is why the handler threads may read it without a mutex. Phase 3 changes the
// successor and fingers while threads are reading them, and locking arrives then.
struct NodeState {
    Peer self;
    Peer predecessor;
    Peer successor;
    std::vector<Peer> fingers;    // built now so bootstrap is complete; routed on from step 2.2
};

NodeState node;

std::string render(const Peer &p) {
    return id_to_hex(p.id) + " " + p.ip + " " + std::to_string(p.port);
}

// Build this node's routing state from the membership file (decision D-020).
bool bootstrap(const std::string &ip, const std::string &port,
               const std::string &path, std::string &err) {
    std::ifstream file(path);
    if (!file) { err = "cannot open membership file " + path; return false; }
    std::stringstream text;
    text << file.rdbuf();

    // D-020: the whole membership lives only in this local variable. It is
    // destroyed when bootstrap returns, so no lookup can ever consult it.
    std::vector<Peer> members;
    if (!parse_members(text.str(), members, err)) return false;

    // Our own address goes through the same parser, so it is validated and
    // canonicalised exactly as the file's entries were.
    std::vector<Peer> me;
    std::string me_err;
    if (!parse_members(ip + ":" + port, me, me_err)) {
        err = "invalid own address " + ip + ":" + port;
        return false;
    }

    // Find ourselves by ADDRESS, never by identifier (invariant I8).
    bool listed = false;
    for (const Peer &p : members)
        if (p.ip == me[0].ip && p.port == me[0].port) listed = true;
    if (!listed) {
        err = me[0].ip + ":" + std::to_string(me[0].port) + " is not in " + path;
        return false;
    }

    node.self        = me[0];
    node.fingers     = build_fingers(node.self.id, members);
    node.successor   = node.fingers[0];                     // successor_of(self.id + 1)
    node.predecessor = predecessor_of(node.self.id, members);
    return true;
}

// Turn one request line into one reply line. Never throws, never exits.
std::string handle(const std::string &line) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);

    if (words.empty()) return "ERR empty request";

    if (words[0] == "PING" && words.size() == 1)
        return "PONG " + id_to_hex(node.self.id);

    if (words[0] == "FIND_SUCCESSOR" && words.size() == 2) {
        Id k;
        if (!id_from_hex(words[1], k))
            return "ERR identifier must be 16 hexadecimal characters";

        // D-021c: ownership is decided from the successor pointer alone.
        if (in_range_oc(k, node.self.id, node.successor.id))
            return "OWNER " + render(node.successor);
        return "NEXT " + render(node.successor);            // step 2.2: closest preceding finger
    }

    return "ERR unknown request";
}

// One connection: answer lines until the peer closes, a send fails, or a line
// is too long. Runs on its own detached thread.
void serve(int fd) {
    std::string line;
    while (recv_line(fd, line)) {
        if (!send_line(fd, handle(line))) break;
    }
    close(fd);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <ip> <port> <membership-file>\n", argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);      // D-011: a departed peer is an error value

    std::string err;
    if (!bootstrap(argv[1], argv[2], argv[3], err)) {
        std::fprintf(stderr, "[node] %s\n", err.c_str());
        return 1;
    }

    const int listener = listen_on(node.self.ip, node.self.port);
    if (listener < 0) {
        std::fprintf(stderr, "[node] cannot listen on %s:%d: %s\n",
                     node.self.ip.c_str(), node.self.port, std::strerror(errno));
        return 1;
    }

    std::fprintf(stderr, "[node] %s:%d id=%s pred=%s succ=%s\n",
                 node.self.ip.c_str(), node.self.port, id_to_hex(node.self.id).c_str(),
                 id_to_hex(node.predecessor.id).c_str(), id_to_hex(node.successor.id).c_str());

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
