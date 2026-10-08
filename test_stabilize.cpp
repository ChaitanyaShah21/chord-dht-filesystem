// In-process tests for joins, stabilisation and failure handling (stabilize.cpp).
//
// A whole ring runs in this one process. Every node is a NodeState; the
// Transport hands a request straight to the target's handle_request(), and a
// node marked dead refuses while a node marked frozen times out. Rounds run on
// one thread in an order the test chooses, so timings that a real ring
// produces only by luck -- two joins between the same pair, a node dying
// between its notify and its predecessor's stabilise -- are constructed exactly.
//
// Running every node on ONE thread also enforces D-030's rule that no lock is
// held across a call: a node that called out while holding its mutex, and was
// called back (a self-call, or a probe), would re-lock the same std::mutex and
// hang this test instead of passing it.
//
// Correctness is judged by an INDEPENDENT oracle: successors, predecessors,
// lists and fingers by brute-force clockwise distance, written here without
// route_step, build_fingers, successor_of or the in_range functions.

#include "chord.h"
#include "lookup.h"
#include "net.h"
#include "stabilize.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
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

std::string addr(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

// How the failure detector files a member's strikes: address and identifier,
// because one process can be several members (D-047).
std::string strike_key(const Peer &p) { return addr(p) + "/" + id_to_hex(p.id); }

// A distinct, canonical address for the n-th constructed node.
Peer make_peer(Id id, int n) {
    return Peer{id, "10.0." + std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255), 9000};
}

// ---------------------------------------------------------------------------
// The ring in one process.

struct Sim {
    std::map<std::string, std::unique_ptr<NodeState>> nodes;   // by address
    std::set<std::string> dead, frozen;
    std::vector<std::string> events;                           // every node's log, in order
    long calls = 0;
    int r;
    Transport transport;

    explicit Sim(int successors = DEFAULT_SUCCESSORS) : r(successors) {
        // Captures `this`, so a Sim must never be copied or moved.
        transport = [this](const Peer &to, const std::string &request, std::string &reply) {
            return deliver(to, request, reply);
        };
    }
    Sim(const Sim &) = delete;
    Sim &operator=(const Sim &) = delete;

    Delivery deliver(const Peer &to, const std::string &request, std::string &reply) {
        ++calls;
        reply.clear();
        const std::string a = addr(to);
        auto it = nodes.find(a);
        if (it == nodes.end() || dead.count(a)) return Delivery::Refused;   // nothing listening
        if (frozen.count(a)) return Delivery::Timeout;                      // listening, not answering
        Handled h = handle_request(*it->second, request, transport);
        reply = h.reply;
        if (h.after) h.after();
        return Delivery::Ok;
    }

    NodeState &add(const Peer &p) {
        auto node = std::make_unique<NodeState>(p, r);
        const std::string a = addr(p);
        node->log = [this, a](const std::string &e) { events.push_back(a + " " + e); };
        NodeState &ref = *node;
        nodes[a] = std::move(node);
        dead.erase(a);
        return ref;
    }

    NodeState &at(const Peer &p) { return *nodes.at(addr(p)); }

    bool runs(const std::string &a) const { return nodes.count(a) && !dead.count(a) && !frozen.count(a); }

    // One round: every running node, in the order given, runs one maintenance round.
    void round(const std::vector<Peer> &order) {
        for (const Peer &p : order)
            if (runs(addr(p))) maintenance_round(at(p), transport);
    }
};

// A ring wired correct from the start (init_wired), as the benchmark starts.
std::vector<Peer> wired_ring(Sim &sim, std::vector<Peer> members) {
    std::sort(members.begin(), members.end(), [](const Peer &a, const Peer &b) { return a.id < b.id; });
    for (const Peer &p : members) sim.add(p);
    for (const Peer &p : members) init_wired(sim.at(p), members);
    return members;
}

std::vector<Peer> random_peers(std::mt19937_64 &rng, int n, int first_index = 1) {
    std::vector<Peer> out;
    std::set<Id> used;
    for (int i = 0; i < n; ++i) {
        Id id;
        do { id = rng(); } while (used.count(id));
        used.insert(id);
        out.push_back(make_peer(id, first_index + i));
    }
    return out;
}

// ---------------------------------------------------------------------------
// The independent oracle: clockwise distances, nothing else.

Id cw(Id from, Id to) { return to - from; }    // clockwise distance, wraps modulo 2^64

struct Expected {
    Peer succ;
    bool has_pred = false;
    Peer pred;
    std::vector<Peer> succs;
    std::vector<Peer> fingers;
};

Expected expected_for(const Peer &me, const std::vector<Peer> &members, int r) {
    Expected e;
    std::vector<Peer> others;
    for (const Peer &p : members) if (!(p.ip == me.ip && p.port == me.port)) others.push_back(p);
    std::sort(others.begin(), others.end(),
              [&](const Peer &a, const Peer &b) { return cw(me.id, a.id) < cw(me.id, b.id); });
    if (others.empty()) {
        e.succ = me;
        e.succs = {me};
    } else {
        e.succ = others.front();
        e.has_pred = true;
        e.pred = others.back();
        for (int i = 0; i < r && i < static_cast<int>(others.size()); ++i) e.succs.push_back(others[i]);
    }
    for (int i = 0; i < ID_BITS; ++i) {
        const Id start = me.id + (static_cast<Id>(1) << i);
        Peer best = members[0];
        for (const Peer &p : members)
            if (cw(start, p.id) < cw(start, best.id)) best = p;     // first node at or after start
        e.fingers.push_back(best);
    }
    return e;
}

bool same_list(const std::vector<Peer> &a, const std::vector<Peer> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (!same_node(a[i], b[i])) return false;
    return true;
}

struct Verdict {
    bool ring = true, lists = true, fingers = true;
    std::string wrong;     // the first node whose successor or predecessor was wrong
};

