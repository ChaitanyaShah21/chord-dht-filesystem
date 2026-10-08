// sim-balance -- how evenly chunks spread over processes, with and without virtual
// nodes (decision D-051).
//
//   ./sim-balance <processes> <vnodes> <keys> <address-sets> [<first-set>]
//       one line per address set: owned and stored load per process, summarised
//   ./sim-balance --predict <vnodes> <members-file> < keys
//       for one given ring: how many copies of the given keys each process should store
//
// Placement is a pure function of the addresses and the keys, so it is computed
// here with the node's own code: id_of_member() for every member's identifier and
// distinct_processes() over the owner and its successor list for every replica set
// -- exactly what a coordinating node does (D-047, D-048). The --predict form is
// checked against a real ring, where each process's store must hold exactly the
// number of chunks predicted.
//
// Address set s uses 127.2.<s % 250>.1, ports from 21000: the scheme the benchmarks
// use for real rings, so a simulated set can be started as real processes.
//
// Output of the first form, one line per address set:
//   P V set owned_max_over_mean owned_cv stored_max_over_mean stored_cv short
// where owned = keys a process owns (as primary), stored = copies it keeps (RF = 3),
// cv = coefficient of variation (standard deviation / mean) across processes, and
// short = fraction of keys whose owner's successor list held fewer than three
// distinct processes, so their replica set was smaller than three.

#include "chord.h"
#include "chunk.h"
#include "sha1.h"
#include "stabilize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

struct Ring {
    std::vector<Peer> members;            // sorted by identifier
    std::map<std::string, int> process;   // "ip:port" -> index
};

std::string where(const Peer &p) { return p.ip + ":" + std::to_string(p.port); }

Ring build(const std::vector<Peer> &processes, int v) {
    Ring r;
    for (std::size_t i = 0; i < processes.size(); ++i) {
        r.process[where(processes[i])] = static_cast<int>(i);
        for (int m = 0; m < v; ++m)
            r.members.push_back(Peer{id_of_member(processes[i].ip, processes[i].port, m), processes[i].ip,
                                     processes[i].port});
    }
    std::sort(r.members.begin(), r.members.end(), [](const Peer &a, const Peer &b) { return a.id < b.id; });
    return r;
}

// The replica set a node would compute for key k: the owner, then its successor
// list of DEFAULT_SUCCESSORS members, reduced to distinct processes by the node's
// own function.
std::vector<Peer> replicas(const Ring &r, Id k) {
    auto it = std::lower_bound(r.members.begin(), r.members.end(), k,
                               [](const Peer &p, Id key) { return p.id < key; });
    std::size_t i = it == r.members.end() ? 0 : static_cast<std::size_t>(it - r.members.begin());
    std::vector<Peer> list;
    const std::size_t n = std::min(r.members.size(), static_cast<std::size_t>(DEFAULT_SUCCESSORS + 1));
    for (std::size_t s = 0; s < n; ++s) list.push_back(r.members[(i + s) % r.members.size()]);
    return distinct_processes(list, RF);
}

void summarise(const std::vector<double> &load, double &max_over_mean, double &cv) {
    double sum = 0, mx = 0;
    for (double x : load) { sum += x; mx = std::max(mx, x); }
    const double mean = sum / load.size();
    double var = 0;
    for (double x : load) var += (x - mean) * (x - mean);
    var /= load.size();
    max_over_mean = mean > 0 ? mx / mean : 0;
    cv = mean > 0 ? std::sqrt(var) / mean : 0;
}

int usage(const char *argv0) {
    std::fprintf(stderr, "usage: %s <processes> <vnodes> <keys> <address-sets> [<first-set>]\n"
                         "       %s --predict <vnodes> <members-file> < keys\n", argv0, argv0);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--predict") {
        const int v = std::atoi(argv[2]);
        std::ifstream f(argv[3]);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<Peer> processes;
        std::string err;
        if (v < 1 || !parse_members(text, processes, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        const Ring r = build(processes, v);
        std::vector<long> stored(processes.size(), 0);
        std::string key;
        while (std::cin >> key) {
            Id k;
            if (!id_from_key(key, k)) continue;
            for (const Peer &p : replicas(r, k)) ++stored[r.process.at(where(p))];
        }
        for (const Peer &p : processes) std::printf("%s %ld\n", where(p).c_str(), stored[r.process.at(where(p))]);
        return 0;
    }

    if (argc != 5 && argc != 6) return usage(argv[0]);
    const int processes = std::atoi(argv[1]), v = std::atoi(argv[2]), keys = std::atoi(argv[3]),
              sets = std::atoi(argv[4]), first = argc == 6 ? std::atoi(argv[5]) : 0;
    if (processes < 1 || v < 1 || keys < 1 || sets < 1) return usage(argv[0]);

    for (int s = first; s < first + sets; ++s) {
        std::vector<Peer> procs;
        for (int i = 0; i < processes; ++i) {
            const std::string ip = "127.2." + std::to_string(s % 250) + ".1";
            procs.push_back(Peer{id_of_address(ip, 21000 + i), ip, 21000 + i});
        }
        const Ring r = build(procs, v);
        std::vector<double> owned(processes, 0), stored(processes, 0);
        int short_sets = 0;
        for (int k = 0; k < keys; ++k) {
            Id id;
            id_from_key(sha1("key-" + std::to_string(s) + "-" + std::to_string(k)), id);
            const std::vector<Peer> set = replicas(r, id);
            owned[r.process.at(where(set[0]))] += 1;
            for (const Peer &p : set) stored[r.process.at(where(p))] += 1;
            if (static_cast<int>(set.size()) < std::min(RF, processes)) ++short_sets;
        }
        double omm, ocv, smm, scv;
        summarise(owned, omm, ocv);
        summarise(stored, smm, scv);
        std::printf("%d %d %d %.4f %.4f %.4f %.4f %.6f\n", processes, v, s, omm, ocv, smm, scv,
                    static_cast<double>(short_sets) / keys);
    }
    return 0;
}
