// client.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <cstring>
#include "sha1.h"
#include <atomic>

using namespace std;

// Make PIECE_SIZE a const (avoid macro precedence issues)
constexpr size_t PIECE_SIZE = 512 * 1024; // 512KB

int peer_listen_port = 6881; // default port, can be overwritten

struct SeedFile {
    string groupid;
    string filename;
};

vector<SeedFile> seeding_files;
mutex seeding_mtx;

struct DownloadPiece {
    size_t index;             // piece index
    size_t size;              // actual size
    std::string hash;         // expected SHA1 hash
    bool done = false;        // completed flag
};

struct DownloadTask {
    string groupid;
    string filename;
    string dest_path;
    size_t filesize;
    vector<string> piece_hashes;
    vector<bool> received;
    vector<int> piece_sizes;
    vector<pair<string,int>> peers;
    atomic<size_t> pieces_done{0};
    bool completed = false;

    DownloadTask() = default;

    // custom move constructor
    DownloadTask(DownloadTask&& other) noexcept {
        groupid       = std::move(other.groupid);
        filename      = std::move(other.filename);
        dest_path     = std::move(other.dest_path);
        filesize      = other.filesize;
        piece_hashes  = std::move(other.piece_hashes);
        received      = std::move(other.received);
        piece_sizes   = std::move(other.piece_sizes);
        peers         = std::move(other.peers);
        pieces_done.store(other.pieces_done.load());
        completed     = other.completed;
    }

    // custom move assignment
    DownloadTask& operator=(DownloadTask&& other) noexcept {
        if (this != &other) {
            groupid       = std::move(other.groupid);
            filename      = std::move(other.filename);
            dest_path     = std::move(other.dest_path);
            filesize      = other.filesize;
            piece_hashes  = std::move(other.piece_hashes);
            received      = std::move(other.received);
            piece_sizes   = std::move(other.piece_sizes);
            peers         = std::move(other.peers);
            pieces_done.store(other.pieces_done.load());
            completed     = other.completed;
        }
        return *this;
    }

    // forbid copy
    DownloadTask(const DownloadTask&) = delete;
    DownloadTask& operator=(const DownloadTask&) = delete;
};



vector<DownloadTask> active_downloads;
mutex download_mtx;

vector<pair<string,int>> tracker_addrs;
int current_sock = -1;
mutex sock_mtx;
string logged_in_user;
const int CONNECT_RETRIES = 3;

// Write a single piece to file using system calls
bool write_piece(const std::string &filepath, size_t offset, const std::vector<unsigned char> &data) {
    int fd = open(filepath.c_str(), O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return false;

    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    size_t total = 0;
    while (total < data.size()) {
        ssize_t written = ::write(fd, data.data() + total, data.size() - total);
        if (written <= 0) {
            close(fd);
            return false;
        }
        total += (size_t)written;
    }

    close(fd);
    return true;
}

// Read a piece from file using system calls (optional)
bool read_piece(const std::string &filepath, size_t offset, size_t size, std::vector<unsigned char> &out) {
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) return false;

    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    out.resize(size);
    size_t total = 0;
    while (total < size) {
        ssize_t r = ::read(fd, out.data() + total, size - total);
        if (r <= 0) {
            close(fd);
            return false;
        }
        total += (size_t)r;
    }

    close(fd);
    return true;
}

// sending bytes to tracker/peer reliably
bool send_all(int sock, const char *buff, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sock, buff + total, len - total, 0);
        if (s <= 0) {
            return false;
        }
        total += (size_t)s;
    }
    return true;
}

bool send_line(int sock, const string &line) {
    string out = line;
    // Only add newline if not already ending with one
    if (!out.empty() && (out.back() == '\n' || out.back() == '\r')) 
    {
        // already fine
    } 
    else 
    {
        out.push_back('\n');
    }
    return send_all(sock, out.c_str(), out.size());
}

// recv a single newline-terminated line. 'buffer' holds leftover data between calls.
bool recv_line(int sock, string &out) {
    out.clear();
    char ch;
    while (true) {
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') break;
        out.push_back(ch);
    }
    return true;
}

