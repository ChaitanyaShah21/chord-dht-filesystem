#include "group.h"

#include "chord.h"      // parse_members: the one strict address parser

#include <sstream>

bool parse_address_list(const std::string &text, std::vector<Address> &out, std::string &err) {
    out.clear();
    if (text.empty() || text.find('\n') != std::string::npos) {
        err = "an address list is ip:port[,ip:port...]";
        return false;
    }
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        std::vector<Peer> one;
        std::string why;
        if (item.empty() || !parse_members(item, one, why) || one.size() != 1) {
            err = "not an address: '" + item + "'";
            return false;
        }
        Address a{one[0].ip, one[0].port};
        for (const Address &b : out)
            if (b.ip == a.ip && b.port == a.port) {
                err = a.text() + " is listed twice";
                return false;
            }
        out.push_back(a);
    }
    if (out.empty() || text.back() == ',') {
        err = "an address list is ip:port[,ip:port...]";
        return false;
    }
    return true;
}

namespace {

bool starts_with(const std::string &s, const char *prefix) { return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; }

}  // namespace

bool ask_group(GroupClient &g, const std::string &request, std::string &reply) {
    const std::size_t n = g.replicas.size();
    const Millis deadline = g.now() + g.wait_ms;
    std::vector<std::string> said(n);                 // what each listed replica said last round
    while (true) {
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t first = (g.preferred + k) % n;
            Address target = g.replicas[first];
            std::string note;
            // Follow redirects from this replica, a bounded number of times.
            for (std::size_t hops = 0; hops <= n; ++hops) {
                std::string line;
                const Delivery d = g.exchange(target, request, line);
                if (d != Delivery::Ok) {
                    note += target.text() + " " + delivery_name(d);
                    break;
                }
                if (starts_with(line, "REDIRECT ")) {
                    std::vector<Address> to;
                    std::string why;
                    note += target.text() + " redirected to " + line.substr(9) + "; ";
                    if (!parse_address_list(line.substr(9), to, why) || to.size() != 1) break;
                    target = to[0];
                    continue;
                }
                if (starts_with(line, "UNAVAILABLE")) {
                    note += target.text() + " answered " + line;
                    break;
                }
                // An answer. Start the next request where this one succeeded.
                for (std::size_t i = 0; i < n; ++i)
                    if (g.replicas[i].ip == target.ip && g.replicas[i].port == target.port) g.preferred = i;
                reply = line;
                return true;
            }
            said[first] = note;
            if (g.now() >= deadline) break;
        }
        const Millis left = deadline - g.now();
        if (left <= 0) break;
        g.sleep(static_cast<int>(left < g.pause_ms ? left : g.pause_ms));
    }
    reply.clear();
    for (const std::string &s : said)
        if (!s.empty()) reply += (reply.empty() ? "" : "; ") + s;
    return false;
}
