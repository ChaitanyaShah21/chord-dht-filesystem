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

bool send_all(int sock, const string &msg) {
    string out = msg;
    if(out.empty() || out.back() != '\n')
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
// Core command handler
//------------------------------------------------------------
string handle_command(const string &cmdline, const string &client_user="", bool record = true) {
    istringstream iss(cmdline);
    string cmd; iss >> cmd;
    if(cmd.empty()) return "Error- Empty Command";

    lock_guard<mutex> lock(state_mtx);

    //---------------- user commands ----------------
    if(cmd == "create_user") {
        string user, pass; iss >> user >> pass;
        if(user.empty() || pass.empty()) return "Invalid input\nUse: create_user <user> <pass>";
        if(users.count(user)) return "User already exists";
        users[user] = pass;
        if(record) append_update_to_file(cmdline);
        return "User created succesfully";
    }

    else if(cmd == "login") {
        string user, pass; iss >> user >> pass;
        if(user.empty() || pass.empty()) return "Invalid Input\nUse: login <user> <pass>";
        if(!users.count(user)) return "User doesn't exist";
        if(users[user] != pass) return "Invalid password";
        if(online_users.count(user)) return "Error - User already logged in";
        online_users.insert(user);
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
            return "Invalid input.\nUse: create_group <groupid> <owner>";
        if(client_user.empty() || owner != client_user)
            return "Error: you can only perform this command as yourself";
        if(!online_users.count(owner)) return "Owner must be logged in";
        if(groups.count(gid)) return "Group already exists";

        Group g; g.owner = owner; g.members.insert(owner);
        groups[gid] = g;
        if(record) append_update_to_file(cmdline);
        return "Group Created";
    }

    else if(cmd == "join_group") {
        string gid, user; iss >> gid >> user;
        if(gid.empty() || user.empty()) return "Invalid input.\nUse: join_group <groupid> <user>";
        if(client_user.empty() || user != client_user)
            return "Error: you can only perform this command as yourself";
        if(!online_users.count(user)) return "User must be logged in";
        if(!groups.count(gid)) return "Group not found";
        if(groups[gid].members.count(user)) return "Already a member";

        groups[gid].pending.insert(user);
        if(record) append_update_to_file(cmdline);
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
            return "Invalid input.\nUse: upload_file <groupid> <user> <filename> <size> <piecehashes...>";

        // user_info might look like "alice@127.0.0.1:6881"
        string user = user_info;
        size_t atpos = user.find('@');
        if (atpos != string::npos)
            user = user.substr(0, atpos); // extract just the username

        if (!online_users.count(user))
            return "User must be logged in";
        if (!groups.count(gid))
            return "Group not found";
        if (!groups[gid].members.count(user))
            return "User not in group";

        if(client_user.empty() || user != client_user)
            return "Error: you can only perform this command as yourself";
        if(!groups.count(gid)) return "Group not found";
        if(!groups[gid].members.count(user)) return "User not in group";

        size_t filesize = stoull(size_str);
        vector<string> hashes; string h;
        while(iss >> h) hashes.push_back(h);
        if(hashes.empty()) return "Error: no piece hashes given";

        FileInfo fi; fi.owner = user; fi.size = filesize; fi.piece_hashes = hashes;
        fi.seeders.insert(user);
        group_files[gid][filename] = fi;
        if(record) append_update_to_file(cmdline);

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
        if(!groups.count(gid)) return "Group not found";
        Group &g = groups[gid];
        if(g.owner != owner) return "Only the group owner can accept requests";
        if(!g.pending.count(user)) return "No such pending request";

        g.pending.erase(user);
        g.members.insert(user);
        if(record) append_update_to_file(cmdline);
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
        if(!groups.count(gid)) return "Group not found";

        Group &g = groups[gid];
        if(!g.members.count(user)) return "You are not a member of this group";

        // Owner leaving
        if(g.owner == user) {
            g.members.erase(user);
            if(g.members.empty()) {
                groups.erase(gid);
                if(record) append_update_to_file(cmdline);
                return "Group deleted as no members left";
            } else {
                g.owner = *g.members.begin();
                if(record) append_update_to_file(cmdline);
                return "Owner left. New owner: " + g.owner;
            }
        } else {
            g.members.erase(user);
            if(record) append_update_to_file(cmdline);
            return "Left group successfully";
        }
    }


    else return "Unknown command";
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
    listen_at_port = atoi(argv[1]);
    state_filename = "state_" + to_string(listen_at_port) + ".log";

    ifstream ifs(state_filename);
    string prev;
    while(getline(ifs, prev)) {
        if(prev.empty()) continue;
        update_log.push_back(prev);
        size_t sp = prev.find(' ');
        if(sp != string::npos) {
            size_t seq = stoull(prev.substr(0, sp));
            next_seq = max(next_seq.load(), seq+1);
        }
    }
    for(const auto &u : update_log) {
        size_t space = u.find(' ');
        string cmdline = (space != string::npos) ? u.substr(space+1) : u;
        handle_command(cmdline, "", false);
    }

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