int connect_to_server(const string &ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(s);
        return -1;
    }

    if (connect(s, (sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }

    return s;
}

// get local IP used for an existing connected socket (string form)
static string get_local_ip_from_socket(int sock) {
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    if (getsockname(sock, (sockaddr *)&addr, &addrlen) < 0) {
        return string("127.0.0.1");
    }
    char buff[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, buff, sizeof(buff)) == nullptr) {
        return string("127.0.0.1");
    }
    return string(buff);
}

static string prepare_command_for_tracker(const string &raw, const string &user) {
    if (user.empty()) return raw;
    istringstream iss(raw);
    string cmd; iss >> cmd;
    if (cmd.empty()) return raw;

    const static vector<string> skip = {
        "create_user", "login", "list_groups", "list_members", "list_files", "quit", "exit"
    };
    if (find(skip.begin(), skip.end(), cmd) != skip.end()) return raw;

    if (cmd == "create_group" || cmd == "join_group" || cmd == "list_requests" ||
        cmd == "leave_group" || cmd == "list_files" || cmd == "list_members") {

        string rest;
        getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        if (rest.empty()) return raw;
        return cmd + " " + rest + " " + user;
    }

    if (cmd == "accept_request") {
        string gid, u2;
        iss >> gid >> u2;
        if (gid.empty() || u2.empty()) return raw;
        return cmd + " " + gid + " " + user + " " + u2;
    }

    if (cmd == "stop_share") {
        string gid, fname;
        iss >> gid >> fname;
        if (gid.empty() || fname.empty()) return raw;
        return cmd + " " + gid + " " + user + " " + fname;
    }

    {
        string rest;
        getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        return cmd + (rest.empty() ? string("") : " " + rest) + " " + user;
    }
}

bool connect_any_tracker() {
    for (auto [ip, port] : tracker_addrs) {
        int s = connect_to_server(ip, port);
        if (s >= 0) {
            cerr << "[client] connect to " << ip << ":" << port << "\n";
            current_sock = s;
            return true;
        }
    }
    return false;
}

void handle_trackers_line(const string &line) {
    tracker_addrs.clear();
    istringstream iss(line.substr(9)); // skip "TRACKERS "
    string entry;
    while (iss >> entry) {
        size_t colon = entry.find(':');
        if (colon != string::npos) {
            string t_ip = entry.substr(0, colon);
            int t_port = stoi(entry.substr(colon + 1));
            tracker_addrs.push_back({t_ip, t_port});
        }
    }
    cerr << "[client] updated tracker list (" << tracker_addrs.size() << " entries)\n";
}

// protocol - client to peer: GET_PIECE
// response - PIECE <len>\n followed by raw bytes
void handle_peer_connection(int peer_sock) {
    const size_t BUF_SZ = 4096;
    string partial;
    char tmp[BUF_SZ];

    while (true) {
        ssize_t n = recv(peer_sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        partial.append(tmp, tmp + n);

        size_t pos;
        while ((pos = partial.find('\n')) != string::npos) {
            string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);

            // trim
            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if (start == string::npos) continue;
            line = line.substr(start, end - start + 1);

            istringstream iss(line);
            string cmd;
            iss >> cmd;
            if (cmd == "GET_PIECE") {
                string gid, filename;
                int piece_idx;
                iss >> gid >> filename >> piece_idx;
                if (gid.empty() || filename.empty() || piece_idx < 0) {
                    send(peer_sock, "ERROR\n", 6, 0);
                    continue;
                }

                int fd = open(filename.c_str(), O_RDONLY);
                if (fd < 0) {
                    send(peer_sock, "ERROR\n", 6, 0);
                    continue;
                }

                off_t piece_offset = (off_t)piece_idx * (off_t)PIECE_SIZE;
                if (lseek(fd, piece_offset, SEEK_SET) == (off_t)-1) {
                    close(fd);
                    send(peer_sock, "ERROR\n", 6, 0);
                    continue;
                }

                struct stat st;
                if (fstat(fd, &st) < 0) {
                    close(fd);
                    send(peer_sock, "ERROR\n", 6, 0);
                    continue;
                }

                size_t bytes_rem = 0;
                if ((size_t)piece_offset >= (size_t)st.st_size) {
                    bytes_rem = 0;
                } else {
                    bytes_rem = min((size_t)PIECE_SIZE, (size_t)(st.st_size - piece_offset));
                }

                ostringstream hdr;
                hdr << "PIECE " << bytes_rem << "\n";
                string hdrs = hdr.str();
                if (send(peer_sock, hdrs.c_str(), hdrs.size(), 0) < 0) {
                    close(fd);
                    continue;
                }

                size_t sent = 0;
                char buff[8192];
                while (sent < bytes_rem) {
                    ssize_t r = read(fd, buff, (size_t)min(sizeof(buff), bytes_rem - sent));
                    if (r <= 0) break;
                    ssize_t w = send(peer_sock, buff, r, 0);
                    if (w <= 0) break;
                    sent += (size_t)w;
                }
                close(fd);
            } else {
                send(peer_sock, "UNKNOWN\n", 8, 0);
            }
        }
    }
    close(peer_sock);
}

