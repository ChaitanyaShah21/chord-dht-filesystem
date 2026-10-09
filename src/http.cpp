#include "http.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

namespace {

const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
    }
    return "Error";
}

// Milliseconds left until `deadline`, rounded up; 0 or less once it has passed.
long long ms_left(Deadline deadline) {
    const auto left = deadline - std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(left + std::chrono::microseconds(999)).count();
}

// Wait until fd is readable (or closed) or the deadline passes. poll() is given
// the time remaining, never a fixed slice, so a peer sending a byte at a time
// cannot stretch the deadline (the same rule as net.cpp's readers, D-061).
bool readable_by(int fd, Deadline deadline) {
    while (true) {
        const long long ms = ms_left(deadline);
        if (ms <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, static_cast<int>(ms));
        if (r < 0 && errno == EINTR) continue;
        return r > 0;
    }
}

const char *const TEXT = "text/plain; charset=utf-8";

}  // namespace

HttpRequest parse_http_head(const std::string &head) {
    HttpRequest r;
    // The request line: everything before the first '\n', without a '\r' before it.
    std::string line = head.substr(0, head.find('\n'));
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Exactly three words, single spaces: METHOD PATH VERSION.
    const std::size_t a = line.find(' ');
    if (a == std::string::npos) return r;
    const std::size_t b = line.find(' ', a + 1);
    if (b == std::string::npos || line.find(' ', b + 1) != std::string::npos) return r;
    const std::string method = line.substr(0, a);
    const std::string target = line.substr(a + 1, b - a - 1);
    const std::string version = line.substr(b + 1);

    if (method.empty() || method.size() > 16) return r;
    for (char c : method)
        if (c < 'A' || c > 'Z') return r;
    if (target.empty() || target[0] != '/') return r;
    for (unsigned char c : target)
        if (c < 0x21 || c > 0x7e) return r;          // visible ASCII only: no control bytes
    if (version != "HTTP/1.0" && version != "HTTP/1.1") return r;

    r.ok = true;
    r.method = method;
    r.path = target.substr(0, target.find('?'));      // Prometheus may add a query; it means nothing here
    return r;
}

std::string http_response(int status, const std::string &content_type, const std::string &body) {
    return "HTTP/1.0 " + std::to_string(status) + " " + reason(status) + "\r\n" +
           "Content-Type: " + content_type + "\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           "Connection: close\r\n\r\n" + body;
}

std::string metrics_answer(const std::string &head, const std::function<std::string()> &render) {
    const HttpRequest r = parse_http_head(head);
    if (!r.ok) return http_response(400, TEXT, "bad request\n");
    if (r.method != "GET") return http_response(405, TEXT, "only GET\n");
    if (r.path != "/metrics") return http_response(404, TEXT, "only /metrics\n");
    // The version tells Prometheus which text format this is (0.0.4).
    return http_response(200, "text/plain; version=0.0.4; charset=utf-8", render());
}

HeadRead read_http_head(int fd, std::string &head, Deadline deadline) {
    head.clear();
    char buf[1024];
    while (true) {
        if (!readable_by(fd, deadline)) return HeadRead::Failed;
        // Never ask for more than the cap leaves room for, so `head` cannot grow past it.
        const std::size_t room = MAX_HTTP_HEAD - head.size();
        const ssize_t n = recv(fd, buf, room < sizeof buf ? room : sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return HeadRead::Failed;            // closed, or failed
        const std::size_t from = head.size() < 3 ? 0 : head.size() - 3;   // a terminator may straddle reads
        head.append(buf, static_cast<std::size_t>(n));
        const std::size_t crlf = head.find("\r\n\r\n", from), lf = head.find("\n\n", from);
        if (crlf != std::string::npos || lf != std::string::npos) return HeadRead::Complete;
        if (head.size() >= MAX_HTTP_HEAD) return HeadRead::TooLarge;
    }
}

void serve_metrics_connection(int fd, const std::function<std::string()> &render) {
    // A client that never reads must not hold this thread: bound every send.
    timeval tv{HTTP_DEADLINE_MS / 1000, (HTTP_DEADLINE_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::string head;
    const HeadRead got = read_http_head(fd, head, deadline_in(HTTP_DEADLINE_MS));
    if (got != HeadRead::Failed) {
        // An exception escaping this detached thread would end the whole process
        // (std::terminate); a scrape that fails is answered 500 instead.
        std::string answer;
        try {
            answer = got == HeadRead::TooLarge ? http_response(431, TEXT, "request head too large\n")
                                               : metrics_answer(head, render);
        } catch (...) {
            answer = http_response(500, TEXT, "could not render the metrics\n");
        }
        send_all(fd, answer.data(), answer.size());
        // Close gracefully. If the peer sent more than was read (a body, a second
        // request), close() with unread data makes the kernel send a reset, which
        // can destroy the response before the peer reads it. So: say "no more from
        // me" (shutdown), read and drop what is left, briefly, and only then close.
        shutdown(fd, SHUT_WR);
        const Deadline drain = deadline_in(200);
        std::size_t dropped = 0;
        char buf[4096];
        while (dropped < 65536 && readable_by(fd, drain)) {
            const ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;                           // the peer closed too: done
            dropped += static_cast<std::size_t>(n);
        }
    }
    close(fd);
}

bool serve_metrics(const std::string &ip, int port, std::function<std::string()> render, std::string &err) {
    const int listener = listen_on(ip, port);
    if (listener < 0) {
        err = "cannot listen on " + ip + ":" + std::to_string(port) + " for metrics: " + std::strerror(errno);
        return false;
    }
    try {
        // One thread, one connection at a time (D-083): a flood of connections
        // waits in the listen backlog instead of creating threads.
        std::thread([listener, render]() {
            while (true) {
                const int fd = accept(listener, nullptr, nullptr);
                if (fd < 0) {
                    // EINTR or a reset before accept: try again. Out of descriptors
                    // (EMFILE): pause, or this loop would spin at full speed.
                    if (errno != EINTR) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                serve_metrics_connection(fd, render);
            }
        }).detach();
    } catch (const std::system_error &e) {
        close(listener);
        err = std::string("cannot start the metrics thread: ") + e.what();
        return false;
    }
    return true;
}
