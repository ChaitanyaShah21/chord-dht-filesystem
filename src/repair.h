#ifndef REPAIR_H
#define REPAIR_H

#include "chunk.h"

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Re-replication (decision D-050): getting every chunk back to RF copies on its
// RF distinct processes after the ring changes.
//
// Driven by OWNERS. Each round, for every member of this process with a known
// predecessor, the process compares the arc it owns and the processes that
// should hold that arc's replicas with what it last checked. If either changed,
// it asks those processes which of the arc's chunks they lack (HAVE) and sends
// them (STORE). Every `scan_every` rounds a full scan also re-hashes every local
// chunk (the scrub: a bad file is replaced by a verified copy read through the
// ring), re-checks every arc whether or not it changed, and hands each chunk this
// process holds but does not own to the chunk's owner as a PUT, so the owner
// replicates it to its whole replica set.
//
// Runs on its own thread in the node, so moving chunks never delays the
// maintenance round that keeps the ring itself correct.
// ---------------------------------------------------------------------------

// What one member's arc looked like the last time it was checked successfully.
struct ArcCheck {
    Id                       pred = 0;
    std::vector<std::string> holders;     // "ip:port" of each replica process, in order
};

struct RepairState {
    std::map<Id, ArcCheck> checked;       // by member identifier
    long                   round = 0;
};

// What one round did, for the log and the tests.
struct RepairReport {
    int arcs_checked  = 0;   // arcs whose chunks were offered to their replicas
    int chunks_sent   = 0;   // STOREs that succeeded
    int send_failures = 0;   // HAVE or STORE exchanges that failed (retried next round)
    int handed_off    = 0;   // chunks stored at an owner by the hand-off
    int scrubbed      = 0;   // local files removed because they no longer hash to their key
    int restored      = 0;   // of those, replaced by a verified copy read through the ring
    bool full_scan    = false;
};

// One repair round. `route` carries lookups (for the hand-off), `n.send` chunk
// frames. With scan_every <= 0 a full scan never happens.
RepairReport repair_round(ChunkNode &n, RepairState &st, const Transport &route, int scan_every);

#endif