void peer_server_thread_func(int port) {
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) return;
    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    // These two failures used to return silently. That is the worst possible
    // behaviour here: with no peer server the client still logs in, still uploads
    // its manifest, and still advertises itself as a seeder -- so every peer that
    // tries to fetch from it fails, and nothing anywhere says why. Announce it.
    if (bind(lsock, (sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("[client] peer-server bind");
        cerr << "[client] FATAL: cannot serve pieces on port " << port
             << "; this client will advertise files it cannot actually serve\n";
        close(lsock);
        return;
    }
    if (listen(lsock, 10) < 0) {
        perror("[client] peer-server listen");
        close(lsock);
        return;
    }

    cerr << "[client] peer-server listening on port " << port << "\n";
    while (true) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int csock = accept(lsock, (sockaddr*)&cli, &len);
        if (csock < 0) continue;
        thread t(handle_peer_connection, csock);
        t.detach();
    }
    close(lsock);
}

bool upload_file_to_tracker(const string &filepath, const string &groupid, const string &username, int sock) {
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Error: Cannot open file " << filepath << "\n";
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        cerr << "Error: Cannot stat file " << filepath << "\n";
        close(fd);
        return false;
    }

    size_t filesize = (size_t)st.st_size;
    vector<string> piece_hashes;
    vector<unsigned char> buffer;
    buffer.resize(PIECE_SIZE);
    ssize_t n;
    while ((n = read(fd, buffer.data(), PIECE_SIZE)) > 0) {
        piece_hashes.push_back(sha1_bytes(buffer.data(), (size_t)n));
    }

    if (n < 0) {
        cerr << "Error: Read failed for " << filepath << "\n";
        close(fd);
        return false;
    }
    close(fd);

    string local_ip = get_local_ip_from_socket(sock);
    ostringstream useraddr;
    useraddr << username << "@" << local_ip << ":" << peer_listen_port;
    string user_with_addr = useraddr.str();

    ostringstream cmd;
    cmd << "upload_file " << groupid << " " << user_with_addr << " " << filepath << " " << filesize;
    for (const auto &h : piece_hashes) {
        cmd << " " << h;
    }

    if (!send_line(sock, cmd.str())) {
        cerr << "Failed to send upload command\n";
        return false;
    }
    string recv_buf, resp;
    if (recv_line(sock, resp)) {
        cout << resp << "\n";
    } else {
        cerr << "No response from tracker after upload\n";
    }
    return true;
}

bool download_piece(int sock, const string &groupid, const string &filename, size_t piece_index, char *buffer, size_t &out_len) {
    // This function is unused in current flow but kept for completeness
    string req = "get_piece " + groupid + " " + filename + " " + to_string(piece_index);
    if (!send_line(sock, req)) {
        return false;
    }

    string line;
    string recv_buf;
    if (!recv_line(sock, line)) return false;

    out_len = stoul(line); // piece size
    size_t total = 0;
    while (total < out_len) {
        ssize_t n = recv(sock, buffer + total, out_len - total, 0);
        if (n <= 0) return false;
        total += (size_t)n;
    }
    return true;
}

