// In-process tests for observability (decisions D-083 to D-087): the metrics
// registry and its text format, the metrics port's HTTP parser and reader, the
// structured logger, and request ids.
//
// The text format is also checked by an independent parser on real processes
// (scripts/e2e-observe.sh); here the cases are constructed, so each rule of the
// format, and each hostile input to the parser, is exercised on purpose.

#include "http.h"
#include "log.h"
#include "metrics.h"
#include "net.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// The lines of `text` that start with `prefix`.
std::vector<std::string> lines_starting(const std::string &text, const std::string &prefix) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (line.compare(0, prefix.size(), prefix) == 0) out.push_back(line);
    return out;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

void test_counters_and_gauges() {
    Registry r;
    Counter &plain = r.counter("chordfs_things_total", "Things that happened.");
    Counter &get = r.counter("chordfs_requests_total", "Requests served.", labels({{"verb", "get"}}));
    Counter &put = r.counter("chordfs_requests_total", "Requests served.", labels({{"verb", "put"}}));
    double level = 7;
    r.gauge("chordfs_level", "A level.", [&] { return level; });
    plain.add();
    get.add(3);
    put.add(0);
    level = 2.5;
    const std::string text = r.render();

    check(contains(text, "# HELP chordfs_things_total Things that happened.\n# TYPE chordfs_things_total counter\n"
                         "chordfs_things_total 1\n"),
          "a counter renders HELP, TYPE and its value, in that order");
    check(lines_starting(text, "# TYPE chordfs_requests_total").size() == 1,
          "two series of one name share ONE TYPE line (one family)");
    check(contains(text, "chordfs_requests_total{verb=\"get\"} 3\nchordfs_requests_total{verb=\"put\"} 0\n"),
          "labelled series follow their family's header, in registration order");
    check(contains(text, "chordfs_level 2.5\n"), "a gauge is read when rendered, not when registered");
    check(text.back() == '\n', "the exposition ends with a newline");
}

void test_histogram() {
    Registry r;
    Histogram &h = r.histogram("chordfs_wait_seconds", "Waits.", {0.01, 0.1, 1});
    for (double x : {0.005, 0.01, 0.05, 0.5, 0.5, 3.0}) h.observe(x);
    const std::string text = r.render();
    check(contains(text, "# TYPE chordfs_wait_seconds histogram\n"), "a histogram's TYPE is histogram");
    check(contains(text, "chordfs_wait_seconds_bucket{le=\"0.01\"} 2\n"),
          "a value equal to a bound counts in that bound's bucket (le is 'less or equal')");
    check(contains(text, "chordfs_wait_seconds_bucket{le=\"0.1\"} 3\n"), "buckets are cumulative");
    check(contains(text, "chordfs_wait_seconds_bucket{le=\"1\"} 5\n"), "the last finite bucket counts all but the overflow");
    check(contains(text, "chordfs_wait_seconds_bucket{le=\"+Inf\"} 6\n"), "+Inf counts everything");
    check(contains(text, "chordfs_wait_seconds_count 6\n"), "_count equals the +Inf bucket");
    check(contains(text, "chordfs_wait_seconds_sum 4.065\n"), "_sum adds the observations");

    Histogram &lab = r.histogram("chordfs_lab_seconds", "Labelled.", {1}, labels({{"verb", "get"}}));
    lab.observe(0.5);
    const std::string t2 = r.render();
    check(contains(t2, "chordfs_lab_seconds_bucket{verb=\"get\",le=\"1\"} 1\n"),
          "a labelled histogram puts le after its own labels");
    check(contains(t2, "chordfs_lab_seconds_sum{verb=\"get\"} 0.5\n"), "and _sum keeps the labels");

    // Hostile observations: none may wrap the unsigned sum or reach an undefined conversion.
    Histogram odd({1});
    odd.observe(-5);
    odd.observe(std::numeric_limits<double>::quiet_NaN());
    odd.observe(1e300);
    const Histogram::Reading rd = odd.read();
    check(rd.counts[0] == 2 && rd.counts[1] == 1, "negative and NaN count as 0; a huge value overflows");
    check(rd.sum == 1e12, "a huge value is capped in the sum (1e12), never wrapped");

    // Hop counts are whole numbers and are summed exactly.
    Histogram hops(hop_buckets());
    for (int i = 0; i < 1000; ++i) hops.observe(i % 7);
    check(hops.read().sum == 2997, "integer observations sum exactly");
}

