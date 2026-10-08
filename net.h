#ifndef NET_H
#define NET_H

#include <cstddef>
#include <string>

// ---------------------------------------------------------------------------
// Line framing for the node's control protocol (decision D-021, part a).
//
// One request is one line; one reply is one line. This is the single copy the
// node uses. client.cpp and tracker.cpp still carry their own older copies --
// a known duplication, recorded in D-021, not an oversight.
//
// The caller must ignore SIGPIPE process-wide before using any of this
// (decision D-011), so that writing to a departed peer returns an error value
// instead of killing the process.
// ---------------------------------------------------------------------------

// The longest line recv_line will accept, not counting the '\n'. A peer that
// sends more without a newline is broken or hostile, and the connection is
// dropped rather than buffered without limit.
constexpr std::size_t MAX_LINE = 1024;

// Send `line` followed by exactly one '\n'. Enforces D-010: any terminator the
// caller supplied is stripped, and any '\n' or '\r' INSIDE the line is replaced
// by a space and logged as a bug -- so no message can ever become two.
// Returns false if the peer has gone or the socket failed.
bool send_line(int fd, const std::string &line);

// Receive one line into `out`, without its '\n' (and without a trailing '\r').
// Returns false if the peer closed, the socket failed, or the line exceeded
// MAX_LINE. On false, `out` may hold a partial line and must not be used.
// errno tells the cases apart for a caller that needs to: an over-long line
// sets EMSGSIZE, a clean close by the peer sets errno to 0, and any other
// failure leaves the errno recv() reported (EAGAIN: the receive timeout fired).
bool recv_line(int fd, std::string &out);

// Connect to an IPv4 ip:port. Returns the socket, or -1 with errno describing
// the failure (ECONNREFUSED: nothing is listening).
//
// If timeout_ms > 0, SO_RCVTIMEO and SO_SNDTIMEO are set BEFORE connecting
// (decision D-025). On Linux a blocking connect honours SO_SNDTIMEO, so a host
// that never answers the handshake fails after timeout_ms with errno
// EINPROGRESS, instead of after the kernel's ~127 s of SYN retries.
int connect_to(const std::string &ip, int port, int timeout_ms = 0);

// What happened to one request/reply exchange. A refusal and a timeout are
// different evidence (decision D-013), so they are never merged:
//   Refused  -- the connection was refused: nothing is listening. Proof.
//   Timeout  -- no handshake, or no reply, within the deadline. Suspicion.
//   Closed   -- the peer closed or reset the connection before a full reply.
//   Failed   -- anything else: a bad address, no route, out of descriptors,
//               or a reply longer than MAX_LINE.
enum class Delivery { Ok, Refused, Timeout, Closed, Failed };

// A short lowercase name for a Delivery, for log lines: "ok", "refused", ...
const char *delivery_name(Delivery d);

// One exchange on a FRESH connection (decision D-024, F19b): connect, send
// `request` as one line, read one line into `reply`, close. Every step is
// bounded by timeout_ms. `reply` is meaningful only when Ok is returned.
Delivery exchange_line(const std::string &ip, int port, const std::string &request,
                       std::string &reply, int timeout_ms);

// Bind and listen on an IPv4 ip:port with SO_REUSEADDR (defect B5). Returns the
// listening socket, or -1 with errno describing the failure.
int listen_on(const std::string &ip, int port);

// ---------------------------------------------------------------------------
// Bodies: binary data after a header line (decision D-044).
//
// A chunk can contain any byte, newlines included, so it cannot travel as a
// line. A header line whose first word is PUT, STORE or CHUNK is followed by a
// body, and the header's LAST word is the body's length in bytes:
//
//     PUT <key> <w> <length>\n<length bytes>
//
// recv_line reads one byte per call and never past its newline, so after it
// returns the body's first byte is still waiting in the socket for recv_exact.
// ---------------------------------------------------------------------------

// The longest body either side will send or accept: one chunk, 512 KiB (D-016).
// Equal to MAX_CHUNK in store.h; chunk.cpp checks the two agree at compile time.
#ifndef CHORDFS_MAX_CHUNK
#define CHORDFS_MAX_CHUNK (512 * 1024)       // overridden only by the sweep build (D-063)
#endif
constexpr std::size_t MAX_BODY = CHORDFS_MAX_CHUNK;

enum class Body {
    None,        // this line carries no body
    Present,     // a body of `length` bytes follows
    Malformed,   // the verb says a body follows, but the length is not a clean number
    TooLarge,    // the length is a clean number above MAX_BODY
};

// The one place that knows which lines carry a body. Used by sender and
// receiver alike, so they cannot disagree. The length must be digits only, with
// no sign and no leading zero (a lone "0" is a zero-byte body), at most 7 digits.
// `length` is written only when Present is returned.
Body body_length(const std::string &line, std::size_t &length);

// Send all `len` bytes, looping over short writes. False if the peer has gone.
bool send_all(int fd, const char *data, std::size_t len);

// Receive exactly `n` bytes into `out`, looping over short reads. Returns false
// if the peer closed first (errno set to 0) or the socket failed (errno from
// recv; EAGAIN means the receive timeout fired). `out` is meaningful only on true.
bool recv_exact(int fd, std::size_t n, std::string &out);

// Read and throw away exactly `n` bytes, through a small buffer: a body that is
// refused (D-052) is consumed without being held, so the stream stays in step.
// False as recv_exact.
bool recv_discard(int fd, std::size_t n);

// One exchange on a fresh connection, with bodies: send `line` and, if the line
// declares one, `body` (whose size must equal the declared length); then read
// one reply line and, if it declares one, its body into `reply_body`. Every step
// is bounded by timeout_ms, applied to each system call. Classified as
// exchange_line classifies; a reply declaring a malformed or oversized body is
// Failed. The outputs are meaningful only when Ok is returned.
Delivery exchange_frame(const std::string &ip, int port, const std::string &line,
                        const std::string &body, std::string &reply, std::string &reply_body,
                        int timeout_ms);

#endif
