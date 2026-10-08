#include "repair.h"

#include <algorithm>

namespace {

std::string address(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

// A local copy that no longer hashes to its key: remove it, then fetch a good one
// through the ring -- a read that checks the hash itself, so only good bytes come
// back (D-049) -- rather than waiting for someone else to notice it is missing.
void replace_corrupt(ChunkNode &n, const Transport &route, const std::string &key, RepairReport &rep) {
    store_remove(*n.store, key);
    ++rep.scrubbed;
    const GetOutcome good = get_chunk(n.members.front()->self, key, route, n.send);
    if (good.ok && store_put(*n.store, key, good.body) == PutResult::Stored) ++rep.restored;
}

// Ask the process `to` which of `keys` it lacks, MAX_HAVE_KEYS at a time, and
// send each one it lacks: as STORE (keep one copy) to a replica, or as PUT (keep
// it and replicate it, D-046) to an owner receiving a hand-off, so the owner's
// whole replica set gets it at once. Returns false if any exchange failed, so the
// caller tries again next round. A local copy that no longer hashes to its key is
// replaced instead of sent: repair must never spread a corrupt chunk.
bool offer(ChunkNode &n, const Transport &route, const Peer &to, const std::vector<std::string> &keys,
           RepairReport &rep, bool as_put = false) {
    bool ok = true;
    for (std::size_t i = 0; i < keys.size(); i += MAX_HAVE_KEYS) {
        const std::size_t end = std::min(keys.size(), i + MAX_HAVE_KEYS);
        std::string line = "HAVE";
        for (std::size_t j = i; j < end; ++j) line += " " + keys[j];

        Frame answer;
        const Delivery d = n.send(to, line, "", answer);
        // Exactly one digit per key asked, or the answer is not believed.
        if (d != Delivery::Ok || answer.line.compare(0, 5, "HAVE ") != 0 || answer.line.size() != 5 + (end - i)) {
            ++rep.send_failures;
            ok = false;
            continue;
        }
        for (std::size_t j = i; j < end; ++j) {
            if (answer.line[5 + (j - i)] != '0') continue;          // it has this one
            std::string body;
            if (store_get(*n.store, keys[j], body) != GetResult::Found) continue;   // gone since listed
            if (chunk_key(body) != keys[j]) {
                replace_corrupt(n, route, keys[j], rep);
                ok = false;                        // offer it again next round, from the good copy
                continue;
            }
            Frame stored;
            const std::string size = std::to_string(body.size());
            const Delivery s = n.send(to, as_put ? "PUT " + keys[j] + " 1 " + size : "STORE " + keys[j] + " " + size,
                                      body, stored);
            if (s == Delivery::Ok && (stored.line == "OK" || stored.line.compare(0, 3, "OK ") == 0)) ++rep.chunks_sent;
            else { ++rep.send_failures; ok = false; }
        }
    }
    return ok;
}

// Re-hash every local chunk. A file that no longer matches its name, or that the
// index lists but the disk no longer has, is removed and replaced by a copy read
// through the ring -- a read that checks the hash itself, so only good bytes come
// back (D-049) -- rather than waiting for the owner's next full check (D-050).
void scrub(ChunkNode &n, const Transport &route, RepairReport &rep) {
    for (const std::string &key : store_keys(*n.store)) {
        std::string body;
        const GetResult g = store_get(*n.store, key, body);
        if (g == GetResult::Found && chunk_key(body) == key) continue;
        replace_corrupt(n, route, key, rep);
    }
}

// Give every chunk this process holds but does not own to its owner. Keys are
// taken in order, and one lookup answers for every key up to the owner it names:
// the member that answered OWNER o owns nothing, and o owns everything, in
// (that member, o].
void hand_off(ChunkNode &n, const Transport &route, const std::vector<std::pair<Id, Id>> &owned,
              RepairReport &rep) {
    std::vector<std::string> foreign;
    for (const std::string &key : store_keys(*n.store)) {
        Id k = 0;
        id_from_key(key, k);
        bool ours = false;
        for (const auto &arc : owned) ours = ours || in_range_oc(k, arc.first, arc.second);
        if (!ours) foreign.push_back(key);
    }

    const Peer start = n.members.front()->self;
    std::size_t i = 0;
    while (i < foreign.size()) {
        Id k = 0;
        id_from_key(foreign[i], k);
        const LookupResult r = lookup(start, k, route);
        if (!r.ok()) { ++rep.send_failures; ++i; continue; }

        std::vector<std::string> batch{foreign[i++]};
        const Id from = r.path.back().id;
        while (i < foreign.size()) {
            Id next = 0;
            id_from_key(foreign[i], next);
            if (!in_range_oc(next, from, r.owner.id)) break;
            batch.push_back(foreign[i++]);
        }
        if (same_process(r.owner, start)) continue;   // one of our own members owns them after all
        const int before = rep.chunks_sent;
        offer(n, route, r.owner, batch, rep, true);
        rep.handed_off += rep.chunks_sent - before;
    }
}

}  // namespace

RepairReport repair_round(ChunkNode &n, RepairState &st, const Transport &route, int scan_every) {
    RepairReport rep;
    if (!n.store || n.members.empty()) return rep;
    ++st.round;
    rep.full_scan = scan_every > 0 && st.round % scan_every == 0;
    if (rep.full_scan) scrub(n, route, rep);           // first, so nothing corrupt is offered

    std::vector<std::pair<Id, Id>> owned;              // arcs (predecessor, member] we own
    for (NodeState *m : n.members) {
        bool has_pred, orphaned;
        Id pred;
        {
            std::lock_guard<std::mutex> lock(m->mu);
            has_pred = m->has_pred;
            pred     = m->pred.id;
            orphaned = m->orphaned;
        }
        if (!has_pred || orphaned) continue;           // our arc is unknown: own nothing yet
        owned.push_back({pred, m->self.id});

        const std::vector<Peer> set = replica_set(*m);
        ArcCheck now;
        now.pred = pred;
        for (std::size_t i = 1; i < set.size(); ++i) now.holders.push_back(address(set[i]));

        // Level-triggered: compare with what was last checked, not "was I told of
        // a change". A change that happened while we were busy is still a change.
        auto it = st.checked.find(m->self.id);
        const bool changed = it == st.checked.end() || it->second.pred != now.pred ||
                             it->second.holders != now.holders;
        if (!changed && !rep.full_scan) continue;

        const std::vector<std::string> keys = store_keys_in_arc(*n.store, pred, m->self.id);
        bool ok = true;
        for (std::size_t i = 1; i < set.size(); ++i) ok = offer(n, route, set[i], keys, rep) && ok;
        ++rep.arcs_checked;
        if (ok) st.checked[m->self.id] = now;          // done: until the arc or the set changes again
        else    st.checked.erase(m->self.id);          // something failed: check again next round
    }

    if (rep.full_scan) hand_off(n, route, owned, rep);

    if (n.log && (rep.chunks_sent || rep.send_failures || rep.scrubbed))
        n.log("repair: " + std::to_string(rep.arcs_checked) + " arcs checked, " + std::to_string(rep.chunks_sent) +
              " chunks sent (" + std::to_string(rep.handed_off) + " handed off), " +
              std::to_string(rep.send_failures) + " failures, " + std::to_string(rep.scrubbed) + " scrubbed, " +
              std::to_string(rep.restored) + " restored" +
              (rep.full_scan ? ", full scan" : ""));
    return rep;
}
