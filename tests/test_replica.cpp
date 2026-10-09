// In-process tests for the tracker replica (replica.cpp, decision D-072): the lock,
// the ticker, the senders and the waiting requests, with real threads and real
// time. Three replicas run in this process; the network between them is a function
// that calls the other replica's handle() directly, unless the test has cut that
// link. test-raft checks the protocol against every history it can construct; this
// checks the code that drives it -- and is what ThreadSanitizer runs
// (make test-replica-tsan).

#include "raft.h"
#include "replica.h"
#include "store.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>
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

std::string key(int i) { return chunk_key("manifest " + std::to_string(i)); }

constexpr int N = 3;
constexpr Millis E = 100;

// The group: replicas, their disks (which survive a restart), and the links.
struct Group {
    std::vector<std::unique_ptr<MemoryStorage>> disks;
    std::vector<std::unique_ptr<Replica>> replicas;
    std::shared_mutex net;                       // shared: a call in progress; exclusive: a replica changing
    bool cut[N][N] = {};
    bool down[N] = {};
    std::atomic<int> fatal{0};
    std::vector<Address> addrs;
    int commit_wait;

    explicit Group(int commit_wait_ms = 600) : commit_wait(commit_wait_ms) {
        for (int i = 0; i < N; ++i) {
            addrs.push_back(Address{"127.0.0.1", 7000 + i});
            disks.push_back(std::make_unique<MemoryStorage>());
        }
        replicas.resize(N);
        for (int i = 0; i < N; ++i) boot(i);
    }
    ~Group() {
        for (int i = 0; i < N; ++i) stop(i);
    }

    void boot(int i) {
        ReplicaConfig c;
        c.group = addrs;
        c.id = i;
        c.election = E;
        c.commit_wait_ms = commit_wait;
        c.seed = 17 + static_cast<std::uint64_t>(i);
        c.log_transitions = false;
        auto r = std::make_unique<Replica>(c, *disks[i], disks[i]->durable,
            [this, i](int to, const std::string &line, const std::string &body, std::string &reply, int) {
                std::shared_lock<std::shared_mutex> lock(net);
                if (cut[i][to] || cut[to][i]) return Delivery::Timeout;
                if (down[to]) return Delivery::Refused;
                reply = replicas[to]->handle(line, body);
                return Delivery::Ok;
            });
        r->on_fatal = [this](const std::string &) { ++fatal; };
        r->start();
        std::unique_lock<std::shared_mutex> lock(net);
        replicas[i] = std::move(r);
        down[i] = false;
    }
    void stop(int i) {
        std::unique_ptr<Replica> gone;
        {
            std::unique_lock<std::shared_mutex> lock(net);     // waits for calls into it to finish
            if (down[i] || !replicas[i]) return;
            down[i] = true;
            gone = std::move(replicas[i]);
        }
        gone.reset();                                         // joins its threads, outside the lock
    }
    void isolate(int i, bool on) {
        std::unique_lock<std::shared_mutex> lock(net);
        for (int j = 0; j < N; ++j) cut[i][j] = cut[j][i] = on;
    }