Verdict judge(Sim &sim, const std::vector<Peer> &members) {
    Verdict v;
    for (const Peer &m : members) {
        const Expected e = expected_for(m, members, sim.r);
        NodeState &s = sim.at(m);
        std::lock_guard<std::mutex> lock(s.mu);
        const bool pred_ok = s.has_pred == e.has_pred && (!e.has_pred || same_node(s.pred, e.pred));
        if (!same_node(s.succs[0], e.succ) || !pred_ok) {
            if (v.ring) v.wrong = addr(m);
            v.ring = false;
        }
        if (!same_list(s.succs, e.succs)) v.lists = false;
        if (!same_list(s.fingers, e.fingers)) v.fingers = false;
    }
    return v;
}

bool all_correct(const Verdict &v) { return v.ring && v.lists && v.fingers; }

// Rounds (each a random order of the running members) until `done` holds;
// returns the number of rounds run, or -1 if `limit` is reached first.
template <typename Done>
int rounds_until(Sim &sim, const std::vector<Peer> &members, std::mt19937_64 &rng, int limit, Done done) {
    for (int n = 0; n <= limit; ++n) {
        if (done(judge(sim, members))) return n;
        std::vector<Peer> order = members;
        std::shuffle(order.begin(), order.end(), rng);
        sim.round(order);
    }
    return -1;
}

// Every running member looks up `keys`; every owner must be the oracle's.
int wrong_lookups(Sim &sim, const std::vector<Peer> &members, const std::vector<Id> &keys) {
    int wrong = 0;
    for (const Peer &start : members) {
        for (Id k : keys) {
            Peer best = members[0];
            for (const Peer &p : members) if (cw(k, p.id) < cw(k, best.id)) best = p;
            const LookupResult r = lookup(start, k, sim.transport);
            if (!r.ok() || !same_node(r.owner, best)) ++wrong;
        }
    }
    return wrong;
}

std::vector<Peer> without(std::vector<Peer> v, const Peer &gone) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Peer &p) { return same_node(p, gone); }), v.end());
    return v;
}