bool download_piece_from_peer(const std::string &peer_ip, int peer_port,
                              const std::string &groupid, const std::string &filename,
                              size_t piece_index, const std::string &expected_hash,
                              const std::string &dest_filepath) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer_port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    ostringstream oss;
    oss << "GET_PIECE " << groupid << " " << filename << " " << piece_index << "\n";
    string req = oss.str();
    if (!send_all(sock, req.c_str(), req.size())) { close(sock); return false; }

    string recv_buf, header;
    if (!recv_line(sock, header)) { close(sock); return false; }

    istringstream hs(header);
    string tag;
    size_t piece_size = 0;
    hs >> tag >> piece_size;
    if (tag != "PIECE") { close(sock); return false; }

    vector<unsigned char> buffer(piece_size);
    size_t total = 0;
    while (total < piece_size) {
        ssize_t r = recv(sock, (char*)buffer.data() + total, piece_size - total, 0);
        if (r <= 0) { close(sock); return false; }
        total += (size_t)r;
    }
    buffer.resize(total);
    close(sock);

    string hash = sha1_bytes(buffer.data(), buffer.size());
    if (hash != expected_hash) {
        return false;
    }

    if (!write_piece(dest_filepath, piece_index * PIECE_SIZE, buffer)) {
        return false;
    }

    return true;
}

void download_file_multipeer(DownloadTask &task) {
    size_t total_pieces = task.piece_hashes.size();
    atomic<size_t> next_piece(0);

    auto worker = [&](void) {
        while (true) {
            size_t idx = next_piece.fetch_add(1);
            if (idx >= total_pieces) break;

            bool success = false;
            // try all peers for this piece
            for (auto &peer : task.peers) {
                if (download_piece_from_peer(peer.first, peer.second,
                                             task.groupid, task.filename,
                                             idx, task.piece_hashes[idx], task.dest_path)) {
                    success = true;
                    break;
                }
            }
            if (success) {
                task.pieces_done.fetch_add(1);
            } else {
                // log failure for this piece; do not decrement next_piece (atomic).
                cerr << "[client] failed to download piece " << idx << " from all peers\n";
            }
        }
    };

    size_t max_threads = min((size_t)task.peers.size(), (size_t)4);
    if (max_threads == 0) max_threads = 1;
    vector<thread> threads;
    for (size_t i = 0; i < max_threads; ++i) threads.emplace_back(worker);
    for (auto &t : threads) t.join();

    // mark complete if all pieces fetched
    if (task.pieces_done.load() == total_pieces) {
        task.completed = true;
    } else {
        task.completed = false;
    }
}