    std::string ask(int i, const std::string &line) {
        Replica *r;
        {
            std::shared_lock<std::shared_mutex> lock(net);
            if (down[i]) return "DOWN";
            r = replicas[i].get();
        }
        return r->handle(line, "");
    }
    // STATUS <id> <role> <term> <leader> <commit> <applied> <last>
    std::vector<std::string> status(int i) {
        std::istringstream in(ask(i, "STATUS"));
        std::vector<std::string> w;
        std::string s;
        while (in >> s) w.push_back(s);
        return w;
    }
    // The one replica leading in the highest term, among those not excluded; -1 if none within `ms`.
    int leader(int ms, int exclude = -1) {
        for (int t = 0; t < ms; t += 10) {
            int best = -1;
            unsigned long long best_term = 0;
            for (int i = 0; i < N; ++i) {
                if (i == exclude) continue;
                const std::vector<std::string> w = status(i);
                if (w.size() == 8 && w[2] == "leader" && std::stoull(w[3]) >= best_term) {
                    best = i;
                    best_term = std::stoull(w[3]);
                }
            }
            if (best >= 0) return best;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void test_election_and_requests() {
    std::printf("-- three replicas elect a leader and serve\n");
    Group g;
    const int l = g.leader(3000);
    check(l >= 0, "a leader is elected");
    int leaders = 0;
    for (int i = 0; i < N; ++i) leaders += g.status(i)[2] == "leader";
    check(leaders == 1, "exactly one");
    const int f = (l + 1) % N;
    sleep_ms(3 * static_cast<int>(E));                        // the followers have heard from it
    check(g.ask(f, "REGISTER a.txt " + key(1)) == "REDIRECT " + g.addrs[l].text(), "a follower redirects a write to the leader");
    check(g.ask(f, "RESOLVE a.txt") == "REDIRECT " + g.addrs[l].text(), "and a read");
    check(g.ask(f, "PING") == "PONG", "but answers PING itself");
    check(g.ask(l, "REGISTER a.txt " + key(1)) == "OK", "the leader registers a name");
    check(g.ask(l, "REGISTER a.txt " + key(2)) == "EXISTS " + key(1), "and refuses a second key for it");
    check(g.ask(l, "RESOLVE a.txt") == "FILE " + key(1), "resolves it (ReadIndex)");
    check(g.ask(l, "LIST") == "NAMES 1 a.txt", "and lists it");
    check(g.ask(l, "BOGUS").compare(0, 4, "ERR ") == 0, "a malformed request is an ERR at any replica");
    check(g.ask(f, "VOTE 00000000 9 1 0 0") == "ERR group mismatch", "a call with another group's fingerprint is refused");
    check(g.ask(f, "APPEND x").compare(0, 4, "ERR ") == 0, "a malformed APPEND is refused");
    sleep_ms(3 * static_cast<int>(E));
    bool same = true;
    for (int i = 0; i < N; ++i) same = same && g.status(i)[6] == g.status(l)[6];
    check(same, "every replica has applied as far as the leader");
}

void test_concurrent_clients() {
    std::printf("-- eight clients at once\n");
    Group g;
    const int l = g.leader(3000);
    std::vector<std::thread> clients;
    std::vector<std::vector<std::string>> replies(8);
    for (int c = 0; c < 8; ++c)
        clients.emplace_back([&, c] {
            for (int i = 0; i < 40; ++i) {
                // Half the names are contested: two clients register them with different keys.
                const std::string name = "n" + std::to_string(i % 2 ? i : (c % 4) * 100 + i);
                replies[c].push_back(g.ask(l, "REGISTER " + name + " " + key(c)));
            }
        });
    for (std::thread &t : clients) t.join();
    int ok = 0, exists = 0, other = 0;
    for (const auto &v : replies)
        for (const std::string &r : v) {
            if (r == "OK") ++ok;
            else if (r.compare(0, 7, "EXISTS ") == 0) ++exists;
            else ++other;
        }
    check(other == 0, "every request answered OK or EXISTS (" + std::to_string(ok) + " OK, " + std::to_string(exists) +
                          " EXISTS, " + std::to_string(other) + " other)");
    // Each contested odd name: exactly one OK among the eight clients.
    bool one_winner = true;
    for (int i = 1; i < 40; i += 2) {
        int wins = 0;
        for (int c = 0; c < 8; ++c) wins += replies[c][i] == "OK";
        one_winner = one_winner && wins == 1;
    }
    check(one_winner, "every contested name has exactly one winner");
    sleep_ms(4 * static_cast<int>(E));
    const std::string list = g.ask(l, "LIST");
    bool same = true;
    for (int i = 0; i < N; ++i) same = same && g.status(i)[6] == g.status(l)[6];
    check(same && list.compare(0, 6, "NAMES ") == 0, "and every replica applied the same log");
}

void test_minority_leader() {
    std::printf("-- the leader cut off from both followers\n");
    Group g;
    const int l = g.leader(3000);
    check(g.ask(l, "REGISTER before " + key(1)) == "OK", "a write before the cut");
    g.isolate(l, true);
    const std::string r = g.ask(l, "REGISTER during " + key(2));
    check(r.compare(0, 11, "UNAVAILABLE") == 0, "a write to the cut-off leader is never acknowledged: " + r.substr(0, 60));
    const std::string rd = g.ask(l, "RESOLVE before");
    check(rd.compare(0, 11, "UNAVAILABLE") == 0 || rd.compare(0, 9, "REDIRECT ") == 0,
          "nor can it answer a read (no majority confirms it): " + rd.substr(0, 60));
    const int l2 = g.leader(3000, l);
    check(l2 >= 0 && l2 != l, "the other two elect a new leader");
    check(l2 >= 0 && g.ask(l2, "REGISTER after " + key(3)) == "OK", "and writes continue there");
    sleep_ms(3 * static_cast<int>(E));
    check(g.status(l)[2] != "leader", "the cut-off leader has stepped down (check-quorum)");
    const std::string term_before_heal = l2 >= 0 ? g.status(l2)[3] : "";
    g.isolate(l, false);
    sleep_ms(6 * static_cast<int>(E));
    // Healing may move the leadership: the cut-off replica kept standing for election,
    // raising its term each time, and its higher term deposes the leader when it
    // returns -- the cost of having no pre-vote (D-077). So ask whoever leads now.
    const int l3 = g.leader(3000);
    if (l3 >= 0 && (l3 != l2 || g.status(l3)[3] != term_before_heal))
        std::printf("    (the heal forced an election: term %s -> %s, as D-077 accepts without pre-vote)\n",
                    term_before_heal.c_str(), g.status(l3)[3].c_str());
    check(l3 >= 0 && g.ask(l3, "RESOLVE before") == "FILE " + key(1) && g.ask(l3, "RESOLVE after") == "FILE " + key(3),
          "after healing, both acknowledged writes are there");
    const std::string during = l3 >= 0 ? g.ask(l3, "RESOLVE during") : "";
    check(during == "MISSING", "and the unacknowledged one never took effect: " + during);
    bool same = true;
    for (int i = 0; i < N; ++i) same = same && l3 >= 0 && g.status(i)[6] == g.status(l3)[6];
    check(same, "the old leader caught up with the new log");
}

// A request whose entry is replaced must not be told what the replacement did. The
// leader is cut off holding a client's REGISTER; the others elect a leader, which
// commits its own entries at that index; the partition heals while the client is still
// waiting, and the old leader applies the replacement there. The client must hear
// UNAVAILABLE -- its registration never took effect -- not the result of applying
// someone else's entry. (Found by mutation testing: the minority test above lets the
// waiter time out before the heal, so nothing was applied while anyone waited.)
void test_replaced_entry() {
    std::printf("-- a waiting request whose entry a new leader replaces\n");
    Group g(3000);
    const int l = g.leader(3000);
    g.isolate(l, true);
    std::string reply;
    std::thread client([&] { reply = g.ask(l, "REGISTER replaced " + key(5)); });
    sleep_ms(50);                                             // the entry is in the old leader's log
    const int l2 = g.leader(3000, l);
    check(l2 >= 0 && g.ask(l2, "REGISTER other " + key(6)) == "OK", "the majority elects a leader and commits there");
    g.isolate(l, false);                                      // heal while the client still waits
    client.join();
    check(reply.compare(0, 11, "UNAVAILABLE") == 0, "the waiting client is told UNAVAILABLE: " + reply.substr(0, 70));
    const int l3 = g.leader(3000);
    check(l3 >= 0 && g.ask(l3, "RESOLVE replaced") == "MISSING", "and indeed its name was never registered");
}

void test_restart_and_failure() {
    std::printf("-- a replica restarted from its disk, and a disk that fails\n");
    Group g;
    int l = g.leader(3000);
    for (int i = 0; i < 20; ++i) g.ask(l, "REGISTER r" + std::to_string(i) + " " + key(i));
    const int f = (l + 1) % N;
    g.stop(f);
    for (int i = 20; i < 40; ++i) g.ask(l, "REGISTER r" + std::to_string(i) + " " + key(i));
    check(g.ask(l, "RESOLVE r39") == "FILE " + key(39), "writes go on with one replica down");
    g.boot(f);
    sleep_ms(8 * static_cast<int>(E));
    check(g.status(f)[6] == g.status(l)[6], "the restarted replica catches up from its disk and the leader");
    // Kill the leader; the other two carry on with everything acknowledged.
    g.stop(l);
    const int l2 = g.leader(3000, l);
    check(l2 >= 0 && g.ask(l2, "RESOLVE r39") == "FILE " + key(39), "with the leader gone, a new one has every acknowledged name");
    // A disk that fails: the replica must stop rather than answer.
    const int victim = l2 >= 0 ? l2 : f;
    g.disks[victim]->fail = true;
    g.ask(victim, "REGISTER doomed " + key(99));
    sleep_ms(50);
    check(g.fatal.load() > 0, "a replica whose disk fails calls its fatal handler");
}

}  // namespace

int main() {
    std::printf("test-replica: three tracker replicas in one process, real threads\n");
    test_election_and_requests();
    test_concurrent_clients();
    test_minority_leader();
    test_replaced_entry();
    test_restart_and_failure();
    std::printf("%s  %d/%d checks passed\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