int count_events(const Sim &sim, const std::string &needle) {
    int n = 0;
    for (const std::string &e : sim.events) if (e.find(needle) != std::string::npos) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Wire formats

void test_wire() {
    std::printf("wire formats\n");
    const Peer a = make_peer(0x0123456789abcdefULL, 1), b = make_peer(42, 2);

    Neighbours n;
    n.has_pred = true; n.pred = a; n.succs = {b, a};
    const std::string line = render_neighbours(n);
    check(line == "NEIGHBOURS 0123456789abcdef 10.0.0.1 9000 2 000000000000002a 10.0.0.2 9000 "
                  "0123456789abcdef 10.0.0.1 9000", "NEIGHBOURS renders peers exactly as lookup replies do");
    Neighbours back;
    std::string why;
    check(parse_neighbours(line, back, why) && back.has_pred && same_node(back.pred, a) &&
          back.pred.id == a.id && back.succs.size() == 2 && same_node(back.succs[0], b),
          "a rendered NEIGHBOURS reply parses back to the same thing");

    Neighbours none;
    none.succs = {b};
    check(render_neighbours(none) == "NEIGHBOURS NONE 1 000000000000002a 10.0.0.2 9000", "no predecessor is NONE");
    check(parse_neighbours(render_neighbours(none), back, why) && !back.has_pred, "NONE parses as no predecessor");

    const std::vector<std::string> hostile = {
        "", "NEIGHBOURS", "NEIGHBOURS NONE", "NEIGHBOURS NONE 0",
        "NEIGHBOURS NONE 1", "NEIGHBOURS NONE 2 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 1 000000000000002a 10.0.0.2 9000 extra",
        "NEIGHBOURS NONE 01 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE +1 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 17 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS  NONE 1 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 1 000000000000002a 10.0.0.2 9000 ",
        "neighbours NONE 1 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 1 00000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 1 000000000000002a 10.0.0.02 9000",
        "NEIGHBOURS NONE 1 000000000000002a 10.0.0.2 09000",
        "NEIGHBOURS 000000000000002a 10.0.0.2 1 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS 000000000000002a 1 000000000000002a 10.0.0.2 9000",
        "NEIGHBOURS NONE 1 000000000000002a localhost 9000",
        "OWNER 000000000000002a 10.0.0.2 9000",
    };
    int rejected = 0;
    for (const std::string &h : hostile) {
        Neighbours untouched;
        untouched.has_pred = true; untouched.pred = a;
        std::string w;
        if (!parse_neighbours(h, untouched, w) && !w.empty() && untouched.has_pred && same_node(untouched.pred, a) &&
            untouched.succs.empty())
            ++rejected;
        else
            std::printf("        accepted or touched: \"%s\"\n", h.c_str());
    }
    check(rejected == static_cast<int>(hostile.size()),
          std::to_string(hostile.size()) + " malformed NEIGHBOURS replies rejected, output untouched");

    // The longest possible reply -- a predecessor and MAX_SUCCESSORS peers at the
    // widest address -- must fit in one line, or a correct node could not send it.
    Neighbours widest;
    const Peer w = Peer{~static_cast<Id>(0), "255.255.255.255", 65535};
    widest.has_pred = true; widest.pred = w;
    widest.succs.assign(MAX_SUCCESSORS, w);
    check(render_neighbours(widest).size() <= MAX_LINE,
          "the widest NEIGHBOURS reply (" + std::to_string(render_neighbours(widest).size()) +
          " bytes) fits in MAX_LINE");
}

void test_merge() {
    std::printf("merging a successor's list\n");
    const Peer me = make_peer(100, 1), s1 = make_peer(200, 2), s2 = make_peer(300, 3), s3 = make_peer(400, 4);
    const Peer twin = make_peer(100, 9);       // my identifier, another address

    check(same_list(merge_successors(me, s1, {s2, s3}, 8), {s1, s2, s3}), "successor first, then its list");
    check(same_list(merge_successors(me, s1, {s2, me, s3}, 8), {s1, s2}), "cut where the list wraps back to me");
    check(same_list(merge_successors(me, s1, {s2, s3}, 2), {s1, s2}), "truncated at r");
    check(same_list(merge_successors(me, s1, {s2, s3}, 1), {s1}), "r = 1 keeps only the successor");
    check(same_list(merge_successors(me, s1, {s1, s2, s2, s3}, 8), {s1, s2, s3}), "no repeats");
    check(same_list(merge_successors(me, s1, {twin, s2}, 8), {s1, s2}),
          "a peer carrying my identifier at another address is never adopted (D-028)");
    check(same_list(merge_successors(me, me, {me}, 8), {me}), "alone: the list is just me");
}

// ---------------------------------------------------------------------------
// Requests, one at a time

void test_requests() {
    std::printf("requests\n");
    Sim sim;
    const Peer a = make_peer(1000, 1), b = make_peer(2000, 2), c = make_peer(3000, 3), x = make_peer(1500, 4);
    std::vector<Peer> ring = wired_ring(sim, {a, b, c});
    NodeState &sb = sim.at(b);

    check(handle_request(sb, "PING", sim.transport).reply == "PONG " + id_to_hex(b.id), "PING");
    check(handle_request(sb, "", sim.transport).reply.compare(0, 4, "ERR ") == 0, "an empty request is ERR");
    check(handle_request(sb, "PING extra", sim.transport).reply.compare(0, 4, "ERR ") == 0, "extra words are ERR");
    check(handle_request(sb, "NOTIFY 1", sim.transport).reply.compare(0, 4, "ERR ") == 0, "short NOTIFY is ERR");
    check(handle_request(sb, "FINGERS please", sim.transport).reply.compare(0, 4, "ERR ") == 0, "FINGERS takes no argument");
    check(handle_request(sb, "FIND_SUCCESSOR " + id_to_hex(2500), sim.transport).reply == "OWNER " + peer_words(c),
          "FIND_SUCCESSOR answers from the successor, as in Phase 2");

    // NOTIFY from a node that is not closer, while the predecessor is alive: nothing changes.
    long before = sim.calls;
    Handled h = handle_request(sb, "NOTIFY " + peer_words(c), sim.transport);
    if (h.after) h.after();
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.pred, a), "a farther notifier does not replace a live predecessor"); }
    check(sim.calls == before + 1, "...but the live predecessor was probed once");

    // NOTIFY from a closer node: adopted at once, no probe.
    sim.add(x);
    before = sim.calls;
    h = handle_request(sb, "NOTIFY " + peer_words(x), sim.transport);
    check(h.reply == "OK" && !h.after, "a closer notifier is adopted without a probe");
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.pred, x) && sim.calls == before, "predecessor is now the closer node"); }

    // The predecessor dies; a farther notifier replaces it, but only after the probe is refused.
    sim.dead.insert(addr(x));
    h = handle_request(sb, "NOTIFY " + peer_words(a), sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.pred, x), "the reply is sent before the probe runs"); }
    check(static_cast<bool>(h.after), "a probe is deferred until after the reply");
    if (h.after) h.after();
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.pred, a), "a refused predecessor is replaced by the notifier"); }

    // NOTIFY carrying my own identifier at another address is refused (D-028).
    const Peer twin = make_peer(b.id, 7);
    { std::lock_guard<std::mutex> l(sb.mu); sb.has_pred = false; }
    handle_request(sb, "NOTIFY " + peer_words(twin), sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(!sb.has_pred, "a notifier with my identifier is never adopted"); }
    handle_request(sb, "NOTIFY " + peer_words(b), sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(!sb.has_pred, "a node never becomes its own predecessor"); }
    { std::lock_guard<std::mutex> l(sb.mu); sb.has_pred = true; sb.pred = a; }

    // NEIGHBOURS is one snapshot of predecessor and list.
    Neighbours n;
    std::string why;
    check(parse_neighbours(handle_request(sb, "NEIGHBOURS", sim.transport).reply, n, why) && n.has_pred &&
          same_node(n.pred, a) && n.succs.size() == 2 && same_node(n.succs[0], c),
          "NEIGHBOURS returns the predecessor and the successor list");

    // SUSPECT: a node we do not hold is not even probed.
    before = sim.calls;
    check(handle_request(sb, "SUSPECT 10.9.9.9 9000", sim.transport).reply == "OK" && sim.calls == before,
          "SUSPECT naming an unknown node: OK, and no probe");
    check(handle_request(sb, "SUSPECT 10.0.0.03 9000", sim.transport).reply == "OK" && sim.calls == before,
          "SUSPECT with a non-canonical address matches nothing");
    // A live suspect is probed and kept.
    handle_request(sb, "SUSPECT " + c.ip + " " + std::to_string(c.port), sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.succs[0], c) && sim.calls == before + 1,
                                                  "SUSPECT naming a live node: probed once, nothing evicted"); }
    // A frozen suspect: a timeout from a hint changes nothing, not even a strike.
    sim.frozen.insert(addr(c));
    handle_request(sb, "SUSPECT " + c.ip + " " + std::to_string(c.port), sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.succs[0], c) && sb.strikes.count(strike_key(c)) == 0,
                                                  "SUSPECT naming a frozen node: no eviction and no strike"); }
    sim.frozen.erase(addr(c));
    // A dead suspect is evicted on the node's own refused probe.
    sim.dead.insert(addr(c));
    handle_request(sb, "SUSPECT " + c.ip + " " + std::to_string(c.port), sim.transport);
    {
        std::lock_guard<std::mutex> l(sb.mu);
        bool in_fingers = false;
        for (const Peer &f : sb.fingers) in_fingers = in_fingers || same_node(f, c);
        check(!same_node(sb.succs[0], c) && !in_fingers, "SUSPECT naming a dead node: evicted from list and fingers");
    }

    // FINGERS: one entry per run, and a reply too long for one line is refused.
    NodeState lone(a, 8);
    init_alone(lone);
    check(handle_request(lone, "FINGERS", sim.transport).reply == "FINGERS 1 0 " + peer_words(a),
          "FINGERS of a lone node is one run of itself");
    {
        std::lock_guard<std::mutex> l(lone.mu);
        for (int i = 0; i < ID_BITS; ++i) lone.fingers[i] = make_peer(static_cast<Id>(i) + 1, 100 + i);
    }
    check(handle_request(lone, "FINGERS", sim.transport).reply.compare(0, 4, "ERR ") == 0,
          "64 distinct fingers do not fit in one line, and are refused rather than truncated");
    (void)ring;
}

