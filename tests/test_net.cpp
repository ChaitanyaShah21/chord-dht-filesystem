// In-process tests for the line framing and the lookup transport (net.cpp).
//
// The end-to-end suite cannot reach the most important line in net.cpp: the
// node never produces a message containing a newline, so D-010's sanitiser is
// never exercised by talking to a real node. These tests drive send_line and
// recv_line directly over a socketpair -- two connected sockets inside one
// process -- so every framing edge case can be constructed on purpose.

#include "net.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

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

// -------------------------------------------------------------------------
// The lookup transport: exchange_line over real loopback sockets. Each case
// builds the one server behaviour it needs, so the four outcomes D-013 must
// keep apart -- refused, timeout, closed, failed -- are each produced on purpose.

// The port the kernel gave a socket bound to port 0.
int port_of(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    getsockname(fd, static_cast<sockaddr *>(static_cast<void *>(&a)), &len);
    return ntohs(a.sin_port);
}

// A free port with nothing listening on it: bind, read the number, close.
// (Another process could take it in between; on a test machine it does not.)
int dead_port() {
    const int fd = listen_on("127.0.0.1", 0);
    const int port = port_of(fd);
    close(fd);
    return port;
}

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since).count());
}

void test_connect_errno() {
    // connect_to must hand back the REAL cause. Before this was fixed it called
    // close() after a failed connect, and close() is allowed to change errno.
    errno = 0;
    const int fd = connect_to("127.0.0.1", dead_port(), 1000);
    check(fd < 0 && errno == ECONNREFUSED, "connect_to a dead port fails with errno ECONNREFUSED");

    errno = 0;
    check(connect_to("999.0.0.1", 80, 1000) < 0 && errno == EINVAL, "connect_to a bad address sets EINVAL");
}

void test_recv_line_errno() {
    std::string out;
    { Pair p; close(p.a); p.a = -1; errno = EINTR;
      check(!recv_line(p.b, out) && errno == 0, "a clean close sets errno to 0, whatever it held before"); }
    { Pair p; write_raw(p.a, std::string(MAX_LINE + 1, 'a')); errno = 0;
      check(!recv_line(p.b, out) && errno == EMSGSIZE, "an over-long line sets errno to EMSGSIZE"); }
}

void test_exchange_line() {
    std::string reply;

    // Ok: a server that answers one line.
    {
        const int lfd = listen_on("127.0.0.1", 0);
        std::thread server([lfd] {
            const int c = accept(lfd, nullptr, nullptr);
            std::string req;
            if (recv_line(c, req)) send_line(c, "echo " + req);
            close(c);
        });
        const Delivery d = exchange_line("127.0.0.1", port_of(lfd), "PING", reply, 2000);
        server.join();
        close(lfd);
        check(d == Delivery::Ok && reply == "echo PING", "a server that answers gives Ok and its one line");
    }

    // Refused: nothing listening.
    check(exchange_line("127.0.0.1", dead_port(), "PING", reply, 2000) == Delivery::Refused && reply.empty(),
          "nothing listening gives Refused");

    // Timeout: a listener that never accepts. The kernel completes the
    // handshake from the backlog, so connect succeeds and the READ times out --
    // exactly what a SIGSTOP'd node looks like.
    {
        const int lfd = listen_on("127.0.0.1", 0);
        const auto t0 = std::chrono::steady_clock::now();
        const Delivery d = exchange_line("127.0.0.1", port_of(lfd), "PING", reply, 200);
        const long ms = elapsed_ms(t0);
        close(lfd);
        check(d == Delivery::Timeout, "a peer that never replies gives Timeout, not Closed or Refused");
        check(ms >= 150 && ms < 2000, "and it gives up after about the deadline (took " + std::to_string(ms) + " ms for 200)");
    }

    // Closed: a server that accepts and hangs up without a word.
    {
        const int lfd = listen_on("127.0.0.1", 0);
        std::thread server([lfd] { close(accept(lfd, nullptr, nullptr)); });
        const Delivery d = exchange_line("127.0.0.1", port_of(lfd), "PING", reply, 2000);
        server.join();
        close(lfd);
        check(d == Delivery::Closed && reply.empty(), "a peer that hangs up before replying gives Closed");
    }

    // Closed, mid-line: half a reply, then a hang-up. Not a reply.
    {
        const int lfd = listen_on("127.0.0.1", 0);
        std::thread server([lfd] {
            const int c = accept(lfd, nullptr, nullptr);
            std::string req;
            recv_line(c, req);
            write_raw(c, "OWNER 0123");
            close(c);
        });
        const Delivery d = exchange_line("127.0.0.1", port_of(lfd), "PING", reply, 2000);
        server.join();
        close(lfd);
        check(d == Delivery::Closed && reply.empty(), "half a reply then a hang-up gives Closed, and no partial reply");
    }

    // Failed: a reply with no newline, longer than MAX_LINE.
    {
        const int lfd = listen_on("127.0.0.1", 0);
        std::thread server([lfd] {
            const int c = accept(lfd, nullptr, nullptr);
            std::string req;
            recv_line(c, req);
            write_raw(c, std::string(MAX_LINE + 10, 'x'));
            close(c);
        });
        const Delivery d = exchange_line("127.0.0.1", port_of(lfd), "PING", reply, 2000);
        server.join();
        close(lfd);
        check(d == Delivery::Failed, "an over-long reply gives Failed, not Closed");
    }

    // Failed: an address that is not an address.
    check(exchange_line("not-an-ip", 80, "PING", reply, 200) == Delivery::Failed, "a bad address gives Failed");

    // A host that never answers the handshake (decision D-025). 192.0.2.1 is in
    // TEST-NET-1 (RFC 5737), which is never assigned, so a SYN sent there gets
    // no reply. With the deadline set BEFORE connect, Linux gives up after it;
    // set after connect, this would block for the kernel's ~127 s of retries.
    // A machine with no route at all fails at once instead: that is reported
    // as a skip, because it proves nothing either way.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const Delivery d = exchange_line("192.0.2.1", 80, "PING", reply, 300);
        const long ms = elapsed_ms(t0);
        if (d == Delivery::Failed && ms < 100) {
            std::printf("  SKIP  no route to 192.0.2.1 here, so the connect deadline cannot be observed\n");
        } else {
            check(d == Delivery::Timeout && ms < 2000,
                  "a host that never answers the handshake gives Timeout at the deadline (took " +
                  std::to_string(ms) + " ms for 300)");
        }
    }
}

}  // namespace

