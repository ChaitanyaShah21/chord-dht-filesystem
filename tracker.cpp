#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <csignal>

using namespace std;

unordered_map<string,string> users; // username,password
unordered_set<string> online_users;
unordered_map<string, string> user_address_map;

atomic<size_t> next_seq{0}; // sequence number for updates

int listen_at_port = 0;
vector<string> update_log;

struct FileInfo {
    string owner;
    size_t size;
    vector<string> piece_hashes;
    unordered_set<string> seeders;
};

unordered_map<string, unordered_map<string, FileInfo>> group_files;

struct Group {
    string owner;
    unordered_set<string> members;
    unordered_set<string> pending;
};

unordered_map<string, Group> groups;

mutex state_mtx;
mutex peer_mtx;
mutex log_mtx;
vector<int> peer_sockets;
vector<size_t> peer_last_seq;
vector<pair<string,int>> peer_addrs;

int connect_to_peer(const string &ip,int port);
string state_filename;

// The control protocol's framing contract: one reply is exactly one line.
//
// The client reads a reply as bytes-up-to-newline, so a payload carrying a
// newline of its own arrives as *two* replies, and every later reply on that
// connection is one behind -- permanently, on a connection that is otherwise
// healthy. That is defect R6, and it is defect R3 reached from a different
// direction: R3 was a caller that never consumed its reply, R6 is a message
// that contains the delimiter. Same outcome, because nothing on the wire says
// how many lines a reply is.
//
// Fork F9, option B: the invariant is enforced *here*, in the single function
// that frames a reply, rather than trusted to every author of a message. A
// handler that returns an embedded newline is then a logged bug instead of a
// corrupted session, and the sixth such string written months from now cannot
// desync anything.
//
// Note what this function must never become: the client's own send_all carries
// raw piece bytes, which contain newlines constantly. Delimiter stripping is
// correct only on a text control channel and would corrupt every transfer if
// applied to the data path.
bool send_all(int sock, const string &msg) {
    string out = msg;

    // Drop any terminator the caller supplied; exactly one is appended below.
    // Without this, sanitising would turn a trailing '\n' into a trailing space
    // and change the bytes seen by well-formed callers.
    while(!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();

    size_t offenders = 0;
    for(char &c : out) {
        if(c == '\n' || c == '\r') { c = ' '; ++offenders; }
    }
    if(offenders > 0)
        cerr << "[tracker] BUG: reply carried " << offenders
             << " embedded newline(s); replaced with spaces to preserve framing: "
             << out << "\n";

    out.push_back('\n');
    size_t total = 0;
    while(total < out.size()) {
        ssize_t n = send(sock, out.c_str()+total, out.size()-total, 0);
        if(n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

void append_update_to_file(const string &update) {
    lock_guard<mutex> log_lock(log_mtx);
    size_t seq = next_seq++;
    ostringstream oss;
    oss << seq << " " << update;
    string line = oss.str();
    update_log.push_back(line);
    ofstream ofs(state_filename, ios::app);
    if(ofs) ofs << line << "\n";
}

int connect_to_peer(const string &ip,int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(s);
        return -1;
    }
    if(connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

void broadcast_sync(const string &cmdline, int exclude_sock = -1) {
    string msg;
    {
        lock_guard<mutex> log_lock(log_mtx);
        if(update_log.empty()) return;
        msg = "SYNC " + update_log.back();
    }
    lock_guard<mutex> peer_lock(peer_mtx);
    for(size_t i = 0; i < peer_sockets.size(); i++) {
        int sock = peer_sockets[i];
        if(sock < 0 || sock == exclude_sock) continue;
        if(!send_all(sock, msg)) {
            cerr << "[tracker] peer socket " << sock << " disconnected\n";
            close(sock);
            peer_sockets[i] = -1;
        } else {
            peer_last_seq[i] = update_log.size();
        }
    }
}

//------------------------------------------------------------
// Parsing helpers
//------------------------------------------------------------

// stoull throws on anything that is not a number, and on anything too large for
// size_t. Both are reachable from outside this process: a hostile `upload_file`
// from a client, and a state-log record whose tail was torn by a crash. An
// uncaught throw inside a detached client thread calls std::terminate and takes
// the entire tracker down (defect D4); the same throw during recovery stops the
// tracker from starting at all. Every number arriving from outside comes through
// here, and this function does not throw.
static bool parse_number(const string &text, size_t &out) {
    if(text.empty()) return false;
    for(char c : text)
        if(!isdigit(static_cast<unsigned char>(c))) return false;
    try { out = stoull(text); }
    catch(const std::exception &) { return false; }   // out_of_range on a 26-digit size
    return true;
}

//------------------------------------------------------------
// Effects -- the durable state transitions, and nothing else
//
// Decision D-009, fork F8. Each apply_* performs one change that is meant to
// survive a restart. None of them takes a client_user, consults online_users,
// writes soft state, or appends to the log.
//
// That is the whole point. Recovery calls these directly, so it cannot re-run an
// authorisation check -- not because it skips one, but because there is no code
// path from here to one. Defect R1 was recovery re-running admission checks
// instead of re-applying effects, and it failed silently for ten months.
//
// The checks that do remain here are the ones that ask about DURABLE state
// ("does this group exist?", "is this user a member?"). Those are deterministic:
// they were true when the command was first accepted, so replaying the log in
// the same order makes them true again. Checks about the REQUESTER ("are you who
// you say you are?") or about SOFT state ("are you logged in?") stay on the live
// path in handle_command, where there is a requester to ask about.
//
// Each returns an empty string on success, or a message saying why not.
//
// CONCURRENCY: the caller holds state_mtx. handle_command takes it at the top;
// replay_command takes it too, though recovery runs before any thread exists.
//------------------------------------------------------------

static string apply_create_user(const string &user, const string &pass) {
    if(users.count(user)) return "User already exists";
    users[user] = pass;
    return "";
}

static string apply_create_group(const string &gid, const string &owner) {
    if(groups.count(gid)) return "Group already exists";
    Group g; g.owner = owner; g.members.insert(owner);
    groups[gid] = g;
    return "";
}

static string apply_join_group(const string &gid, const string &user) {
    if(!groups.count(gid)) return "Group not found";
    if(groups[gid].members.count(user)) return "Already a member";
    groups[gid].pending.insert(user);
    return "";
}

static string apply_accept_request(const string &gid, const string &owner, const string &user) {
    if(!groups.count(gid)) return "Group not found";
    Group &g = groups[gid];
    if(g.owner != owner) return "Only the group owner can accept requests";
    if(!g.pending.count(user)) return "No such pending request";
    g.pending.erase(user);
    g.members.insert(user);
    return "";
}

// `note` receives the half of the reply that describes what the effect chose to
// do, because leaving a group has three different outcomes.
//
// DEFECT R5 lives on the `*g.members.begin()` line below. That is whichever
// element the unordered_set happens to yield first, which depends on hashing and
// on insertion history -- and on nothing that is written to the log. A replay can
// therefore hand the group to a different owner than the live run did. This is
// the concrete price of logging the REQUEST rather than the EFFECT (option C of
// fork F8, rejected in D-009). An effect log would have recorded the choice.
static string apply_leave_group(const string &gid, const string &user, string &note) {
    if(!groups.count(gid)) return "Group not found";
    Group &g = groups[gid];
    if(!g.members.count(user)) return "You are not a member of this group";

    if(g.owner != user) {
        g.members.erase(user);
        note = "Left group successfully";
        return "";
    }

    g.members.erase(user);
    if(g.members.empty()) {
        groups.erase(gid);
        note = "Group deleted as no members left";
        return "";
    }
    g.owner = *g.members.begin();                    // defect R5
    note = "Owner left. New owner: " + g.owner;
    return "";
}

// The manifest only -- owner, size, piece hashes. All durable.
//
// The old code also inserted the uploader into fi.seeders and wrote its address
// into user_address_map, in the same breath. Those are SOFT state: only true
// while that peer is alive. Replaying them at startup resurrects peers that are
// long gone -- exactly what the update_seeder handler already refuses to do
// (D-008). The live path adds the seeder after this returns; recovery does not,
// and waits for a heartbeat to learn who actually holds the file.
//
// Splitting the function is what made this visible. A `replaying` flag would
// have replayed the soft state and nobody would have looked.
static string apply_upload_file(const string &gid, const string &user,
                                const string &filename, size_t filesize,
                                const vector<string> &hashes) {
    if(!groups.count(gid)) return "Group not found";
    if(!groups[gid].members.count(user)) return "User not in group";
    if(hashes.empty()) return "Error: no piece hashes given";

    FileInfo fi;
    fi.owner = user;
    fi.size = filesize;
    fi.piece_hashes = hashes;
    group_files[gid][filename] = fi;
    return "";
}

//------------------------------------------------------------
// Core command handler -- the live path
//
// Reached only by a command that arrived on a socket, so there is always a
// requester to check. `client_user` has NO default and there is no `record`
// parameter any more: the call that caused defect R1,
//
//     handle_command(cmdline, "", false);
//
// no longer compiles. The compiler enforces the separation, not a code review.
//------------------------------------------------------------
string handle_command(const string &cmdline, const string &client_user) {
    istringstream iss(cmdline);
    string cmd; iss >> cmd;
    if(cmd.empty()) return "Error- Empty Command";

    lock_guard<mutex> lock(state_mtx);

    //---------------- user commands ----------------
    if(cmd == "create_user") {
        string user, pass; iss >> user >> pass;
        if(user.empty() || pass.empty()) return "Invalid input. Use: create_user <user> <pass>";
        string err = apply_create_user(user, pass);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);
        return "User created succesfully";
    }

    else if(cmd == "login") {
        string user, pass; iss >> user >> pass;
        if(user.empty() || pass.empty()) return "Invalid input. Use: login <user> <pass>";
        if(!users.count(user)) return "User doesn't exist";
        if(users[user] != pass) return "Invalid password";
        if(online_users.count(user)) return "Error - User already logged in";
        online_users.insert(user);
        // Not logged: who is connected is soft state, re-established by clients
        // reconnecting. There is no apply_login for the same reason.
        return "LOGIN_SUCCESS " + user;
    }

    else if(cmd == "logout") {
        string user; iss >> user;
        if(user.empty() || client_user.empty() || user != client_user)
            return "Error: You can only log out yourself";
        if(!online_users.count(user)) return "User not logged in";
        online_users.erase(user);
        return "Logged out succesfully";
    }

    //---------------- group commands ----------------
    else if(cmd == "create_group") {
        string gid, owner; iss >> gid >> owner;
        if(gid.empty() || owner.empty())
            return "Invalid input. Use: create_group <groupid> <owner>";
        // admission -- the requester, then soft state
        if(client_user.empty() || owner != client_user)
            return "Error: you can only perform this command as yourself";
        if(!online_users.count(owner)) return "Owner must be logged in";
        // effect
        string err = apply_create_group(gid, owner);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);
        return "Group Created";
    }

    else if(cmd == "join_group") {
        string gid, user; iss >> gid >> user;
        if(gid.empty() || user.empty()) return "Invalid input. Use: join_group <groupid> <user>";
        if(client_user.empty() || user != client_user)
            return "Error: you can only perform this command as yourself";
        if(!online_users.count(user)) return "User must be logged in";
        string err = apply_join_group(gid, user);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);
        return "Joining request sent(waiting for approval)";
    }

    else if(cmd == "list_groups") {
        if(groups.empty()) return "No groups";
        ostringstream oss; oss << "Groups:";
        for(auto &p: groups) oss << " " << p.first << "(leader:" << p.second.owner << ")";
        return oss.str();
    }

    else if(cmd == "upload_file")
    {
        string gid, user_info, filename, size_str;
        iss >> gid >> user_info >> filename >> size_str;

        if (gid.empty() || user_info.empty() || filename.empty() || size_str.empty())
            return "Invalid input. Use: upload_file <groupid> <user> <filename> <size> <piecehashes...>";

        // user_info might look like "alice@127.0.0.1:6881"
        string user = user_info;
        size_t atpos = user_info.find('@');
        if (atpos != string::npos)
            user = user_info.substr(0, atpos); // extract just the username

        // admission
        if(client_user.empty() || user != client_user)
            return "Error: you can only perform this command as yourself";
        if (!online_users.count(user))
            return "User must be logged in";

        // A non-numeric or oversized size used to throw out of stoull inside a
        // detached thread and take the whole tracker down (defect D4).
        size_t filesize = 0;
        if(!parse_number(size_str, filesize))
            return "Error: file size must be a number";

        vector<string> hashes; string h;
        while(iss >> h) hashes.push_back(h);

        // effect -- the manifest, which is durable
        string err = apply_upload_file(gid, user, filename, filesize, hashes);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);

        // soft state -- who holds a copy *right now*. Deliberately out here and
        // not inside apply_upload_file, so recovery never replays it (D-009).
        group_files[gid][filename].seeders.insert(user);

        // Record the uploader's address as "ip:port" only -- NOT "user@ip:port".
        //
        // This line used to read:
        //     user_address_map[user] = user_info.substr(user.find('@') + 1);
        // which searched `user` ("alice", no '@' in it) but sliced `user_info`
        // ("alice@127.0.0.1:6881"). find() returned npos == SIZE_MAX, npos + 1
        // wrapped around to 0, and substr(0) handed back the whole string. So the
        // map held "alice" -> "alice@127.0.0.1:6881", get_file_info emitted
        // "alice@alice@127.0.0.1:6881", the downloader split on the first '@' and
        // fed "alice@127.0.0.1" to inet_pton, which failed. Every download died
        // three components away from the typo. See docs/failures.md, defect R2.
        //
        // `atpos` is the offset computed above from user_info itself. Only record
        // an address when there actually is one; a bare username carries none.
        if (atpos != string::npos)
            user_address_map[user] = user_info.substr(atpos + 1);

        return "UPLOAD_SUCCESS " + filename;
    }

    else if(cmd == "update_seeder")
    {
        // update_seeder <groupid> <username> <ip:port> <filename>
        //
        // "I hold this file and can serve it." Sent by a peer after it finishes a
        // download, and re-sent periodically so the set stays fresh.
        //
        // The address is its OWN field rather than being packed into the username
        // as "user@ip:port". That packing is what caused R2: one token carrying two
        // values gets split in the wrong place eventually. See docs/failures.md.
        string gid, user, addr, filename;
        iss >> gid >> user >> addr >> filename;

        if(gid.empty() || user.empty() || addr.empty() || filename.empty())
            return "Usage: update_seeder <groupid> <user> <ip:port> <filename>";
        if(addr.find(':') == string::npos)
            return "Error: address must be ip:port";
        if(!groups.count(gid)) return "Group not found";
        if(!groups[gid].members.count(user)) return "User not in group";
        if(!group_files.count(gid) || !group_files[gid].count(filename))
            return "File not found in group";

        group_files[gid][filename].seeders.insert(user);
        user_address_map[user] = addr;

        // Deliberately NOT appended to the update log. Durable state is "this file
        // exists and here is its manifest"; who currently holds it is SOFT STATE --
        // liveness information that is only true while the peer is alive. Replaying
        // it at startup would resurrect peers that are long gone, and the heartbeat
        // re-establishes the real set within one period anyway.
        return "SEEDER_OK " + filename;
    }

    else if(cmd == "get_file_info")
    {
        string gid, filename, user;
        iss >> gid >> filename >> user;  // optional username

        if(gid.empty() || filename.empty())
            return "Usage: get_file_info <groupid> <filename>";

        if(!group_files.count(gid) || !group_files[gid].count(filename))
            return "File not found in group";

        const FileInfo &fi = group_files[gid][filename];
        ostringstream oss;
        oss << "FILE_INFO " << filename << " " << fi.size << " OWNER " << fi.owner;

        for(auto &h : fi.piece_hashes) oss << " " << h;
        oss << " SEEDERS";
        for (auto &s : fi.seeders) {
            auto it = user_address_map.find(s);
            if (it != user_address_map.end()) {
                oss << " " << s << "@" << it->second; // e.g. alice@127.0.0.1:6881
            } else {
                oss << " " << s;
            }
        }
        return oss.str();
    }

    else if(cmd == "list_requests")
    {
        string gid, owner; iss >> gid >> owner;
        if(gid.empty() || owner.empty())
            return "Usage: list_requests <groupid> <owner>";
        if(client_user.empty() || owner != client_user)
            return "Error: you can only perform this command as yourself";
        if(!groups.count(gid)) return "Group not found";
        Group &g = groups[gid];
        if(g.owner != owner) return "Only the group owner can view requests";
        if(g.pending.empty()) return "No pending requests";

        ostringstream oss;
        oss << "Pending:";
        for(auto &u : g.pending) oss << " " << u;
        return oss.str();
    }

    else if(cmd == "accept_request") {
        string gid, owner, user; iss >> gid >> owner >> user;
        if(gid.empty() || owner.empty() || user.empty())
            return "Usage: accept_request <groupid> <owner> <user>";
        if(client_user.empty() || owner != client_user)
            return "Error: you can only perform this command as yourself";
        string err = apply_accept_request(gid, owner, user);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);
        return "User added to group";
    }

    else if(cmd == "list_members") {
        string gid; iss >> gid;
        if(gid.empty()) return "Usage: list_members <groupid>";
        if(!groups.count(gid)) return "Group not found";
        const Group &g = groups[gid];
        if(g.members.empty()) return "No members";
        ostringstream oss;
        oss << "Members:";
        for(auto &m : g.members) oss << " " << m;
        return oss.str();
    }

    else if(cmd == "leave_group") {
        string gid, user; iss >> gid >> user;
        if(gid.empty() || user.empty())
            return "Usage: leave_group <groupid> <user>";
        if(client_user.empty() || user != client_user)
            return "Error: you can only perform this command as yourself";
        string note;
        string err = apply_leave_group(gid, user, note);
        if(!err.empty()) return err;
        append_update_to_file(cmdline);
        return note;
    }

    else return "Unknown command";
}

