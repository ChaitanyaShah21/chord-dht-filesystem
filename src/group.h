#ifndef GROUP_H
#define GROUP_H

#include "net.h"
#include "raft.h"       // Millis

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Addresses of a tracker group, and the client's side of talking to one
// (decisions D-079, D-081). Nothing here opens a socket: the exchange, the clock
// and the sleep are passed in, so tests drive it with scripted replicas.
// ---------------------------------------------------------------------------

struct Address {
    std::string ip;          // canonical dotted quad
    int port = 0;
    std::string text() const { return ip + ":" + std::to_string(port); }
};

// "ip:port,ip:port,..." in the order given, each validated as strictly as a ring
// address. False with `err` on any malformed item, an empty list, or a repeat.
bool parse_address_list(const std::string &text, std::vector<Address> &out, std::string &err);

// One exchange with one replica: send `request`, read one reply line.
using ExchangeFn = std::function<Delivery(const Address &to, const std::string &request, std::string &reply)>;

struct GroupClient {
    std::vector<Address> replicas;     // as given on the command line
    int wait_ms = 10000;               // give up after this long (--tracker-wait)
    int pause_ms = 100;                // between rounds in which no replica answered
    std::size_t preferred = 0;         // where the next request starts: the last replica that answered
    ExchangeFn exchange;
    std::function<Millis()> now;       // milliseconds on a steady clock
    std::function<void(int)> sleep;    // milliseconds
};

// Ask the group. A reply of REDIRECT <ip:port> is followed at once (at most as many
// redirects per round as there are replicas, so two replicas naming each other
// cannot loop); UNAVAILABLE, a refusal, a timeout or a closed connection moves on
// to the next replica; after a round with no answer, pause and go round again
// until wait_ms has passed. True with the answer in `reply`; false with `reply`
// saying what each replica last said.
bool ask_group(GroupClient &g, const std::string &request, std::string &reply);

#endif