// D-061: a server's receives must finish by a deadline, however the bytes are paced.
// The trickle cases are the point: one byte every 40 ms would satisfy a per-call
// timeout of 200 ms for ever, and must still fail at the 200 ms deadline.
long long ms_since_start(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// Write `bytes` one at a time, `gap_ms` apart, on a thread of its own.
std::thread trickle(int fd, std::string bytes, int gap_ms) {
    return std::thread([fd, bytes, gap_ms] {
        for (char c : bytes) {
            if (send(fd, &c, 1, MSG_NOSIGNAL) != 1) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        }
    });
}

// A backstop on the reading end: if the deadline under test were not enforced, the
// read would otherwise wait for ever and hang the test binary instead of failing it.
void backstop(int fd) {
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

void test_deadlines() {
    std::string out;
    { Pair p; backstop(p.b); write_raw(p.a, "PING\n");
      check(recv_line_by(p.b, out, deadline_in(200)) && out == "PING", "a line that is there arrives before its deadline"); }

    { Pair p; backstop(p.b); const auto t0 = std::chrono::steady_clock::now();
      const bool got = recv_line_by(p.b, out, deadline_in(200));
      const int e = errno; const long long ms = ms_since_start(t0);
      check(!got && e == EAGAIN && ms >= 190 && ms < 1000,
            "nothing sent: the line read gives up at the deadline with EAGAIN (" + std::to_string(ms) + " ms)"); }

    { Pair p; backstop(p.b); std::thread t = trickle(p.a, "abcdefghijklmnopqrstuvwxyz\n", 40);       // ~1,080 ms in all
      const auto t0 = std::chrono::steady_clock::now();
      const bool got = recv_line_by(p.b, out, deadline_in(200));
      const long long ms = ms_since_start(t0);
      check(!got && errno == EAGAIN && ms < 500,
            "a line trickled a byte every 40 ms still fails at the 200 ms deadline (" + std::to_string(ms) + " ms)");
      shutdown(p.b, SHUT_RDWR); t.join(); }

    { Pair p; backstop(p.b); std::thread t = trickle(p.a, "abc\n", 20);
      check(recv_line_by(p.b, out, deadline_in(1000)) && out == "abc", "a slow line that finishes in time is read whole");
      t.join(); }

    { Pair p; backstop(p.b); std::thread t = trickle(p.a, std::string(100, 'x'), 20);                  // ~2 s for 100 bytes
      std::string body; const auto t0 = std::chrono::steady_clock::now();
      const bool got = recv_exact_by(p.b, 100, body, deadline_in(200));
      const long long ms = ms_since_start(t0);
      check(!got && errno == EAGAIN && ms < 500, "a body trickled past its deadline fails at the deadline (" + std::to_string(ms) + " ms)");
      shutdown(p.b, SHUT_RDWR); t.join(); }

    { Pair p; backstop(p.b); const int fd = p.a;
      // 300 KB is more than a socketpair buffers, so the writer needs its own thread.
      std::thread w([fd] { write_raw(fd, std::string(300000, 'y')); });
      std::string body;
      check(recv_exact_by(p.b, 300000, body, deadline_in(2000)) && body == std::string(300000, 'y'),
            "a 300 KB body arriving in many pieces is read whole within the deadline");
      w.join(); }

    { Pair p; backstop(p.b); std::thread t = trickle(p.a, std::string(100, 'z'), 20);
      const auto t0 = std::chrono::steady_clock::now();
      const bool got = recv_discard_by(p.b, 100, deadline_in(200));
      check(!got && ms_since_start(t0) < 500, "draining a refused body has the same deadline");
      shutdown(p.b, SHUT_RDWR); t.join(); }

    { Pair p; backstop(p.b); write_raw(p.a, "half"); close(p.a); p.a = -1;
      check(!recv_line_by(p.b, out, deadline_in(1000)) && errno == 0, "a peer that closes mid-line is a close (errno 0), not a timeout"); }

    { Pair p; backstop(p.b); check(!recv_line_by(p.b, out, deadline_in(-5)) && errno == EAGAIN, "a deadline already past fails at once"); }
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);      // exactly as node.cpp does (D-011)
    std::printf("test-net: line framing and the lookup transport\n");

    test_send_line();
    test_recv_line();
    test_connect_errno();
    test_recv_line_errno();
    test_exchange_line();
    test_deadlines();

    std::printf("%s  %d/%d checks passed\n",
                failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