void test_concurrency() {
    // Eight threads adding at once: with relaxed atomics no increment is lost.
    Counter c;
    Histogram h(latency_buckets());
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 100000; ++i) {
                c.add();
                h.observe(0.003);
            }
        });
    for (std::thread &t : ts) t.join();
    check(c.value() == 800000, "8 threads x 100,000 increments = 800,000, none lost");
    std::uint64_t total = 0;
    for (std::uint64_t n : h.read().counts) total += n;
    check(total == 800000, "8 threads x 100,000 observations, every one in a bucket");

    // Render while observing: every rendering must still be self-consistent.
    Registry r;
    Histogram &live = r.histogram("chordfs_live_seconds", "Live.", latency_buckets());
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        while (!stop) live.observe(0.004);
    });
    bool consistent = true;
    for (int i = 0; i < 200; ++i) {
        const std::string text = r.render();
        const std::vector<std::string> inf = lines_starting(text, "chordfs_live_seconds_bucket{le=\"+Inf\"}");
        const std::vector<std::string> cnt = lines_starting(text, "chordfs_live_seconds_count");
        if (inf.size() != 1 || cnt.size() != 1 || inf[0].substr(inf[0].rfind(' ')) != cnt[0].substr(cnt[0].rfind(' ')))
            consistent = false;
    }
    stop = true;
    writer.join();
    check(consistent, "rendered during 200 scrapes under load, +Inf always equals _count");
}

void test_format_rules() {
    check(labels({{"why", "a \"quoted\" \\ back\nslash"}}) == "why=\"a \\\"quoted\\\" \\\\ back\\nslash\"",
          "label values escape quote, backslash and newline");
    check(valid_metric_name("chordfs_x_total") && valid_metric_name("a:b") && valid_metric_name("_x"),
          "letters, digits, underscores and colons are valid names");
    check(!valid_metric_name("") && !valid_metric_name("9lives") && !valid_metric_name("a-b") &&
              !valid_metric_name("a b"),
          "an empty name, a leading digit, a hyphen and a space are refused");
    check(format_value(1234) == "1234" && format_value(0) == "0" && format_value(-1) == "-1",
          "whole numbers print without a fraction");
    check(format_value(0.0005) == "0.0005" && format_value(2.5) == "2.5", "fractions print plainly");
    check(format_value(std::numeric_limits<double>::infinity()) == "+Inf" &&
              format_value(std::numeric_limits<double>::quiet_NaN()) == "NaN",
          "infinity and NaN use the format's spellings");

    Registry r;
    r.counter("chordfs_a_total", "A.");
    bool threw = false;
    try { r.gauge("chordfs_a_total", "A again.", [] { return 0.0; }); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "one name registered as two types is refused at start-up");
    threw = false;
    try { r.counter("chordfs_a_total", "A."); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "the same name and labels twice is refused");
    threw = false;
    try { r.counter("bad name", "?"); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "an invalid name is refused");
    threw = false;
    try { r.histogram("chordfs_h", "H.", {1, 1}); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "histogram bounds that do not increase are refused");
    check(r.render() == "# HELP chordfs_a_total A.\n# TYPE chordfs_a_total counter\nchordfs_a_total 0\n",
          "refused registrations leave nothing behind in the output");

    Registry h;
    h.counter("chordfs_h_total", "Line one\nline two \\ end");
    check(contains(h.render(), "# HELP chordfs_h_total Line one\\nline two \\\\ end\n"),
          "HELP text escapes newline and backslash, so it stays one line");
}

// ---------------------------------------------------------------------------
// The metrics port's HTTP
// ---------------------------------------------------------------------------

