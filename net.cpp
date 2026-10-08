#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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
        // of one system call per byte -- a latency cost, not a correctness one,
        // and it does not affect hop counts.
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { errno = 0; return false; }     // the peer closed: not an error code
        if (n < 0) return false;                     // errno says why (EAGAIN: timed out)
        if (c == '\n') break;
        if (out.size() == MAX_LINE) {                // no newline in sight: drop it
            errno = EMSGSIZE;
            return false;
        }
        out.push_back(c);
    }
    if (!out.empty() && out.back() == '\r') out.pop_back();   // tolerate "\r\n"
    return true;
}

int connect_to(const std::string &ip, int port, int timeout_ms) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { errno = EINVAL; return -1; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Set the deadlines BEFORE connect (D-025): on Linux the send timeout also
    // bounds the handshake. A timeval is seconds plus MICROseconds.
    if (timeout_ms > 0) {
        timeval tv{};
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) {
            const int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }

    if (connect(fd, static_cast<sockaddr *>(static_cast<void *>(&addr)), sizeof addr) < 0) {
        const int saved = errno;      // close() may overwrite errno; the caller needs the
        close(fd);                    // real cause, because ECONNREFUSED is proof of death
        errno = saved;                // and a timeout is not (D-013)
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

const char *delivery_name(Delivery d) {
    switch (d) {
        case Delivery::Ok:      return "ok";
        case Delivery::Refused: return "refused";
        case Delivery::Timeout: return "timeout";
        case Delivery::Closed:  return "closed";
        case Delivery::Failed:  return "failed";
    }
    return "failed";
}

namespace {

// Name the failure of a send_line or recv_line from the errno it left behind.
Delivery classify_io_error(int err) {
    if (err == EAGAIN || err == EWOULDBLOCK) return Delivery::Timeout;   // SO_RCVTIMEO/SNDTIMEO fired
    if (err == 0 || err == ECONNRESET || err == EPIPE) return Delivery::Closed;
    return Delivery::Failed;                                            // includes EMSGSIZE
}

}  // namespace

Body body_length(const std::string &line, std::size_t &length) {
    const std::size_t first_space = line.find(' ');
    const std::string verb = line.substr(0, first_space);
    if (verb != "PUT" && verb != "STORE" && verb != "CHUNK") return Body::None;
    if (first_space == std::string::npos) return Body::Malformed;      // "CHUNK" alone

    const std::string word = line.substr(line.rfind(' ') + 1);        // the last word
    if (word.empty() || word.size() > 7) return Body::Malformed;
    if (word.size() > 1 && word[0] == '0') return Body::Malformed;     // "007" is not canonical
    std::size_t value = 0;
    for (char c : word) {
        if (c < '0' || c > '9') return Body::Malformed;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    if (value > MAX_BODY) return Body::TooLarge;
    length = value;
    return Body::Present;
}

bool send_all(int fd, const char *data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, std::size_t n, std::string &out) {
    out.resize(n);
    std::size_t got = 0;
    while (got < n) {
        // recv may return fewer bytes than asked -- a short read -- and on a real
        // network it routinely does. Ask again for exactly what is still missing.
        const ssize_t r = recv(fd, &out[got], n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { errno = 0; return false; }      // closed mid-body: a failed transfer
        if (r < 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool recv_discard(int fd, std::size_t n) {
    char scratch[4096];
    while (n > 0) {
        const ssize_t r = recv(fd, scratch, n < sizeof scratch ? n : sizeof scratch, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { errno = 0; return false; }
        if (r < 0) return false;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

Delivery exchange_frame(const std::string &ip, int port, const std::string &line,
                        const std::string &body, std::string &reply, std::string &reply_body,
                        int timeout_ms) {
    reply.clear();
    reply_body.clear();

    // A request that declares a different length than it carries is a bug in the
    // caller; sending it would desynchronise the stream, so it is never sent.
    std::size_t declared = 0;
    const Body b = body_length(line, declared);
    if ((b == Body::Present && declared != body.size()) || b == Body::Malformed ||
        b == Body::TooLarge || (b == Body::None && !body.empty()))
        return Delivery::Failed;

    const int fd = connect_to(ip, port, timeout_ms);
    if (fd < 0) {
        if (errno == ECONNREFUSED) return Delivery::Refused;
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
            return Delivery::Timeout;
        return Delivery::Failed;
    }

    Delivery result = Delivery::Ok;
    if (!send_line(fd, line) || !send_all(fd, body.data(), body.size()) || !recv_line(fd, reply)) {
        result = classify_io_error(errno);
    } else {
        std::size_t n = 0;
        const Body rb = body_length(reply, n);
        if (rb == Body::Malformed || rb == Body::TooLarge) result = Delivery::Failed;
        else if (rb == Body::Present && !recv_exact(fd, n, reply_body)) result = classify_io_error(errno);
    }
    if (result != Delivery::Ok) { reply.clear(); reply_body.clear(); }
    close(fd);
    return result;
}

Delivery exchange_line(const std::string &ip, int port, const std::string &request,
                       std::string &reply, int timeout_ms) {
    reply.clear();

    const int fd = connect_to(ip, port, timeout_ms);
    if (fd < 0) {
        if (errno == ECONNREFUSED) return Delivery::Refused;
        // EINPROGRESS here does NOT mean "a non-blocking connect has started":
        // this socket is blocking, and Linux reports an expired SO_SNDTIMEO on
        // connect this way (D-025). ETIMEDOUT is the kernel's own SYN retries
        // running out, which only happens if no timeout was set.
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
            return Delivery::Timeout;
        return Delivery::Failed;
    }

    Delivery result = Delivery::Ok;
    if (!send_line(fd, request) || !recv_line(fd, reply)) {
        result = classify_io_error(errno);     // read errno before close() can change it
        reply.clear();                         // a partial line is not a reply
    }
    close(fd);
    return result;
}
