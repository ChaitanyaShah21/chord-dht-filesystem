#ifndef HTTP_H
#define HTTP_H

#include "net.h"

#include <cstddef>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// Just enough HTTP/1.0 to be scraped by Prometheus (decision D-083).
//
// One request head in, one response out, then the connection is closed:
//
//   GET /metrics HTTP/1.1          -> 200, the metrics as text
//   GET /anything-else HTTP/1.1    -> 404
//   POST /metrics HTTP/1.1         -> 405 (any method but GET)
//   anything that is not a request -> 400
//   a head over MAX_HTTP_HEAD      -> 431
//
// The parser and the answer are pure functions, tested in process; only
// read_http_head and serve_metrics touch a socket.
// ---------------------------------------------------------------------------

// The longest request head read: the request line and every header, up to and
// including the blank line that ends them. Prometheus sends about 300 bytes.
constexpr std::size_t MAX_HTTP_HEAD = 4096;

// The whole head must arrive within this, and the response must be accepted by
// the kernel within it too.
constexpr int HTTP_DEADLINE_MS = 2000;

struct HttpRequest {
    bool ok = false;           // a well-formed request line
    std::string method;        // "GET"
    std::string path;          // "/metrics", without any "?query"
};

// Parse a complete request head (as read_http_head returns it). Only the first
// line is examined: METHOD SP PATH SP HTTP/1.0|HTTP/1.1, the method in capitals,
// the path starting with '/'. Headers are ignored. Never throws.
HttpRequest parse_http_head(const std::string &head);

// A complete response: status line, Content-Type, Content-Length, Connection:
// close, a blank line, and the body.
std::string http_response(int status, const std::string &content_type, const std::string &body);

// What the metrics port answers to one head. `render` is called only for a GET of
// /metrics.
std::string metrics_answer(const std::string &head, const std::function<std::string()> &render);

enum class HeadRead {
    Complete,    // `head` holds everything up to and including the blank line
    TooLarge,    // MAX_HTTP_HEAD bytes arrived without a blank line
    Failed,      // the peer closed, the socket failed, or the deadline passed
};

// Read a request head from `fd` by the deadline. Ends at "\r\n\r\n" or "\n\n".
HeadRead read_http_head(int fd, std::string &head, Deadline deadline);

// Answer one accepted connection completely, then close it.
void serve_metrics_connection(int fd, const std::function<std::string()> &render);

// Listen on ip:port and serve /metrics on one detached thread, one connection at
// a time, for the rest of the process. Returns false, with `err` set, if the port
// cannot be opened or the thread cannot be started.
bool serve_metrics(const std::string &ip, int port, std::function<std::string()> render, std::string &err);

#endif
