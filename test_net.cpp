// In-process tests for the node's line framing (net.cpp).
//
// The end-to-end suite cannot reach the most important line in net.cpp: the
// node never produces a message containing a newline, so D-010's sanitiser is
// never exercised by talking to a real node. These tests drive send_line and
// recv_line directly over a socketpair -- two connected sockets inside one
// process -- so every framing edge case can be constructed on purpose.

#include "net.h"

#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <string>

namespace {

int checks   = 0;
int failures = 0;

void check(bool condition, const std::string &what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

// A fresh connected pair: whatever is written to a[0] can be read from a[1].
struct Pair {
    int a = -1, b = -1;
    Pair() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) { a = fds[0]; b = fds[1]; }
    }
    ~Pair() { if (a >= 0) close(a); if (b >= 0) close(b); }
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;
};

// Read exactly what is waiting on fd right now (up to 8 KB), as raw bytes.
std::string drain(int fd, std::size_t expect) {
    std::string got;
    char buf[8192];
    while (got.size() < expect) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        got.append(buf, static_cast<std::size_t>(n));
    }
    return got;
}

void write_raw(int fd, const std::string &bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

void test_send_line() {
    { Pair p; check(send_line(p.a, "PING") && drain(p.b, 5) == "PING\n",
                    "a plain message gains exactly one newline"); }
    { Pair p; check(send_line(p.a, "PING\n") && drain(p.b, 5) == "PING\n",
                    "a caller's own newline is not doubled"); }
    { Pair p; check(send_line(p.a, "PING\r\n") && drain(p.b, 5) == "PING\n",
                    "a caller's \\r\\n becomes a single \\n"); }
    { Pair p; check(send_line(p.a, "") && drain(p.b, 1) == "\n",
                    "an empty message is one empty line"); }

    // D-010: an embedded newline must not split one message into two lines.
    std::fprintf(stderr, "  (the next two [net] BUG lines are expected)\n");
    { Pair p; check(send_line(p.a, "OWNER a\nb") && drain(p.b, 10) == "OWNER a b\n",
                    "an embedded \\n becomes a space: still one line"); }
    { Pair p; check(send_line(p.a, "x\ry") && drain(p.b, 4) == "x y\n",
                    "an embedded \\r becomes a space: still one line"); }

    // D-011: the peer is gone. With SIGPIPE ignored this is a false return,
    // not the death of the process.
    { Pair p; close(p.b); p.b = -1;
      check(!send_line(p.a, "anyone there?"), "sending to a closed peer returns false"); }
}

void test_recv_line() {
    std::string out;

    { Pair p; write_raw(p.a, "hello\n");
      check(recv_line(p.b, out) && out == "hello", "a line comes back without its newline"); }
    { Pair p; write_raw(p.a, "hello\r\n");
      check(recv_line(p.b, out) && out == "hello", "a trailing \\r is stripped"); }
    { Pair p; write_raw(p.a, "\n");
      check(recv_line(p.b, out) && out.empty(), "an empty line is a valid, empty line"); }

    // Two lines arriving in a single write must come back as two calls. A
    // reader that pulled a block and threw away what followed the first
    // newline would lose "two" -- the desync class of bug (R3, R6).
    { Pair p; write_raw(p.a, "one\ntwo\n");
      const bool first  = recv_line(p.b, out) && out == "one";
      const bool second = recv_line(p.b, out) && out == "two";
      check(first && second, "two lines in one write are read as two lines, nothing lost"); }

    // The boundary, exactly: MAX_LINE characters is allowed, one more is not.
    { Pair p; write_raw(p.a, std::string(MAX_LINE, 'a') + "\n");
      check(recv_line(p.b, out) && out.size() == MAX_LINE, "a line of exactly MAX_LINE characters is accepted"); }
    { Pair p; write_raw(p.a, std::string(MAX_LINE + 1, 'a') + "\n");
      check(!recv_line(p.b, out), "a line of MAX_LINE + 1 characters is refused"); }

    // The peer dies mid-line: a partial line must not be reported as a line.
    { Pair p; write_raw(p.a, "FIND_SUCC"); close(p.a); p.a = -1;
      check(!recv_line(p.b, out), "a partial line followed by close is not a line"); }
    { Pair p; close(p.a); p.a = -1;
      check(!recv_line(p.b, out), "a peer that closes without sending gives false"); }
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);      // exactly as node.cpp does (D-011)
    std::printf("test-net: line framing\n");

    test_send_line();
    test_recv_line();

    std::printf("%s  %d/%d checks passed\n",
                failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