// ---------------------------------------------------------------------------
// Joins

void test_small_rings() {
    std::printf("rings of one and two\n");
    std::mt19937_64 rng(7);
    Sim sim;
    const Peer a = make_peer(0x8000000000000000ULL, 1), b = make_peer(0x10, 2);
    init_alone(sim.add(a));
    sim.round({a});
    check(all_correct(judge(sim, {a})), "a lone node is correct after a round");
    const LookupResult r = lookup(a, 12345, sim.transport);
    check(r.ok() && same_node(r.owner, a), "a lone node owns every key");

    NodeState &sb = sim.add(b);
    std::string why;
    check(join(sb, a, sim.transport, why) == JoinStatus::Joined, "a second node joins a ring of one");
    const int n = rounds_until(sim, {a, b}, rng, 10, all_correct);
    check(n >= 0 && n <= 3, "a two-node ring is fully correct within 3 rounds (took " + std::to_string(n) + ")");
    check(wrong_lookups(sim, {a, b}, {0, 0x10, 0x11, 0x8000000000000000ULL, ~0ULL}) == 0,
          "both nodes route every key to the right owner");
}

void test_sequential_and_mass_joins() {
    std::printf("joins: one at a time, and all at once\n");
    for (int mass = 0; mass <= 1; ++mass) {
        std::mt19937_64 rng(100 + mass);
        Sim sim;
        const std::vector<Peer> peers = random_peers(rng, 32);
        init_alone(sim.add(peers[0]));
        std::vector<Peer> in{peers[0]};
        int retries = 0;
        for (std::size_t i = 1; i < peers.size(); ++i) {
            NodeState &s = sim.add(peers[i]);
            sim.dead.insert(addr(peers[i]));             // not listening until joined (D-029)
            std::string why;
            const Peer known = in[rng() % in.size()];
            int attempts = 0;
            while (join(s, known, sim.transport, why) != JoinStatus::Joined && ++attempts < JOIN_ATTEMPTS) {
                ++retries;
                sim.round(in);
            }
            sim.dead.erase(addr(peers[i]));
            in.push_back(peers[i]);
            if (!mass) sim.round(in);                    // one round between joins, or none at all
        }
        const int n = rounds_until(sim, in, rng, 400, all_correct);
        const std::string label = mass ? "32 joins with no rounds between them" : "32 joins, one round apart";
        check(n >= 0, label + ": converge to the oracle's state (took " + std::to_string(n) + " rounds, " +
                          std::to_string(retries) + " join retries)");
        std::vector<Id> keys;
        for (int k = 0; k < 16; ++k) keys.push_back(rng());
        for (const Peer &p : in) keys.push_back(p.id);
        check(wrong_lookups(sim, in, keys) == 0, label + ": every lookup from every node reaches the right owner");
    }
}

void test_concurrent_joins_same_pair() {
    std::printf("two joins between the same pair, every round order\n");
    const Peer a = make_peer(100, 1), c = make_peer(1000, 3), b1 = make_peer(400, 4), b2 = make_peer(600, 5);
    std::vector<Peer> order = {a, c, b1, b2};
    std::sort(order.begin(), order.end(), [](const Peer &x, const Peer &y) { return addr(x) < addr(y); });
    int orders = 0, converged = 0, worst = 0;
    do {
        Sim sim;
        wired_ring(sim, {a, c});
        std::string why;
        // Both look up before either is reachable: both are told C is their successor.
        NodeState &s1 = sim.add(b1);
        NodeState &s2 = sim.add(b2);
        sim.dead.insert(addr(b1)); sim.dead.insert(addr(b2));
        const bool j1 = join(s1, a, sim.transport, why) == JoinStatus::Joined;
        const bool j2 = join(s2, c, sim.transport, why) == JoinStatus::Joined;
        sim.dead.erase(addr(b1)); sim.dead.erase(addr(b2));
        int n = -1;
        for (int round = 0; round <= 8 && j1 && j2; ++round) {
            if (all_correct(judge(sim, {a, b1, b2, c}))) { n = round; break; }
            sim.round(order);                            // the same fixed order every round
        }
        ++orders;
        if (n >= 0) ++converged;
        worst = std::max(worst, n);
    } while (std::next_permutation(order.begin(), order.end(),
                                   [](const Peer &x, const Peer &y) { return addr(x) < addr(y); }));
    check(orders == 24 && converged == 24,
          "all 24 round orders converge (worst " + std::to_string(worst) + " rounds)");
}