//------------------------------------------------------------
// Recovery -- the only other caller of apply_*
//
// The complete list of what replay is allowed to do. It calls apply_* directly
// and never calls handle_command, so there is no path from here to an
// authorisation guard, and no flag anyone has to remember.
//
// Anything not named here is skipped, and the skip is announced rather than
// silent. That covers read commands, login/logout, update_seeder, a record torn
// in half by a crash, and the coursework-era log format, which recorded every
// command including `login` and `list_groups`.
//
// It also cannot append to the log, because appending lives only in
// handle_command: replaying a log can never grow it.
//------------------------------------------------------------
static void replay_command(const string &cmdline) {
    lock_guard<mutex> lock(state_mtx);
    istringstream iss(cmdline);
    string cmd; iss >> cmd;
    string err;

    if(cmd == "create_user") {
        string user, pass; iss >> user >> pass;
        err = (user.empty() || pass.empty()) ? "malformed record"
                                             : apply_create_user(user, pass);
    }
    else if(cmd == "create_group") {
        string gid, owner; iss >> gid >> owner;
        err = (gid.empty() || owner.empty()) ? "malformed record"
                                             : apply_create_group(gid, owner);
    }
    else if(cmd == "join_group") {
        string gid, user; iss >> gid >> user;
        err = (gid.empty() || user.empty()) ? "malformed record"
                                            : apply_join_group(gid, user);
    }
    else if(cmd == "accept_request") {
        string gid, owner, user; iss >> gid >> owner >> user;
        err = (gid.empty() || owner.empty() || user.empty())
                  ? "malformed record" : apply_accept_request(gid, owner, user);
    }
    else if(cmd == "leave_group") {
        string gid, user, note; iss >> gid >> user;
        err = (gid.empty() || user.empty()) ? "malformed record"
                                            : apply_leave_group(gid, user, note);
    }
    else if(cmd == "upload_file") {
        string gid, user_info, filename, size_str;
        iss >> gid >> user_info >> filename >> size_str;

        string user = user_info;
        size_t atpos = user_info.find('@');
        if(atpos != string::npos) user = user_info.substr(0, atpos);

        vector<string> hashes; string h;
        while(iss >> h) hashes.push_back(h);

        size_t filesize = 0;
        if(gid.empty() || user.empty() || filename.empty() || !parse_number(size_str, filesize))
            err = "malformed record";
        else
            err = apply_upload_file(gid, user, filename, filesize, hashes);
        // No seeder and no address recorded here -- see apply_upload_file.
    }
    else {
        cerr << "[tracker] replay: skipping '"
             << (cmd.empty() ? string("<blank>") : cmd) << "' -- not a durable command\n";
        return;
    }

    if(!err.empty())
        cerr << "[tracker] replay: skipping " << cmd << " -- " << err << "\n";
}

