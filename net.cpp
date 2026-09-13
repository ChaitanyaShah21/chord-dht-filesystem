#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

bool send_line(int fd, const std::string &line) {
    std::string out = line;

    // Drop any terminator the caller supplied; exactly one is appended below.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();

    std::size_t offenders = 0;
    for (char &c : out) {
        if (c == '\n' || c == '\r') { c = ' '; ++offenders; }
    }
    if (offenders > 0)
        std::fprintf(stderr, "[net] BUG: message carried %zu embedded newline(s); "
                             "replaced with spaces to preserve framing\n", offenders);
    out.push_back('\n');

    // send() may accept fewer bytes than asked -- a short write -- so loop.
    std::size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;       // interrupted before sending: retry
        if (n <= 0) return false;                    // peer gone (EPIPE) or socket error
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_line(int fd, std::string &out) {
    out.clear();
    char c;
    while (true) {
        // One byte per recv: simple and never reads past the newline, at the cost
        // of one system call per byte (defect D5 -- a latency cost, not a
        // correctness one, and it does not affect hop counts).
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;                    // 0: peer closed. <0: error.
        if (c == '\n') break;
        if (out.size() == MAX_LINE) return false;    // no newline in sight: drop it
        out.push_back(c);
    }
    if (!out.empty() && out.back() == '\r') out.pop_back();   // tolerate "\r\n"
    return true;
}

int connect_to(const std::string &ip, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, static_cast<sockaddr *>(static_cast<void *>(&addr)), sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int listen_on(const std::string &ip, int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { errno = EINVAL; return -1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Without SO_REUSEADDR a restarted node cannot rebind its port for up to a
    // minute while old connections sit in TIME_WAIT (defect B5).
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    if (bind(fd, static_cast<sockaddr *>(static_cast<void *>(&addr)), sizeof addr) < 0 ||
        listen(fd, SOMAXCONN) < 0) {
        const int saved = errno;      // close() may overwrite errno; keep the real cause
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