void test_fast_rejoin() {
    std::printf("a node restarted at its old address before the ring notices\n");
    std::mt19937_64 rng(11);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 12));
    const Peer x = ring[5];
    sim.dead.insert(addr(x));                            // killed...
    NodeState &fresh = sim.add(x);                       // ...and restarted with empty state,
    sim.dead.insert(addr(x));                            // not listening until it has joined
    std::string why;
    const JoinStatus first = join(fresh, ring[0], sim.transport, why);
    check(first == JoinStatus::Retry, "the first join attempt is refused: the ring still names this address");
    int attempts = 1;
    while (join(fresh, ring[0], sim.transport, why) != JoinStatus::Joined && attempts++ < JOIN_ATTEMPTS)
        sim.round(without(ring, x));
    check(attempts < JOIN_ATTEMPTS, "the join succeeds once the stale entry has been evicted (attempt " +
                                    std::to_string(attempts) + ")");
    sim.dead.erase(addr(x));                             // now it listens
    const int n = rounds_until(sim, ring, rng, 100, all_correct);
    check(n >= 0, "the ring converges with the restarted node back in it (" + std::to_string(n) + " rounds)");
}

void test_collision() {
    std::printf("identifier collisions (D-028)\n");
    std::mt19937_64 rng(13);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 8));
    const Peer twin = make_peer(ring[3].id, 50);         // ring[3]'s identifier, another address
    NodeState &t = sim.add(twin);
    sim.dead.insert(addr(twin));
    std::string why;
    check(join(t, ring[0], sim.transport, why) == JoinStatus::Collision && why.find(addr(ring[3])) != std::string::npos,
          "a join whose identifier is taken is refused, naming the holder");

    // The twin gets in anyway (as if two joined at once). Nobody may adopt it as
    // successor or predecessor of the node it collides with.
    sim.dead.erase(addr(twin));
    { std::lock_guard<std::mutex> l(t.mu); t.succs = {ring[4]}; t.fingers.assign(ID_BITS, ring[4]); }
    std::vector<Peer> order = ring;
    order.push_back(twin);
    for (int i = 0; i < 10; ++i) sim.round(order);
    NodeState &holder = sim.at(ring[3]);
    std::lock_guard<std::mutex> l(holder.mu);
    bool adopted = same_node(holder.succs[0], twin) || (holder.has_pred && same_node(holder.pred, twin));
    for (const Peer &f : holder.fingers) adopted = adopted || same_node(f, twin);
    check(!adopted, "the colliding node never appears in the holder's tables");
    check(count_events(sim, "carries my identifier") > 0, "...and the refusals are logged");
}

// ---------------------------------------------------------------------------
// Failures

void test_kill_one() {
    std::printf("one node killed in a correct ring\n");
    int ring_worst = 0, list_worst = 0, finger_worst = 0;
    bool all_ok = true;
    for (int seed = 0; seed < 20; ++seed) {
        std::mt19937_64 rng(200 + seed);
        Sim sim;
        std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 32));
        if (!all_correct(judge(sim, ring))) { all_ok = false; continue; }
        const Peer x = ring[rng() % ring.size()];
        sim.dead.insert(addr(x));
        const std::vector<Peer> live = without(ring, x);
        const int nr = rounds_until(sim, live, rng, 200, [](const Verdict &v) { return v.ring; });
        const int nl = rounds_until(sim, live, rng, 200, [](const Verdict &v) { return v.ring && v.lists; });
        const int nf = rounds_until(sim, live, rng, 200, all_correct);
        if (nr < 0 || nl < 0 || nf < 0) all_ok = false;
        ring_worst = std::max(ring_worst, nr);
        list_worst = std::max(list_worst, nr + nl);
        finger_worst = std::max(finger_worst, nr + nl + nf);
    }
    check(all_ok, "20 rings of 32: all three milestones reached after every kill");
    check(ring_worst == 1, "successor and predecessor correct after exactly one round, every time (worst " +
                           std::to_string(ring_worst) + ")");
    check(list_worst <= DEFAULT_SUCCESSORS, "successor lists correct within r rounds (worst " +
                                            std::to_string(list_worst) + ")");
    std::printf("  info  fingers correct within %d rounds at worst (32 nodes, 20 kills)\n", finger_worst);
}

void test_kill_adjacent() {
    std::printf("adjacent nodes killed together\n");
    for (int k : {2, 4, 7}) {
        std::mt19937_64 rng(300 + k);
        Sim sim;
        std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 32));
        std::vector<Peer> live = ring;
        for (int i = 0; i < k; ++i) { sim.dead.insert(addr(ring[10 + i])); live = without(live, ring[10 + i]); }
        const int nr = rounds_until(sim, live, rng, 50, [](const Verdict &v) { return v.ring; });
        check(nr == 1, std::to_string(k) + " adjacent deaths (fewer than r): ring correct after one round (took " +
                       std::to_string(nr) + ")");
        check(rounds_until(sim, live, rng, 200, all_correct) >= 0, std::to_string(k) + " adjacent deaths: fully converged");
    }

    // r adjacent deaths: the predecessor's whole list is gone. It must fall back
    // to a node it knows -- never to itself -- and the ring must still repair.
    std::mt19937_64 rng(399);
    Sim sim(4);
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 32));
    std::vector<Peer> live = ring;
    for (int i = 0; i < 4; ++i) { sim.dead.insert(addr(ring[10 + i])); live = without(live, ring[10 + i]); }
    bool ever_alone = false;
    int n = -1;
    for (int round = 0; round <= 200; ++round) {
        if (all_correct(judge(sim, live))) { n = round; break; }
        std::vector<Peer> order = live;
        std::shuffle(order.begin(), order.end(), rng);
        sim.round(order);
        for (const Peer &p : live) {
            NodeState &s = sim.at(p);
            std::lock_guard<std::mutex> l(s.mu);
            ever_alone = ever_alone || same_node(s.succs[0], s.self);
        }
    }
    check(count_events(sim, "successor list exhausted") > 0, "r = 4 adjacent deaths exhaust a successor list");
    check(!ever_alone, "...and no node ever falls back to itself, which would claim the whole ring");
    check(n >= 0, "...and the ring still converges, from the fingers (" + std::to_string(n) + " rounds)");
}