void test_http_parse() {
    auto ok = [](const std::string &head, const std::string &method, const std::string &path) {
        const HttpRequest r = parse_http_head(head);
        return r.ok && r.method == method && r.path == path;
    };
    check(ok("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n", "GET", "/metrics"), "Prometheus's request parses");
    check(ok("GET /metrics HTTP/1.0\n\n", "GET", "/metrics"), "bare newlines and HTTP/1.0 are accepted");
    check(ok("GET /metrics?x=1 HTTP/1.1\r\n\r\n", "GET", "/metrics"), "a query string is dropped from the path");
    check(ok("POST /metrics HTTP/1.1\r\n\r\n", "POST", "/metrics"), "another method parses (and is refused later)");
    for (const std::string &bad : std::vector<std::string>{"", "\r\n\r\n", "GET\r\n\r\n", "GET /metrics\r\n\r\n", "GET  /metrics HTTP/1.1\r\n\r\n",
                                  "GET /metrics HTTP/1.1 extra\r\n\r\n", "get /metrics HTTP/1.1\r\n\r\n",
                                  "GET metrics HTTP/1.1\r\n\r\n", "GET /metrics HTTP/2.0\r\n\r\n",
                                  "GET /met\x01rics HTTP/1.1\r\n\r\n", "GET /metrics HTTP/1.1x\r\n\r\n",
                                  std::string("GET /metrics\0 HTTP/1.1\r\n\r\n", 26)})
        check(!parse_http_head(bad).ok, "malformed request line refused: " + logfmt_value(bad));

    int calls = 0;
    auto render = [&] { ++calls; return std::string("chordfs_x 1\n"); };
    const std::string good = metrics_answer("GET /metrics HTTP/1.1\r\n\r\n", render);
    check(good.compare(0, 17, "HTTP/1.0 200 OK\r\n") == 0, "GET /metrics answers 200");
    check(contains(good, "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"),
          "with the text format's content type and version");
    check(contains(good, "Content-Length: 12\r\n") && contains(good, "Connection: close\r\n\r\nchordfs_x 1\n"),
          "an exact Content-Length, Connection: close, then the body");
    check(metrics_answer("GET / HTTP/1.1\r\n\r\n", render).compare(0, 12, "HTTP/1.0 404") == 0, "another path: 404");
    check(metrics_answer("PUT /metrics HTTP/1.1\r\n\r\n", render).compare(0, 12, "HTTP/1.0 405") == 0, "another method: 405");
    check(metrics_answer("hello\r\n\r\n", render).compare(0, 12, "HTTP/1.0 400") == 0, "not a request: 400");
    check(calls == 1, "the metrics are rendered only for a GET of /metrics");
}