//------------------------------------------------------------
// Client handler
//------------------------------------------------------------
void client_handler(int client_sock) {
    string logged_in_user;
    ostringstream oss;
    oss << "TRACKERS 127.0.0.1:" << listen_at_port;
    for(auto [ip,port] : peer_addrs) oss << " " << ip << ":" << port;
    send_all(client_sock, oss.str());

    char buffer[1024];
    string partial;
    while(true) {
        ssize_t n = recv(client_sock, buffer, sizeof(buffer)-1, 0);
        if(n <= 0) { cerr << "[tracker] client disconnect\n"; break; }
        buffer[n] = '\0';
        partial.append(buffer, n);
        size_t pos;
        while((pos = partial.find('\n')) != string::npos) {
            string line = partial.substr(0, pos);
            partial.erase(0, pos+1);
            if(line.empty()) continue;
            // --- Clean input (NEW) ---
            line.erase(remove(begin(line), end(line), '\r'), end(line));

            // remove leading '>' and spaces
            while(!line.empty() && (line.front() == '>' || std::isspace(static_cast<unsigned char>(line.front()))))
                line.erase(line.begin());

            // --- Clean up any CR/LF issues ---
            line.erase(remove(line.begin(), line.end(), '\r'), line.end());
            line.erase(remove(line.begin(), line.end(), '\n'), line.end());

            // remove leading '>' and spaces
            while (!line.empty() && (line.front() == '>' || std::isspace(static_cast<unsigned char>(line.front()))))
                line.erase(line.begin());

            // skip empty or whitespace-only lines
            string trimmed = line;
            trimmed.erase(remove_if(trimmed.begin(), trimmed.end(), ::isspace), trimmed.end());
            if (trimmed.empty()) continue;

            // skip our own echoed response lines (rare case when client flush overlaps)
            if (trimmed.rfind("LOGIN_SUCCESS", 0) == 0 ||
                trimmed.rfind("UPLOAD_SUCCESS", 0) == 0 ||
                trimmed.rfind("FILE_INFO", 0) == 0 ||
                trimmed.rfind("Groups:", 0) == 0)
                continue;


            // ignore random single-character junk like 's', '\r', etc.
            if (line.size() < 2)
            {
                continue;
            }
                
            // --- Skip sync lines ---
            if(line.rfind("SYNC", 0) == 0) continue;

            cerr << "[tracker] received: " << line << "\n";
            string resp = handle_command(line, logged_in_user);
            if(resp.rfind("LOGIN_SUCCESS ", 0) == 0) {
                logged_in_user = resp.substr(14);
                resp = "LOGIN successfull";
            }
            send_all(client_sock, resp);
            broadcast_sync(line);
        }
    }
    if(!logged_in_user.empty()) {
        lock_guard<mutex> stlock(state_mtx);
        online_users.erase(logged_in_user);
    }
    close(client_sock);
}

