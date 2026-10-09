#ifndef COLLECT_H
#define COLLECT_H

#include "chunk.h"
#include "transfer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// The collector: mark and sweep over the whole system (decisions D-090 to D-092).
//
//   mark    every name and manifest key from the tracker group (ENTRIES), then
//           every manifest tree through verified reads: the live set. If any tree
//           cannot be read completely, stop -- nothing is dropped.
//   find    every node process, by walking the ring with NEIGHBOURS
//   sweep   for each node: list what it holds (KEYS), send back what is not live
//           (DROP); the node drops each only if it is old enough (D-091)
//
// Like every logic module here, nothing in collect() opens a socket: the tracker,
// the ring and the clock are passed in (CollectIO), so the tests run a whole
// collection against a namespace and nodes held in memory, and set the clock.
// ---------------------------------------------------------------------------

struct CollectSettings {
    std::string gc_key;              // the nodes' shared key (D-092)
    bool dry_run = false;            // mark and list, report what would go, drop nothing
    double deadline_s = 1800;        // D: no DROP is sent this long after the listing began (D-091)
    double upload_limit_s = 3600;    // U: the clients' limit on an upload, which each node's grace must cover
    double tracker_wait_s = 10;      // W: how long a client keeps trying to register (D-081)
};

struct CollectIO {
    // One page of ENTRIES after `after` ("" from the start): (name, key) pairs; an
    // empty page is the end. False, with `err`, if the tracker group could not answer.
    std::function<bool(const std::string &after, std::vector<std::pair<std::string, std::string>> &page,
                       std::string &err)> entries;
    GetFn get;                       // a verified read of one chunk through the ring (D-049)
    // Every node process. False, with `err`, if the list may be incomplete -- the
    // processes found are still swept: a node missed keeps its garbage, nothing more.
    std::function<bool(std::vector<Peer> &processes, std::string &err)> nodes;
    // One request line to a node process, one reply line back.
    std::function<Delivery(const Peer &to, const std::string &line, std::string &reply)> ask;
    std::function<double()> now;     // seconds, by a monotonic clock
};

// What one node's sweep did.
struct NodeSweep {
    std::string address;
    long grace_s = -1;               // the grace the node reported
    std::size_t held = 0;            // chunks it listed
    std::size_t garbage = 0;         // of those, not live
    std::size_t dropped = 0;         // of those, dropped
    std::uint64_t bytes = 0;         // bytes freed
    std::string skipped;             // why this node was not swept, if it was not
};

struct CollectReport {
    bool ok = false;                 // false: the mark failed, and nothing was dropped anywhere
    std::string message;             // why it stopped, or notes (an incomplete node list, the deadline)
    std::size_t names = 0, roots = 0, live = 0;
    std::vector<NodeSweep> nodes;
    std::size_t garbage = 0, dropped = 0;
    std::uint64_t bytes = 0;
    bool deadline_hit = false;
    double mark_s = 0, sweep_s = 0;
};

CollectReport collect(const CollectIO &io, const CollectSettings &s);

// Every distinct process in the ring, by asking NEIGHBOURS from `start` and jumping
// to the farthest successor each time, until the walk comes back round to `start`.
// False, with `err`, if a member could not be asked; `processes` still holds those found.
bool walk_ring(const Peer &start, const Transport &route, std::vector<Peer> &processes, std::string &err,
               int max_steps = 100000);

// The replies the collector reads, strictly parsed (exposed for the tests).
bool parse_entries_reply(const std::string &reply, std::vector<std::pair<std::string, std::string>> &page);
bool parse_keys_reply(const std::string &reply, std::vector<std::string> &keys, long &grace_s);
bool parse_dropped_reply(const std::string &reply, std::size_t asked, std::string &bits, std::uint64_t &bytes);

#endif