void test_walk_after_exhausted_list() {
    std::printf("more adjacent deaths than the list holds: the walk back is done within a round (D-043)\n");
    int worst = 0;
    for (int seed = 0; seed < 10; ++seed) {
        std::mt19937_64 rng(500 + seed);
        Sim sim(4);
        std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 64));
        std::vector<Peer> live = ring;
        for (int i = 0; i < 6; ++i) { sim.dead.insert(addr(ring[10 + i])); live = without(live, ring[10 + i]); }
        const int n = rounds_until(sim, live, rng, 200, [](const Verdict &v) { return v.ring; });
        worst = std::max(worst, n < 0 ? 999 : n);
    }
    std::printf("  info  6 adjacent deaths, r = 4: ring correct after %d rounds at worst (10 rings of 64)\n", worst);
    check(worst <= 3, "6 adjacent deaths with r = 4, 10 rings of 64: ring correct within 3 rounds every time "
                      "(worst " + std::to_string(worst) + ")");
}

void test_mass_start() {
    std::printf("a wired ring whose nodes come up at different times (D-043)\n");
    int worst = 0;
    for (int seed = 0; seed < 10; ++seed) {
        std::mt19937_64 rng(600 + seed);
        Sim sim;
        std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 128));
        // Half the nodes are not listening yet for the first three rounds: the
        // others see them refused, evict them, and fall back past them.
        std::vector<Peer> early, late;
        for (const Peer &p : ring) (rng() % 2 ? early : late).push_back(p);
        for (const Peer &p : late) sim.dead.insert(addr(p));
        for (int r = 0; r < 3; ++r) sim.round(early);
        for (const Peer &p : late) sim.dead.erase(addr(p));
        const int n = rounds_until(sim, ring, rng, 400, all_correct);
        worst = std::max(worst, n < 0 ? 999 : n);
    }
    std::printf("  info  half the nodes late: fully correct after %d rounds at worst (10 rings of 128)\n", worst);
    check(worst <= 20, "10 rings of 128 with half the nodes late: fully correct within 20 rounds every time "
                       "(worst " + std::to_string(worst) + ")");
}

void test_frozen() {
    std::printf("a frozen node: two strikes, then back again\n");
    std::mt19937_64 rng(17);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 16));
    const Peer p = ring[4], x = ring[5];

    sim.frozen.insert(addr(x));
    stabilize(sim.at(p), sim.transport);
    { NodeState &s = sim.at(p); std::lock_guard<std::mutex> l(s.mu);
      check(same_node(s.succs[0], x) && s.strikes[strike_key(x)] == 1, "one timeout is one strike, not an eviction"); }
    sim.frozen.erase(addr(x));
    stabilize(sim.at(p), sim.transport);
    { NodeState &s = sim.at(p); std::lock_guard<std::mutex> l(s.mu);
      check(s.strikes.count(strike_key(x)) == 0, "a valid reply clears the strike"); }

    sim.frozen.insert(addr(x));
    const std::vector<Peer> live = without(ring, x);
    const int n = rounds_until(sim, live, rng, 50, [](const Verdict &v) { return v.ring; });
    check(n == 2, "frozen for good: evicted at the second strike, ring correct after 2 rounds (took " +
                  std::to_string(n) + ")");
    check(count_events(sim, "evict " + addr(x) + " (timeout, second strike)") > 0, "the eviction names the second strike");
    const int all = rounds_until(sim, live, rng, 100, all_correct);
    check(all >= 0, "frozen for good: every finger that pointed at it is repaired too (" + std::to_string(n + all) +
                    " rounds) -- a finger's own repair lookup routes through the frozen node and times out, so "
                    "timeouts must count against fingers as well");

    sim.frozen.erase(addr(x));                           // it was only slow: it re-inserts itself
    const int back = rounds_until(sim, ring, rng, 100, all_correct);
    check(back >= 0, "thawed: it re-inserts itself through its own stabilise and notify, no message needed (" +
                     std::to_string(back) + " rounds)");
}

void test_stalled_finger_cycle() {
    std::printf("a finger whose repair lookup keeps timing out must not stall the others\n");
    std::mt19937_64 rng(37);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 64));
    const Peer q = ring[0];
    NodeState &s = sim.at(q);
    // y: beyond q's 8 successors (and so not its predecessor either), with a
    // finger start i just past it and room for finger i + 1 after that.
    Peer y;
    int i = ID_BITS;
    for (std::size_t k = 9; k < ring.size() && i >= ID_BITS - 1; ++k) {
        y = ring[k];
        i = 1;
        while (i < ID_BITS && !in_range_oo(y.id, q.id, finger_start(q.id, i))) ++i;
    }
    check(i > 1 && i < ID_BITS - 1, "there is a finger start just past y (index " + std::to_string(i) + ")");
    if (!(i > 1 && i < ID_BITS - 1)) return;            // the writes below would run off the table
    {
        // A stale finger i-1 naming y, which then freezes: q's lookup for finger i
        // is routed through y and times out. Finger i+1 is made wrong too, to see
        // whether the cycle ever gets past finger i to repair it.
        std::lock_guard<std::mutex> l(s.mu);
        s.fingers[i - 1] = y;
        s.fingers[i + 1] = ring[1];
        s.next_finger = i;
    }
    sim.frozen.insert(addr(y));
    const std::vector<Peer> live = without(ring, y);
    const int n = rounds_until(sim, live, rng, 300, all_correct);
    check(n >= 0, "the cycle moves past the failing finger, the frozen target is struck out, and the whole "
                  "table is repaired (" + std::to_string(n) + " rounds)");
}

