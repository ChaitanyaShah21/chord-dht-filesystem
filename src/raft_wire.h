#ifndef RAFT_WIRE_H
#define RAFT_WIRE_H

#include "raft.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Raft's calls as lines on the existing framing (decision D-071):
//
//   VOTE <group> <term> <candidate> <last-index> <last-term>   -> VOTED <term> <0|1>
//   APPEND <group> <term> <leader> <prev-index> <prev-term> <commit> <seq> <count> <length>
//          + <length> bytes: <count> lines of "<term> <command>"
//                                                              -> APPENDED <term> <0|1> <index>
//
// <group> is the group's fingerprint (D-079). APPEND's last word is its body's
// length, as for PUT, STORE and CHUNK (D-044); net.cpp's body_length knows it.
//
// Every parser here is strict and never throws on what a peer sends (I5): numbers
// are digits only with no sign and no leading zero, words are single-spaced, and a
// body must hold exactly <count> well-formed lines and nothing else.
// ---------------------------------------------------------------------------

// 8 hexadecimal characters of SHA-1 of the member list joined by commas, in order.
std::string group_fingerprint(const std::vector<std::string> &members);

std::string encode_vote(const std::string &group, const VoteRequest &req);
std::string encode_vote_reply(const VoteReply &reply);
void encode_append(const std::string &group, const AppendRequest &req, std::string &line, std::string &body);
std::string encode_append_reply(const AppendReply &reply);

// The request parsers take the line split into words, and leave the group's
// fingerprint in `group` for the caller to compare. `valid_command` decides which
// commands an APPEND may carry; a body with any other is refused whole.
bool parse_vote(const std::vector<std::string> &w, std::string &group, VoteRequest &req);
bool parse_append(const std::vector<std::string> &w, const std::string &body, bool (*valid_command)(const std::string &),
                  std::string &group, AppendRequest &req);
bool parse_vote_reply(const std::string &line, VoteReply &reply);
bool parse_append_reply(const std::string &line, AppendReply &reply);

// A canonical decimal number: "0", or digits with no leading zero, at most 18 of
// them (so it fits in 64 bits with room to spare).
bool parse_u64(const std::string &s, std::uint64_t &out);

#endif
