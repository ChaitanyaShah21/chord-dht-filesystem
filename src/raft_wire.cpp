#include "raft_wire.h"

#include "lookup.h"     // split_single_spaces
#include "sha1.h"

std::string group_fingerprint(const std::vector<std::string> &members) {
    std::string joined;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i) joined += ",";
        joined += members[i];
    }
    return sha1(joined).substr(0, 8);
}

bool parse_u64(const std::string &s, std::uint64_t &out) {
    if (s.empty() || s.size() > 18) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

namespace {

// A replica id on the wire: a small canonical number.
bool parse_id(const std::string &s, int &out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v) || v > 1000) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_flag(const std::string &s, bool &out) {
    if (s != "0" && s != "1") return false;
    out = s == "1";
    return true;
}

std::string num(std::uint64_t v) { return std::to_string(v); }

}  // namespace

std::string encode_vote(const std::string &group, const VoteRequest &req) {
    return "VOTE " + group + " " + num(req.term) + " " + std::to_string(req.candidate) + " " + num(req.last_index) + " " +
           num(req.last_term);
}

std::string encode_vote_reply(const VoteReply &reply) {
    return "VOTED " + num(reply.term) + (reply.granted ? " 1" : " 0");
}

void encode_append(const std::string &group, const AppendRequest &req, std::string &line, std::string &body) {
    body.clear();
    for (const Entry &e : req.entries) body += num(e.term) + " " + e.command + "\n";
    line = "APPEND " + group + " " + num(req.term) + " " + std::to_string(req.leader) + " " + num(req.prev_index) + " " +
           num(req.prev_term) + " " + num(req.commit) + " " + num(req.seq) + " " + num(req.entries.size()) + " " +
           num(body.size());
}

std::string encode_append_reply(const AppendReply &reply) {
    return "APPENDED " + num(reply.term) + (reply.success ? " 1 " : " 0 ") + num(reply.index);
}

bool parse_vote(const std::vector<std::string> &w, std::string &group, VoteRequest &req) {
    if (w.size() != 6 || w[0] != "VOTE") return false;
    group = w[1];
    return parse_u64(w[2], req.term) && parse_id(w[3], req.candidate) && parse_u64(w[4], req.last_index) &&
           parse_u64(w[5], req.last_term);
}

bool parse_append(const std::vector<std::string> &w, const std::string &body, bool (*valid_command)(const std::string &),
                  std::string &group, AppendRequest &req) {
    std::uint64_t count = 0, length = 0;
    if (w.size() != 10 || w[0] != "APPEND") return false;
    group = w[1];
    if (!parse_u64(w[2], req.term) || !parse_id(w[3], req.leader) || !parse_u64(w[4], req.prev_index) ||
        !parse_u64(w[5], req.prev_term) || !parse_u64(w[6], req.commit) || !parse_u64(w[7], req.seq) ||
        !parse_u64(w[8], count) || !parse_u64(w[9], length) || length != body.size())
        return false;

    // The body: exactly `count` lines, each "<term> <command>", each newline-terminated.
    req.entries.clear();
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) return false;                   // a last line with no newline
        const std::string line = body.substr(pos, nl - pos);
        pos = nl + 1;
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) return false;
        Entry e;
        if (!parse_u64(line.substr(0, space), e.term)) return false;
        e.command = line.substr(space + 1);
        if (!valid_command(e.command)) return false;
        // Terms in a log never fall, and none is newer than the leader's own: a body
        // that breaks either did not come from a correct leader.
        const Term floor = req.entries.empty() ? req.prev_term : req.entries.back().term;
        if (e.term < floor || e.term > req.term) return false;
        req.entries.push_back(std::move(e));
        if (req.entries.size() > count) return false;
    }
    return req.entries.size() == count;
}

bool parse_vote_reply(const std::string &line, VoteReply &reply) {
    const std::vector<std::string> w = split_single_spaces(line);
    return w.size() == 3 && w[0] == "VOTED" && parse_u64(w[1], reply.term) && parse_flag(w[2], reply.granted);
}

bool parse_append_reply(const std::string &line, AppendReply &reply) {
    const std::vector<std::string> w = split_single_spaces(line);
    return w.size() == 4 && w[0] == "APPENDED" && parse_u64(w[1], reply.term) && parse_flag(w[2], reply.success) &&
           parse_u64(w[3], reply.index);
}
