#include "collect.h"

#include "names.h"      // valid_name
#include "store.h"      // valid_key

#include <set>

namespace {

std::string address(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

// Digits only, no sign, no leading zero, at most 18 digits (fits a 64-bit integer).
bool parse_count(const std::string &w, std::uint64_t &out) {
    if (w.empty() || w.size() > 18 || (w.size() > 1 && w[0] == '0')) return false;
    std::uint64_t v = 0;
    for (char c : w) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------

bool parse_entries_reply(const std::string &reply, std::vector<std::pair<std::string, std::string>> &page) {
    const std::vector<std::string> w = split_single_spaces(reply);
    std::uint64_t n = 0;
    if (w.size() < 2 || w[0] != "ENTRIES" || !parse_count(w[1], n) || w.size() != 2 + 2 * n) return false;
    page.clear();
    for (std::size_t i = 2; i < w.size(); i += 2) {
        if (!valid_name(w[i]) || !valid_key(w[i + 1])) return false;
        page.emplace_back(w[i], w[i + 1]);
    }
    return true;
}

bool parse_keys_reply(const std::string &reply, std::vector<std::string> &keys, long &grace_s) {
    const std::vector<std::string> w = split_single_spaces(reply);
    std::uint64_t n = 0, g = 0;
    if (w.size() < 3 || w[0] != "KEYS" || !parse_count(w[1], n) || !parse_count(w[2], g) || w.size() != 3 + n) return false;
    keys.clear();
    for (std::size_t i = 3; i < w.size(); ++i) {
        if (!valid_key(w[i]) || (!keys.empty() && w[i] <= keys.back())) return false;   // in order, no repeats
        keys.push_back(w[i]);
    }
    grace_s = static_cast<long>(g);
    return true;
}

bool parse_dropped_reply(const std::string &reply, std::size_t asked, std::string &bits, std::uint64_t &bytes) {
    const std::vector<std::string> w = split_single_spaces(reply);
    if (w.size() != 3 || w[0] != "DROPPED" || w[1].size() != asked || !parse_count(w[2], bytes)) return false;
    for (char c : w[1])
        if (c != '0' && c != '1') return false;
    bits = w[1];
    return true;
}

// ---------------------------------------------------------------------------
// Finding every node
// ---------------------------------------------------------------------------

bool walk_ring(const Peer &start, const Transport &route, std::vector<Peer> &processes, std::string &err, int max_steps) {
    processes.clear();
    auto add = [&](const Peer &p) {
        for (const Peer &q : processes)
            if (same_process(p, q)) return;
        processes.push_back(p);
    };
    add(start);
    Peer current = start;
    for (int step = 0; step < max_steps; ++step) {
        std::string line, why;
        Neighbours nb;
        if (route(current, "NEIGHBOURS", line) != Delivery::Ok || !parse_neighbours(line, nb, why)) {
            err = "could not ask " + address(current) + " for its successors" + (why.empty() ? "" : ": " + why);
            return false;
        }
        // The list runs clockwise from `current`, without gaps: once the walk has
        // gone all the way round, `start` is in it.
        bool closed = nb.succs.empty();
        for (const Peer &p : nb.succs) {
            add(p);
            closed = closed || same_node(p, start);
        }
        if (closed || same_node(nb.succs.back(), current)) return true;
        current = nb.succs.back();               // the farthest successor: up to r members per step
    }
    err = "the ring walk did not close after " + std::to_string(max_steps) + " steps";
    return false;
}

// ---------------------------------------------------------------------------
// The collection
// ---------------------------------------------------------------------------

CollectReport collect(const CollectIO &io, const CollectSettings &s) {
    CollectReport rep;
    const double t0 = io.now();          // the listing begins: D-091's L

    // ---- mark: every name, then every tree ----
    std::vector<std::pair<std::string, std::string>> all, page;
    std::string after, err;
    while (true) {
        if (!io.entries(after, page, err)) { rep.message = "could not list the names: " + err; return rep; }
        if (page.empty()) break;
        all.insert(all.end(), page.begin(), page.end());
        after = page.back().first;
    }
    rep.names = all.size();

    std::set<std::string> roots, live;
    for (const auto &e : all) roots.insert(e.second);
    rep.roots = roots.size();
    for (const std::string &root : roots) {
        const Resolved r = resolve_manifest(root, io.get);
        if (!r.ok) {
            // A tree that cannot be read is a set of chunks not known to be dead:
            // stop here, before anything anywhere is dropped (D-090).
            std::string named;
            for (const auto &e : all)
                if (e.second == root) named += (named.empty() ? "" : ", ") + e.first;
            rep.message = "the manifest tree of " + named + " could not be read (" + r.message + "); nothing was dropped";
            return rep;
        }
        live.insert(r.manifest_keys.begin(), r.manifest_keys.end());
        for (const ChunkRef &c : r.chunks) live.insert(c.key);
    }
    rep.live = live.size();
    rep.mark_s = io.now() - t0;

    // ---- find every node ----
    std::vector<Peer> processes;
    if (!io.nodes(processes, err)) rep.message = "the node list may be incomplete (" + err + "); nodes missed keep their garbage";

    // ---- sweep, node by node ----
    const double needed = s.upload_limit_s + s.tracker_wait_s + s.deadline_s;
    for (const Peer &p : processes) {
        NodeSweep ns;
        ns.address = address(p);
        std::vector<std::string> garbage, keys;
        std::string reply, cursor = "-";
        bool listed = true;
        while (true) {
            long grace = -1;
            if (io.ask(p, "KEYS " + cursor, reply) != Delivery::Ok || !parse_keys_reply(reply, keys, grace)) {
                ns.skipped = "could not list its chunks (" + reply.substr(0, 80) + ")";
                listed = false;
                break;
            }
            ns.grace_s = grace;
            if (keys.empty()) break;
            ns.held += keys.size();
            for (const std::string &k : keys)
                if (!live.count(k)) garbage.push_back(k);
            cursor = keys.back();
        }
        if (listed && ns.grace_s < needed)
            ns.skipped = "its grace (" + std::to_string(ns.grace_s) + " s) is shorter than the upload limit, the tracker wait "
                         "and this collection's deadline together (" + std::to_string(static_cast<long>(needed)) + " s)";
        ns.garbage = garbage.size();
        rep.garbage += garbage.size();

        // Drop in batches of MAX_HAVE_KEYS, and none once the deadline has passed:
        // the argument that no acknowledged upload is dropped needs every DROP sent
        // within D of the listing's start (D-091).
        for (std::size_t i = 0; ns.skipped.empty() && !s.dry_run && i < garbage.size(); i += MAX_HAVE_KEYS) {
            if (io.now() - t0 > s.deadline_s) { rep.deadline_hit = true; break; }
            const std::size_t end = std::min(garbage.size(), i + static_cast<std::size_t>(MAX_HAVE_KEYS));
            std::string line = "DROP " + s.gc_key;
            for (std::size_t j = i; j < end; ++j) line += " " + garbage[j];
            std::string bits;
            std::uint64_t bytes = 0;
            if (io.ask(p, line, reply) != Delivery::Ok || !parse_dropped_reply(reply, end - i, bits, bytes)) {
                ns.skipped = "DROP refused (" + reply.substr(0, 80) + ")";
                break;
            }
            for (char b : bits) ns.dropped += b == '1' ? 1 : 0;
            ns.bytes += bytes;
        }
        rep.dropped += ns.dropped;
        rep.bytes += ns.bytes;
        rep.nodes.push_back(ns);
        if (rep.deadline_hit) break;
    }
    if (rep.deadline_hit)
        rep.message += (rep.message.empty() ? "" : "; ") + std::string("the deadline passed: the rest was left for the next collection");
    rep.sweep_s = io.now() - t0 - rep.mark_s;
    rep.ok = true;
    return rep;
}