void test_dies_mid_join() {
    std::printf("a node dying between its notify and its predecessor's stabilise\n");
    std::mt19937_64 rng(19);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 8));
    // A new node between ring[2] and ring[3].
    const Id mid = ring[2].id + (ring[3].id - ring[2].id) / 2;
    const Peer b = make_peer(mid, 60);
    NodeState &sb = sim.add(b);
    std::string why;
    check(join(sb, ring[0], sim.transport, why) == JoinStatus::Joined, "the new node joins");
    stabilize(sb, sim.transport);                        // notifies ring[3]: its predecessor is now b
    { NodeState &s3 = sim.at(ring[3]); std::lock_guard<std::mutex> l(s3.mu);
      check(s3.has_pred && same_node(s3.pred, b), "its successor accepted it as predecessor"); }
    sim.dead.insert(addr(b));                            // and it dies before ring[2] hears of it

    stabilize(sim.at(ring[2]), sim.transport);
    { NodeState &s2 = sim.at(ring[2]); std::lock_guard<std::mutex> l(s2.mu);
      check(same_node(s2.succs[0], ring[3]), "the predecessor verifies the candidate and does not adopt a dead node"); }
    const int n = rounds_until(sim, ring, rng, 20, all_correct);
    check(n >= 0 && n <= 2, "the ring converges without it within 2 rounds (took " + std::to_string(n) + ")");
}

void test_orphan() {
    std::printf("a newcomer whose whole neighbourhood dies: orphaned, then rejoined (D-040)\n");
    std::mt19937_64 rng(31);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 16));
    const Id mid = ring[5].id + (ring[6].id - ring[5].id) / 2;
    const Peer b = make_peer(mid, 70);
    NodeState &sb = sim.add(b);
    std::string why;
    check(join(sb, ring[0], sim.transport, why) == JoinStatus::Joined, "the newcomer joins through ring[0]");
    { std::lock_guard<std::mutex> l(sb.mu); check(same_node(sb.succs[0], ring[6]) && sb.succs.size() == 8,
                                                  "at join it copies its successor's list: 8 nodes, not 1"); }
    // Every node it holds -- its successor and the seven after it -- dies before
    // its first round. Only the node it joined through is still known and alive.
    std::vector<Peer> live;
    for (int i = 0; i < 16; ++i) {
        if (i >= 6 && i <= 13) sim.dead.insert(addr(ring[i]));
        else live.push_back(ring[i]);
    }
    stabilize(sb, sim.transport);
    { std::lock_guard<std::mutex> l(sb.mu); check(sb.orphaned, "with no node left that it knows, it is orphaned -- not alone"); }
    check(handle_request(sb, "FIND_SUCCESSOR " + id_to_hex(12345), sim.transport).reply.compare(0, 13, "ERR orphaned:") == 0,
          "an orphan refuses to route instead of claiming every key");
    check(handle_request(sb, "NEIGHBOURS", sim.transport).reply.compare(0, 4, "ERR ") == 0 &&
          handle_request(sb, "NOTIFY " + peer_words(ring[0]), sim.transport).reply.compare(0, 4, "ERR ") == 0,
          "...and refuses NEIGHBOURS and NOTIFY, so no joiner is recruited into a ring of one");
    check(handle_request(sb, "PING", sim.transport).reply.compare(0, 4, "ERR ") == 0,
          "...and refuses PING, so its neighbours' liveness checks forget it");
    live.push_back(b);
    const int n = rounds_until(sim, live, rng, 100, all_correct);
    check(n >= 0, "it rejoins through a remembered contact and the ring converges (" + std::to_string(n) + " rounds)");
    check(count_events(sim, "rejoined through " + addr(ring[0])) == 1, "the rejoin went through the node it first joined through");
}

void test_readoption_window() {
    std::printf("a neighbour's stale predecessor pointer must not re-introduce a dead node\n");
    std::mt19937_64 rng(23);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 8));
    const Peer p = ring[1], x = ring[2], s = ring[3];
    sim.dead.insert(addr(x));
    stabilize(sim.at(p), sim.transport);                 // evicts x, asks s -- whose predecessor is still x
    NodeState &sp = sim.at(p);
    std::lock_guard<std::mutex> l(sp.mu);
    check(same_node(sp.succs[0], s), "after one stabilise the successor is the live node, not the dead one again");
}

void test_suspect_recovery() {
    std::printf("a client reports an unreachable hop, then retries (D-036)\n");
    std::mt19937_64 rng(29);
    Sim sim;
    std::vector<Peer> ring = wired_ring(sim, random_peers(rng, 64));

    // Find a lookup whose path passes through some node in the middle.
    Peer start, dead;
    Id key = 0;
    bool found = false;
    for (int t = 0; t < 1000 && !found; ++t) {
        const Peer st = ring[rng() % ring.size()];
        const Id k = rng();
        const LookupResult r = lookup(st, k, sim.transport);
        if (r.ok() && r.path.size() >= 3) {
            Peer owner_of_k = ring[0];
            for (const Peer &q : ring) if (cw(k, q.id) < cw(k, owner_of_k.id)) owner_of_k = q;
            const Peer mid = r.path[1];
            if (!same_node(mid, owner_of_k)) { start = st; dead = mid; key = k; found = true; }
        }
    }
    check(found, "a lookup with an intermediate hop exists");
    if (!found) return;
    sim.dead.insert(addr(dead));
    const std::vector<Peer> live = without(ring, dead);
    Peer owner = live[0];
    for (const Peer &q : live) if (cw(key, q.id) < cw(key, owner.id)) owner = q;

    const LookupResult plain = lookup(start, key, sim.transport);
    check(!plain.ok() && plain.error == LookupError::Refused && same_node(plain.path.back(), dead),
          "without recovery the lookup fails at the dead node, naming it");
    const LookupResult none = lookup_with_recovery(start, key, sim.transport, 0);
    check(!none.ok() && none.attempts == 1, "retries = 0 is plain lookup()");

    const LookupResult rec = lookup_with_recovery(start, key, sim.transport, 2);
    check(rec.ok() && same_node(rec.owner, owner) && rec.attempts == 2,
          "with recovery: one SUSPECT, one retry, and the right owner (attempts " + std::to_string(rec.attempts) + ")");
    NodeState &namer = sim.at(plain.path[plain.path.size() - 2]);
    std::lock_guard<std::mutex> l(namer.mu);
    bool still = false;
    for (const Peer &f : namer.fingers) still = still || same_node(f, dead);
    check(!still, "the node that named the dead hop evicted it on its own probe");

    // The start node itself dead: nobody named it, so nothing is reported.
    const long before = sim.calls;
    const LookupResult self_dead = lookup_with_recovery(dead, key, sim.transport, 3);
    check(!self_dead.ok() && self_dead.attempts == 1 && sim.calls == before + 1,
          "a dead start node: no SUSPECT, no retry");
}