void seeder_heartbeat_thread(int sock, const string &username) {
    while (true) {
        {
            lock_guard<mutex> lg(seeding_mtx);
            for (const auto &sf : seeding_files) {
                string cmd = "update_seeder " + sf.groupid + " " + username + " " + sf.filename;
                lock_guard<mutex> s_lock(sock_mtx);
                if (current_sock >= 0) {
                    send_line(current_sock, cmd);
                }
            }
        }
        this_thread::sleep_for(chrono::seconds(30));
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        cerr << "Invalid input.\nUse: ./client <tracker ip> <tracker port> [peer_listen_port]\n";
        return -1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);

    int sock = -1;
    for (int i = 0; i < CONNECT_RETRIES; ++i) {
        sock = connect_to_server(ip, port);
        if (sock >= 0) break;
        cerr << "[client] connect failed, retrying [" << i + 1 << "/" << CONNECT_RETRIES << "]\n";
        this_thread::sleep_for(chrono::seconds(1));
    }

    if (sock < 0) {
        cerr << "[client] could not connect to tracker\n";
        return 1;
    }
    cerr << "[client] connected to " << ip << ":" << port << "\n";

    current_sock = sock;

    if (argc >= 4) {
        peer_listen_port = atoi(argv[3]);
        if (peer_listen_port <= 0) peer_listen_port = 6881;
    }
    thread(peer_server_thread_func, peer_listen_port).detach();

    string init_buff;
    string init_resp;
    if (recv_line(sock, init_resp)) {
        if (init_resp.rfind("TRACKERS", 0) == 0) {
            handle_trackers_line(init_resp);
        }
    }

    string recv_buff;
    string line;
    while (true) {
        cout << "> ";
        cout.flush();

        if (!getline(cin, line)) break;

        // Trim any trailing carriage returns or spaces
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        if (line == "quit" || line == "exit") {
            cerr << "[client] exiting\n";
            break;
        }

        if (!logged_in_user.empty()) {
            line = prepare_command_for_tracker(line, logged_in_user);
        }

        // upload_file (client-side handled metadata & seeding)
        if (line.rfind("upload_file", 0) == 0) {
            istringstream iss(line);
            string cmd, groupid, filepath;
            iss >> cmd >> groupid >> filepath;
            if (groupid.empty() || filepath.empty()) {
                cerr << "Usage: upload_file <group> <filepath>\n";
                continue;
            }
            if (logged_in_user.empty()) {
                cerr << "You must be logged in to upload\n";
                continue;
            }
            if (!upload_file_to_tracker(filepath, groupid, logged_in_user, sock)) {
                cerr << "Upload failed\n";
            } else {
                lock_guard<mutex> lg(seeding_mtx);
                seeding_files.push_back({groupid, filepath});
            }
            continue;
        }

        // download_file (request file info, then multi-peer download)
        if (line.rfind("download_file", 0) == 0) {
            istringstream iss(line);
            string cmd, groupid, filename, destpath;
            iss >> cmd >> groupid >> filename >> destpath;

            if (groupid.empty() || filename.empty() || destpath.empty()) {
                cerr << "Usage: download_file <groupid> <filename> <destination>\n";
                continue;
            }
            if (logged_in_user.empty()) {
                cerr << "You must be logged in to download\n";
                continue;
            }

            string getinfo_cmd = "get_file_info " + groupid + " " + filename;
            if (!send_line(sock, getinfo_cmd)) {
                cerr << "Failed to request file info\n";
                continue;
            }

            string tracker_resp;
            if (!recv_line(sock, tracker_resp)) {
                cerr << "Tracker did not respond for file info\n";
                continue;
            }

            if (tracker_resp.rfind("FILE_INFO", 0) != 0) {
                cerr << "Tracker response: " << tracker_resp << "\n";
                continue;
            }

            istringstream pres(tracker_resp);
            string token;
            pres >> token; // FILE_INFO
            string resp_filename;
            pres >> resp_filename;

            size_t filesize = 0;
            pres >> filesize;

            pres >> token; // should be "OWNER"
            string owner_str;
            if (token == "OWNER") {
                pres >> owner_str;
                pres >> token; // next token after owner
            }

            vector<string> piece_hashes;
            while (token != "SEEDERS" && pres) {
                piece_hashes.push_back(token);
                if (!(pres >> token)) break;
            }

            vector<string> seeder_entries;
            string s;
            while (pres >> s) {
                seeder_entries.push_back(s);
            }

            if (piece_hashes.empty()) {
                cerr << "No piece hashes available from tracker\n";
                continue;
            }
            if (seeder_entries.empty()) {
                cerr << "No seeders availble for this file\n";
                continue;
            }

            vector<pair<string,int>> seeder_addr;
            for (auto &se : seeder_entries) {
                string ipport = se;
                size_t atpos = se.find('@');
                if (atpos != string::npos) ipport = se.substr(atpos + 1);
                size_t colon = ipport.find(':');
                if (colon == string::npos) continue;
                string sip = ipport.substr(0, colon);
                int sport = atoi(ipport.substr(colon + 1).c_str());
                seeder_addr.push_back({sip, sport});
            }

            if (seeder_addr.empty()) {
                cerr << "No valid seeder address parsed\n";
                continue;
            }

            int outfd = open(destpath.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666);
            if (outfd < 0) {
                cerr << "Cannot open destination file: " << destpath << "\n";
                continue;
            }
            if (ftruncate(outfd, (off_t)filesize) < 0) {
                cerr << "Failed to set file size\n";
                close(outfd);
                continue;
            }

            DownloadTask task;
            task.groupid = groupid;
            task.filename = filename;
            task.dest_path = destpath;
            task.filesize = filesize;
            task.piece_hashes = piece_hashes;
            task.received.assign(piece_hashes.size(), false);
            task.pieces_done = 0;
            task.peers = seeder_addr;

            {
                lock_guard<mutex> lg(download_mtx);
                active_downloads.emplace_back(move(task));

            }
            DownloadTask &task_ref = active_downloads.back();

            // Start multi-peer download (blocking here)
            download_file_multipeer(task_ref);

            if (task_ref.completed) {
                cerr << "Download completed: " << destpath << "\n";
                lock_guard<mutex> lg(seeding_mtx);
                seeding_files.push_back({groupid, filename});
                string update_cmd = "update_seeder " + groupid + " " + logged_in_user + " " + filename;
                send_line(sock, update_cmd);
            } else {
                cerr << "Download incomplete: " << task_ref.pieces_done.load() << " / " << task_ref.piece_hashes.size() << " pieces\n";
            }

            close(outfd);
            continue;
        }

        if (line == "show_downloads") {
            lock_guard<mutex> lg(download_mtx);
            for (auto &d : active_downloads) {
                size_t total_pieces = d.piece_hashes.size();
                size_t done = d.pieces_done.load();
                string status = (done == total_pieces) ? "[C]" : "[D]";
                cout << status << " [" << d.groupid << "] " << d.filename
                     << " " << done << "/" << total_pieces << " pieces downloaded\n";
            }
            continue;
        }

        // send other commands to tracker
        if (!send_line(sock, line)) {
            cerr << "[client] send failed ... connection lost\n";
            close(sock);

            int new_sock = -1;
            for (int i = 0; i < CONNECT_RETRIES; ++i) {
                new_sock = connect_to_server(ip, port);
                if (new_sock >= 0) break;
                this_thread::sleep_for(chrono::seconds(1));
            }

            if (new_sock < 0) {
                cerr << "[client] reconnect failed, trying other trackers\n";
                if (!connect_any_tracker()) {
                    cerr << "[client] all trackers unreachable, exiting\n";
                    return 1;
                }
                sock = current_sock;
                recv_buff.clear();
                string tmp_buff, tmp_resp;
                if (recv_line(sock, tmp_resp)) {
                    if (tmp_resp.rfind("TRACKERS", 0) == 0) handle_trackers_line(tmp_resp);
                }
                cerr << "[client] switched to another tracker\n";
            } else {
                sock = new_sock;
                recv_buff.clear();
                cerr << "[client] reconnected\n";
            }

            if (!send_line(sock, line)) {
                cerr << "Resend failed, exiting...\n";
                return 1;
            }
        }

        // wait for response
        string response;
        if (!recv_line(sock, response)) {
            cerr << "[client] server closed connection \n";
            close(sock);

            if (!connect_any_tracker()) {
                cerr << "[client] all trackers unreachable, exiting\n";
                return 1;
            }
            sock = current_sock;
            recv_buff.clear();
            string tmp_buf, tmp_resp;
            if (recv_line(sock, tmp_resp)) {
                if (tmp_resp.rfind("TRACKERS", 0) == 0) {
                    handle_trackers_line(tmp_resp);
                }
            }
            cerr << "[client] switched to another tracker\n";
            continue;
        }

        if (response.rfind("TRACKERS", 0) == 0) {
            handle_trackers_line(response);
            continue;
        }

        cout << response << "\n";

        if (response.rfind("LOGIN_SUCCESS", 0) == 0) {
            istringstream riss(response);
            string tag, user; riss >> tag >> user;
            if (!user.empty()) logged_in_user = user;
            else { istringstream liss(line); string cmd, u; liss >> cmd >> u; if (!u.empty()) logged_in_user = u; }
            static bool heartbeat_started = false;
            if (!heartbeat_started && !logged_in_user.empty()) {
                thread(seeder_heartbeat_thread, sock, logged_in_user).detach();
                heartbeat_started = true;
            }
        } else if (response.find("LOGIN successfull") != string::npos || response.find("LOGIN successful") != string::npos) {
            istringstream liss(line); string cmd, u; liss >> cmd >> u; if (!u.empty()) logged_in_user = u;
            static bool heartbeat_started = false;
            if (!heartbeat_started && !logged_in_user.empty()) {
                thread(seeder_heartbeat_thread, sock, logged_in_user).detach();
                heartbeat_started = true;
            }
        } else if (response.rfind("LOGOUT_SUCCESS", 0) == 0 || response.find("Logged out succesfully") != string::npos || response.find("Logout successful") != string::npos) {
            logged_in_user.clear();
        }
    }

    close(sock);
    return 0;
}