//------------------------------------------------------------
// Main
//------------------------------------------------------------
int main(int argc, char **argv) {
    if(argc < 2) {
        cerr << "Usage: ./tracker <port>\n";
        return 1;
    }
    // Defect R9: a client that hangs up mid-conversation used to kill this
    // process. Writing to a socket whose peer has gone raises SIGPIPE, and a
    // signal's default disposition is to terminate -- process-wide, so one
    // client's abrupt disconnect took down every other client's session too.
    // Ignoring it makes send() return -1 with errno EPIPE instead, which the
    // `if(n <= 0) return false` already in send_all has always handled
    // correctly; it simply never got the chance to run. Decision D-011.
    signal(SIGPIPE, SIG_IGN);

    listen_at_port = atoi(argv[1]);
    state_filename = "state_" + to_string(listen_at_port) + ".log";

    // ---------------- recovery ----------------
    // Read the log, then replay it. stoull is never called directly on a record:
    // a torn or hostile line used to throw here and stop the tracker booting.
    ifstream ifs(state_filename);
    string prev;
    while(getline(ifs, prev)) {
        if(prev.empty()) continue;
        update_log.push_back(prev);
        size_t sp = prev.find(' ');
        size_t seq = 0;
        if(sp != string::npos && parse_number(prev.substr(0, sp), seq))
            next_seq = max(next_seq.load(), seq + 1);
    }
    for(const auto &u : update_log) {
        size_t space = u.find(' ');
        size_t ignored = 0;
        // A record this tracker wrote is "<seq> <command>". The coursework-era
        // format had no sequence prefix, so strip a leading token only when it
        // really is a number -- otherwise the command itself would be eaten.
        string cmdline = (space != string::npos && parse_number(u.substr(0, space), ignored))
                       ? u.substr(space + 1) : u;
        replay_command(cmdline);
    }
    if(!update_log.empty())
        cerr << "[tracker] recovered " << update_log.size()
             << " records from " << state_filename << "\n";

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0) { perror("socket"); return 1; }

    // Without SO_REUSEADDR, a restart within the TIME_WAIT window fails with
    // EADDRINUSE: connections accepted by the previous tracker still occupy this
    // local port even after that process is gone. This blocked repeated runs
    // entirely, so it blocked benchmarking. See docs/failures.md, defect B5.
    // Note this is SO_REUSEADDR, not SO_REUSEPORT: it permits binding past those
    // lingering sockets, it does NOT permit a second live tracker on the same port.
    int opt = 1;
    if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        return 1;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(listen_at_port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if(bind(s, (sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if(listen(s, 10) < 0) { perror("listen"); return 1; }

    cerr << "[tracker] listening on port " << listen_at_port << "\n";
    while(true) {
        sockaddr_in cli{}; socklen_t len = sizeof(cli);
        int cs = accept(s, (sockaddr*)&cli, &len);
        if(cs < 0) { perror("accept"); continue; }
        cerr << "[tracker] client connected\n";
        thread(client_handler, cs).detach();
    }
    close(s);
    return 0;
}