// An orphan is STRANDED if no address it remembers belongs to a running node
// that is not itself an orphan: it has nobody left to rejoin through (D-040).
bool stranded(Sim &sim, const Peer &p) {
    NodeState &s = sim.at(p);
    std::vector<Peer> contacts;
    {
        std::lock_guard<std::mutex> l(s.mu);
        if (!s.orphaned) return false;
        contacts = s.contacts;
    }
    for (const Peer &c : contacts) {
        if (!sim.runs(addr(c))) continue;
        NodeState &other = sim.at(c);
        std::lock_guard<std::mutex> l(other.mu);
        if (!other.orphaned) return false;
    }
    return true;
}

void test_churn_property() {
    std::printf("random churn, then quiet\n");
    int converged = 0, stranded_runs = 0, collapsed = 0, unexplained = 0, worst = 0, lookups_wrong = 0;
    bool never_alone = true;
    const int seeds = 200;
    for (int seed = 0; seed < seeds; ++seed) {
        std::mt19937_64 rng(1000 + seed);
        Sim sim;
        std::vector<Peer> pool = random_peers(rng, 48);
        std::vector<Peer> members = wired_ring(sim, std::vector<Peer>(pool.begin(), pool.begin() + 16));
        std::size_t next = 16;
        for (int step = 0; step < 120; ++step) {
            const int what = static_cast<int>(rng() % 10);
            if (what == 0 && next < pool.size()) {                       // join
                const Peer p = pool[next++];
                NodeState &s = sim.add(p);
                sim.dead.insert(addr(p));
                std::string why;
                const Peer known = members[rng() % members.size()];
                if (sim.runs(addr(known)) && join(s, known, sim.transport, why) == JoinStatus::Joined) {
                    sim.dead.erase(addr(p));
                    members.push_back(p);
                }
            } else if (what == 1 && members.size() > 4) {                // kill
                const Peer v = members[rng() % members.size()];
                sim.dead.insert(addr(v));
                sim.frozen.erase(addr(v));
                members = without(members, v);
            } else if (what == 2 && !members.empty()) {                  // freeze or thaw
                const std::string a = addr(members[rng() % members.size()]);
                if (sim.frozen.count(a)) sim.frozen.erase(a); else sim.frozen.insert(a);
            } else {                                                     // some nodes run a round
                std::vector<Peer> order = members;
                std::shuffle(order.begin(), order.end(), rng);
                order.resize(order.size() / 2 + 1);
                sim.round(order);
            }
            if (members.size() >= 2)
                for (const Peer &p : members) {
                    if (!sim.runs(addr(p))) continue;
                    NodeState &s = sim.at(p);
                    std::lock_guard<std::mutex> l(s.mu);
                    never_alone = never_alone && (s.orphaned || !same_node(s.succs[0], s.self));
                }
        }
        sim.frozen.clear();                                              // quiet: everyone answers
        const int n = rounds_until(sim, members, rng, 400, all_correct);
        if (n >= 0) {
            ++converged;
            worst = std::max(worst, n);
            std::vector<Id> keys;
            for (int k = 0; k < 8; ++k) keys.push_back(rng());
            lookups_wrong += wrong_lookups(sim, members, keys);
            continue;
        }
        // Not converged. The only acceptable reason is a stranded orphan.
        int orphans = 0, strand = 0;
        for (const Peer &p : members) {
            NodeState &s = sim.at(p);
            bool o;
            { std::lock_guard<std::mutex> l(s.mu); o = s.orphaned; }
            if (o) ++orphans;
            if (stranded(sim, p)) ++strand;
        }
        if (strand == 0) ++unexplained;
        else if (orphans == static_cast<int>(members.size())) ++collapsed;
        else ++stranded_runs;
    }
    std::printf("  info  %d/%d churn histories converged (worst %d quiet rounds); %d left a stranded orphan; "
                "in %d every survivor was orphaned\n", converged, seeds, worst, stranded_runs, collapsed);
    check(unexplained == 0, "every history converges to the oracle once quiet, unless an orphan is stranded "
                            "(" + std::to_string(unexplained) + " unexplained)");
    check(converged >= seeds * 9 / 10, "at least 90% of histories converge under this churn (" +
                                       std::to_string(converged) + "/" + std::to_string(seeds) + ")");
    check(lookups_wrong == 0, "after convergence every lookup reaches the right owner");
    check(never_alone, "no running node ever claimed the whole ring while others were in it "
                       "(an orphan refuses to route instead)");
}

}  // namespace

int main() {
    std::printf("test-stabilize: joins, stabilisation and failures, a whole ring in one process\n");
    test_wire();
    test_merge();
    test_requests();
    test_small_rings();
    test_sequential_and_mass_joins();
    test_concurrent_joins_same_pair();
    test_fast_rejoin();
    test_collision();
    test_kill_one();
    test_kill_adjacent();
    test_walk_after_exhausted_list();
    test_mass_start();
    test_frozen();
    test_stalled_finger_cycle();
    test_dies_mid_join();
    test_orphan();
    test_readoption_window();
    test_suspect_recovery();
    test_churn_property();
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