// A connected pair of sockets: what one end writes, the other reads.
bool pair(int fds[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0; }

void test_http_read() {
    int fds[2];
    std::string head;

    // The head arriving in pieces, the blank line split across two writes.
    if (!pair(fds)) { check(false, "socketpair"); return; }
    std::thread w([&] {
        for (const char *piece : {"GET /metr", "ics HTTP/1.1\r\nHost: a\r\n\r", "\nleftover body"}) {
            send(fds[1], piece, std::strlen(piece), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    check(read_http_head(fds[0], head, deadline_in(2000)) == HeadRead::Complete && parse_http_head(head).ok,
          "a head in three pieces, the terminator split between two, is read whole");
    w.join();
    close(fds[0]); close(fds[1]);

    // Too long: 4 KiB of header with no blank line.
    if (!pair(fds)) return;
    const std::string big = "GET /metrics HTTP/1.1\r\nX: " + std::string(5000, 'a');
    send(fds[1], big.data(), big.size(), 0);
    check(read_http_head(fds[0], head, deadline_in(2000)) == HeadRead::TooLarge && head.size() == MAX_HTTP_HEAD,
          "a head over 4 KiB is refused, and no more than 4 KiB is ever held");
    close(fds[0]); close(fds[1]);

    // A sender that trickles a byte every 50 ms: cut off at the deadline, not stretched by it.
    if (!pair(fds)) return;
    std::atomic<bool> stop{false};
    std::thread trickle([&] {
        while (!stop) {
            send(fds[1], "a", 1, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    const HeadRead got = read_http_head(fds[0], head, deadline_in(300));
    const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    stop = true;
    trickle.join();
    check(got == HeadRead::Failed && took >= 290 && took < 600,
          "a byte every 50 ms is cut off at the 300 ms deadline (took " + std::to_string(took) + " ms)");
    close(fds[0]); close(fds[1]);

    // The peer closes before a blank line.
    if (!pair(fds)) return;
    send(fds[1], "GET /", 5, 0);
    close(fds[1]);
    check(read_http_head(fds[0], head, deadline_in(2000)) == HeadRead::Failed, "a close mid-head is a failure");
    close(fds[0]);

    // A whole exchange through serve_metrics_connection, with a body the server never reads:
    // the graceful close must still deliver the response.
    if (!pair(fds)) return;
    const std::string req = "POST /metrics HTTP/1.1\r\nContent-Length: 20000\r\n\r\n" + std::string(20000, 'b');
    std::thread client([&] { send(fds[1], req.data(), req.size(), MSG_NOSIGNAL); shutdown(fds[1], SHUT_WR); });
    std::thread server([&] { serve_metrics_connection(fds[0], [] { return std::string(); }); });
    std::string got_reply;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fds[1], buf, sizeof buf, 0)) > 0) got_reply.append(buf, static_cast<std::size_t>(n));
    client.join();
    server.join();
    close(fds[1]);
    check(got_reply.compare(0, 12, "HTTP/1.0 405") == 0, "a request with an unread body still gets its whole response");
}

// ---------------------------------------------------------------------------
// The logger
// ---------------------------------------------------------------------------

void test_log_format() {
    const std::string line = format_log_line("2026-10-09T13:14:16.123Z", Level::Info, "node:7010", "6f1c2a9e0b3d4c5a",
                                             "chunk.get", {{"key", "ab12"}, {"ms", "0.8"}});
    check(line == "ts=2026-10-09T13:14:16.123Z level=info proc=node:7010 req=6f1c2a9e0b3d4c5a event=chunk.get key=ab12 ms=0.8",
          "a line is ts, level, proc, req, event, then the fields, single-spaced");
    check(format_log_line("t", Level::Warn, "client", "", "x", {}) == "ts=t level=warn proc=client event=x",
          "no request id: no req field");

    check(logfmt_value("plain") == "plain", "a safe value is written bare");
    check(logfmt_value("") == "\"\"", "an empty value is quoted, so the field is still visible");
    check(logfmt_value("evict 127.0.0.1:7011 (refused)") == "\"evict 127.0.0.1:7011 (refused)\"",
          "a value with spaces is quoted, its text kept word for word");
    check(logfmt_value("a=b") == "\"a=b\"", "an '=' is quoted, so it cannot forge a field");
    check(logfmt_value("x\" level=error") == "\"x\\\" level=error\"", "a quote is escaped, so it cannot end the value");
    check(logfmt_value("one\ntwo\r\t\x01") == "\"one\\ntwo\\r\\t\\x01\"",
          "newline, return, tab and other control bytes are escaped: one event stays one line");
    check(logfmt_value("back\\slash") == "\"back\\\\slash\"", "a backslash is escaped");

    const std::string hostile = format_log_line("t", Level::Error, "p", "", "e", {{"why", "line1\nts=forged level=info"}});
    check(hostile.find('\n') == std::string::npos, "no field value can put a newline in a line");

    Level l;
    check(parse_level("debug", l) && l == Level::Debug && parse_level("error", l) && l == Level::Error,
          "levels parse by name");
    check(!parse_level("INFO", l) && !parse_level("", l) && !parse_level("verbose", l), "unknown levels are refused");
}

void test_log_writes() {
    // Write through a pipe, from 8 threads at once: every line must arrive whole.
    int p[2];
    if (pipe(p) != 0) { check(false, "pipe"); return; }
    log_setup("test:1", Level::Info, p[1]);
    std::string collected;
    std::thread reader([&] {
        char buf[65536];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) collected.append(buf, static_cast<std::size_t>(n));
    });
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < 500; ++i)
                log_event(Level::Info, "test.line", {{"thread", std::to_string(t)}, {"pad", std::string(300, 'x')}});
        });
    for (std::thread &t : ts) t.join();
    log_event(Level::Debug, "test.hidden");                       // below the threshold
    log_event(Level::Info, "test.long", {{"big", std::string(10000, 'y')}});
    close(p[1]);
    reader.join();
    close(p[0]);
    log_setup("?", Level::Info, 2);

    std::istringstream in(collected);
    std::string line;
    int whole = 0, broken = 0, longest = 0;
    bool hidden = false;
    while (std::getline(in, line)) {
        longest = std::max(longest, static_cast<int>(line.size()) + 1);
        if (line.find("event=test.hidden") != std::string::npos) hidden = true;
        if (line.find("event=test.line") == std::string::npos) continue;
        const bool ok = line.compare(0, 3, "ts=") == 0 && line.find(" proc=test:1 ") != std::string::npos &&
                        line.size() > 300 && line.compare(line.size() - 300, 300, std::string(300, 'x')) == 0;
        ok ? ++whole : ++broken;
    }
    check(whole == 4000 && broken == 0,
          "8 threads x 500 lines through one pipe: " + std::to_string(whole) + " whole, " + std::to_string(broken) + " broken");
    check(!hidden, "a line below the threshold is not written");
    check(longest == static_cast<int>(MAX_LOG_LINE) && contains(collected, "...\n"),
          "a 10 KB line is cut to 4 KiB, newline included, and says so");
}

void test_request_ids() {
    check(valid_request_id("6f1c2a9e0b3d4c5a"), "16 lowercase hex characters is an id");
    for (const std::string &bad : std::vector<std::string>{"", "6f1c2a9e0b3d4c5", "6f1c2a9e0b3d4c5a0", "6F1C2A9E0B3D4C5A", "6f1c2a9e0b3d4c5g", "6f1c 2a9e0b3d4c5"})
        check(!valid_request_id(bad), "not an id: \"" + bad + "\"");
    std::set<std::string> seen;
    bool all_valid = true;
    for (int i = 0; i < 1000; ++i) {
        const std::string id = new_request_id();
        all_valid = all_valid && valid_request_id(id);
        seen.insert(id);
    }
    check(all_valid && seen.size() == 1000, "1,000 new ids: all valid, all different");

    check(current_request_id().empty() && with_request_id("PING") == "PING", "no id: a line goes out unchanged");
    {
        RequestScope outer("aaaaaaaaaaaaaaaa");
        check(with_request_id("GET k") == "RID aaaaaaaaaaaaaaaa GET k", "inside a scope, lines carry its id");
        {
            RequestScope inner("bbbbbbbbbbbbbbbb");
            check(current_request_id() == "bbbbbbbbbbbbbbbb", "a nested scope replaces it");
        }
        check(current_request_id() == "aaaaaaaaaaaaaaaa", "and its end restores the outer one");
        std::string other = "unset";
        std::thread([&] { other = current_request_id(); }).join();
        check(other.empty(), "a thread started inside a scope does NOT inherit the id (thread_local)");
        try {
            RequestScope thrown("cccccccccccccccc");
            throw 1;
        } catch (int) {
        }
        check(current_request_id() == "aaaaaaaaaaaaaaaa", "a scope left by an exception still restores the id");
    }
    check(current_request_id().empty(), "after every scope: no id");
    set_process_request_id("dddddddddddddddd");
    std::string worker;
    std::thread([&] { worker = current_request_id(); }).join();
    check(worker == "dddddddddddddddd", "the process's id is seen by every thread (the client's workers)");
    {
        RequestScope s("eeeeeeeeeeeeeeee");
        check(current_request_id() == "eeeeeeeeeeeeeeee", "a thread's own id wins over the process's");
    }
    set_process_request_id("");
}

void test_rid_prefix() {
    std::string id, rest;
    check(split_request_id("RID 6f1c2a9e0b3d4c5a PUT k 2 10", id, rest) == RidPrefix::Present && id == "6f1c2a9e0b3d4c5a" &&
              rest == "PUT k 2 10",
          "a RID prefix is split into the id and the request");
    check(split_request_id("PUT k 2 10", id, rest) == RidPrefix::None && rest == "PUT k 2 10", "no prefix: the whole line");
    check(split_request_id("RIDDLE me this", id, rest) == RidPrefix::None, "a word that only starts with RID is not a prefix");
    for (const std::string &bad : std::vector<std::string>{"RID ", "RID 6f1c2a9e0b3d4c5a", "RID 6f1c2a9e0b3d4c5a ",
                                                           "RID 6F1C2A9E0B3D4C5A PING", "RID 6f1c2a9e0b3d4c5 PING",
                                                           "RID 6f1c2a9e0b3d4c5ab PING", "RID  6f1c2a9e0b3d4c5a PING",
                                                           "RID 6f1c2a9e0b3d4c5aPING"})
        check(split_request_id(bad, id, rest) == RidPrefix::Malformed, "malformed prefix refused: \"" + bad + "\"");

    std::size_t n = 0;
    check(body_length("RID 6f1c2a9e0b3d4c5a PUT k 2 10", n) == Body::Present && n == 10,
          "body_length looks past the prefix: a prefixed PUT still carries its body");
    check(body_length("RID 6f1c2a9e0b3d4c5a GET k", n) == Body::None, "and a prefixed GET carries none");
    check(body_length("RID 6f1c2a9e0b3d4c5a STORE k 007", n) == Body::Malformed,
          "a prefixed line's length is checked as strictly as a bare one's");
    check(body_length("RID bad STORE k 10", n) == Body::None,
          "a malformed prefix is not looked past (the server refuses the line and closes)");
}

}  // namespace

int main() {
    std::printf("test-obs: metrics, the metrics port, the logger, request ids\n");
    test_counters_and_gauges();
    test_histogram();
    test_concurrency();
    test_format_rules();
    test_http_parse();
    test_http_read();
    test_log_format();
    test_log_writes();
    test_request_ids();
    test_rid_prefix();
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
