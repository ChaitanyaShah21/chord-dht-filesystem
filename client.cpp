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
#include "sha1.h"
#include <atomic>

using namespace std;

#define PIECE_SIZE 512*1024 //512KB

int peer_listen_port = 6881;//default port, can be overwritten

struct SeedFile
{
    string groupid;
    string filename;
};

vector<SeedFile> seeding_files;
mutex seeding_mtx;

struct DownloadTask
{
    string groupid;
    string filename;
    string dest_path;
    size_t filesize;
    vector<string> piece_hashes;
    vector<bool> received;
    vector<int> piece_sizes;
};

vector<DownloadTask>active_downloads;
mutex download_mtx;

vector<pair<string,int>> tracker_addrs;
int current_sock = -1;
mutex sock_mtx;
string logged_in_user;
const int CONNECT_RETRIES = 3;

//sending bytes to tracker
bool send_all(int sock, const char *buff, size_t len)
{
    size_t total = 0;
    while(total<len)//handles partial sends, keep sending until all are sent
    {
        ssize_t s = send(sock,buff + total, len-total, 0);
        if(s<=0)//failed to send
        {
            return false;
        }
        total += (size_t)s;

    }

    return true;
}

//send a line 
bool send_line(int sock,const string &line)
{
    string out = line;
    if(out.empty()|| out.back() != '\n')// ensure there is new line at the end
    {
        out.push_back('\n');
    }

    return send_all(sock,out.c_str(),out.size());
}

bool recv_line(int sock, string &buffer, string &out)
{
    out.clear();
    while(true)
    {
        auto pos = buffer.find('\n');//find newline character, which signifies one complete line and read it
        if(pos!= string::npos)
        {
            out = buffer.substr(0,pos);
            buffer.erase(0, pos+1);//remove the read line from the buffer
            return true;
        }

        char tmp[1024];
        ssize_t n = recv(sock,tmp, sizeof(tmp), 0);//if no new line character, we must have not recieved complete data, as sometimes it may require multiple calls, so read more data and add to buffer
        if(n<=0)
        {
            return false;
        }
        buffer.append(tmp, tmp+n);
    }
}

int connect_to_server(const string &ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);//create socket  for ipv4 tcp connection
    if(s<0)//failed to connect
    {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);//convert to port number to network order byte(big endian), as network protcols use big endian but cpu can use little endian
    if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)//converts ip string to a binary version needed by socket, return 0 if string is not valid ip, -1 on error
    {
        close(s);
        return -1;
    }

    if(connect(s, (sockaddr *)&addr, sizeof(addr)) < 0)//try to connect using 3 way handshake, check for failure
    {
        close(s);
        return -1;
    }

    return s;
}

//get local IP used for an existing connected socket(string form)
static string get_local_ip_from_socket(int sock)
{
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);
    if(getsockname(sock, (sockaddr*)&addr, &addrlen) < 0)
    {
        return string("127.0.0.1");
    }

    char buff[INET_ADDRSTRLEN];
    if(inet_ntop(AF_INET, &addr.sin_addr,buff,sizeof(buff))==nullptr)
    {
        return string("127.0.0.1");//default
    }

    return string(buff);
}



bool connect_any_tracker()
{
    for(auto [ip,port]:tracker_addrs)
    {
        int s = connect_to_server(ip,port);
        if(s>=0)
        {
            cerr<<"[client] connect to "<<ip<<":"<<port<<"\n";
            current_sock=s;
            return true;
        }
    }

    return false;
}

void handle_trackers_line(const string &line)
{
    tracker_addrs.clear();
    istringstream iss(line.substr(9));
    string entry;
    while(iss>>entry)
    {
        size_t colon = entry.find(':');
        if(colon!=string::npos)
        {
            string t_ip = entry.substr(0,colon);
            int t_port = stoi(entry.substr(colon+1));
            tracker_addrs.push_back({t_ip,t_port});
        }
    }
    cerr<<"[client] updated tracker list ("<<tracker_addrs.size()<<" entries)\n";
}

//protocol - client to peer: GET_PIECE
//response - PIECE bytes then raw byte exactly bytes lenght
void handle_peer_connection(int peer_sock)
{
    const size_t BUF_SZ = 4096;
    string partial;
    char tmp[BUF_SZ];
    while(true)
    {
        ssize_t n = recv(peer_sock, tmp, sizeof(tmp), 0);
        if(n<=0)
        {
            break;
        }
        partial.append(tmp,n);
        size_t pos;
        while((pos = partial.find('\n')) != string::npos)
        {
            string line = partial.substr(0,pos);
            partial.erase(0,pos+1);

            //trimming line
            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if(start==string::npos)
            {
                continue;
            }
            line = line.substr(start,end-start+1);
            istringstream iss(line);
            string cmd;
            iss>>cmd;

            if(cmd == "GET_PIECE")
            {
                string gid,filename;
                int piece_idx;
                iss>>gid>>filename>>piece_idx;
                if(gid.empty() || filename.empty() || piece_idx <0)
                {
                    send(peer_sock, "ERROR\n",6,0);
                    continue;
                }

                //compute filepath: the client sotres the filemat localpath
                int fd = open(filename.c_str(), O_RDONLY);
                if(fd<0)
                {
                    send(peer_sock,"ERROR\n",6,0);
                    continue;
                }

                off_t piece_offset = (off_t)piece_idx * (off_t)PIECE_SIZE;
                if(lseek(fd,piece_offset,SEEK_SET) == (off_t)-1)
                {
                    close(fd);
                    send(peer_sock, "ERROR\n",6,0);
                    continue;
                }
                
                //check how many bytes remain
                struct stat st;
                if(fstat(fd, &st) <0)
                {
                    close(fd);
                    send(peer_sock, "ERROR\n",6,0);
                    continue;
                }

                size_t bytes_rem = 0;
                if((size_t) piece_offset >= (size_t)st.st_size)
                {
                    bytes_rem = 0;
                }
                else
                {
                    bytes_rem = min((size_t)PIECE_SIZE, (size_t)(st.st_size - piece_offset));
                }

                //send header then bytes
                ostringstream hdr;
                hdr <<"PIECE "<<bytes_rem<<"\n";
                string hdrs = hdr.str();
                if(send(peer_sock, hdrs.c_str(),hdrs.size(), 0) < 0)
                {
                    close(fd);
                    continue;
                }

                // sending file in chunks
                size_t sent = 0;
                char buff[8192];
                while(sent <bytes_rem)
                {
                    ssize_t r = read(fd, buff, min(sizeof(buff), bytes_rem - sent));
                    if(r<=0)
                    {
                        break;
                    }
                    ssize_t w = send(peer_sock,buff,r,0);
                    if(w<=0)
                    {
                        break;
                    }
                    sent += (size_t)w;
                }
                close(fd);
            }
            else
            {
                //unknown command
                send(peer_sock,"UNKNOWN\n",8,0);
            }
        }
    }
    close(peer_sock);
}

void peer_server_thread_func(int port)
{
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if(lsock < 0)
    {
        return;
    }
    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if(bind(lsock, (sockaddr*)&sa, sizeof(sa)) < 0)
    {
        close(lsock);
        return;
    }

    if(listen(lsock, 10) < 0)
    {
        close(lsock);
        return;
    }

    cerr<<"[client] peer-server listening on port "<<port<<"\n";
    while(true)
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int csock = accept(lsock, (sockaddr*)&cli, &len);
        if(csock<0)
        {
            continue;
        }
        thread t(handle_peer_connection, csock);
        t.detach();
    }

    //unreachable
    close(lsock);

}



bool upload_file_to_tracker(const string &filepath, const string &groupid, const string &username,int sock)
{
    int fd = open(filepath.c_str(), O_RDONLY);
    if(fd<0)
    {
        cerr<<"Error: Cannot open file "<<filepath<<"\n";
        return false;
    }

    struct stat st;
    if(fstat(fd, &st) <0)
    {
        cerr<<"Error: Cannot stat file "<<filepath<<"\n";
        close(fd);
        return false;
    }

    size_t filesize = st.st_size;

    vector<string> piece_hashes;
    char buffer[PIECE_SIZE];

    ssize_t n;
    while((n = read(fd, buffer, PIECE_SIZE)) > 0)
    {
        piece_hashes.push_back(sha1_bytes(reinterpret_cast<unsigned char*>(buffer), n));
    }

    if(n<0)
    {
        cerr<<"Error: Read failed for "<<filepath<<"\n";
        close(fd);
        return false;
    }

    close(fd);

    //get local ip of this client socket
    string local_ip = get_local_ip_from_socket(sock);
    ostringstream  useraddr;
    useraddr<<username<<"@"<<local_ip<<":"<<peer_listen_port;
    string user_with_addr = useraddr.str();

    string cmd = "upload_file " + groupid + " " + user_with_addr + " " + filepath + " " + to_string(filesize);
    for(const auto &h:piece_hashes)
    {
        cmd += " " + h;
    }

    if(!send_line(sock, cmd))
    {
        cerr<<"Failed to send upload command\n";
        return false;
    }
    cerr<<"File upload metadata sent for "<<filepath<<"\n";
    return true;
}

bool download_piece(int sock, const string &groupid, const string &filename, size_t piece_index, char *buffer, size_t &out_len)
{
    //send request to tracker orpeer for piece
    string req = "get_piece "+ groupid +" "+filename+" "+to_string(piece_index);
    if(!send_line(sock,req)) 
    {
        return false;
    }

    //read piece size at first
    string line;
    string recv_buf;
    if(!recv_line(sock, recv_buf,line))
    {
        return false;
    }

    out_len = stoul(line); // piece size returned

    size_t total = 0;
    while(total <out_len)
    {
        ssize_t n = recv(sock, buffer + total, out_len - total, 0);
        if(n<=0)
        {
            return false;
        }
        total += (size_t)n;
    }

    return true;
}

// bool start_download(const string &groupid, const string &filename, const string &dest_path, int sock)
// {
//     //request fileinfo from tracker
//     string cmd = "get_file_info "+groupid+" "+filename;
//     if(!send_line(sock,cmd))
//     {
//         return false;
//     }

//     string line;
//     string recv_buf;

//     if(!recv_line(sock,recv_buf,line))
//     {
//         return false;
//     }
//     if(line.rfind("FILE_INFO",0) != 0)
//     {
//         cerr<<"File not found or error\n";
//         return false;
//     }

//     istringstream iss(line);
//     string token;
//     size_t filesize;
//     iss>>token>>token;//skip file info, filename
//     iss>>filesize>>token>>token;//skip owner label, get owner

//     vector<string> piece_hashes;
//     while(iss>>token && token != "SEEDERS")
//     {
//         piece_hashes.push_back(token);
//     }

//     DownloadTask task;
//     task.groupid = groupid;
//     task.filename = filename;
//     task.dest_path = dest_path;
//     task.filesize = filesize;
//     task.piece_hashes = piece_hashes;
//     task.received.assign(piece_hashes.size(), false);

//     // create file
//     int fd = open(dest_path.c_str(),O_CREAT|O_WRONLY, 0666);
//     if(fd <0)
//     {
//         cerr<<"Cannot create destination file\n";
//         return false;
//     }

//     // download pieces sequentially
//     char buffer[PIECE_SIZE];
//     for(size_t i=0; i<piece_hashes.size();i++)
//     {
//         size_t len;
//         if(!download_piece(sock,groupid,filename,i,buffer,len))
//         {
//             cerr<<"Failed to download piece "<<i<<"\n";
//             close(fd);
//             return false;
//         }

//         string hash = sha1_bytes((unsigned char*)buffer, len);
//         if(hash != piece_hashes[i])
//         {
//             cerr<<"Piece hash mismatch at piece "<<i<<"\n";
//             close(fd);
//             return false;
//         }

//         //write to file at offset
//         if(pwrite(fd, buffer,len,i*PIECE_SIZE) != (ssize_t)len)
//         {
//             cerr<<"Write failed at piece "<<i<<"\n";
//             close(fd);
//             return false;
//         }

//         task.received[i] = true;
//         cerr<<"Downloaded piece "<<i+1<<"/"<<piece_hashes.size()<<"\n"; 
//     }

//     close(fd);
//     cerr<<"Download complete: "<<dest_path<<"\n";
//     return true;
// }

int main(int argc, char **argv)
{
    if(argc<3)
    {
        cerr<<"Invalid input.\nUse: ./client <tracker ip> <tracker port>\n";
        return -1;
    }

    string ip = argv[1];
    int port = atoi(argv[2]);

    //connect, try a few times if failure before quiting

    int sock = -1;

    for(int i=0;i<CONNECT_RETRIES;i++)
    {
        sock = connect_to_server(ip,port);
        if(sock >= 0)// success, no need to retry
        {
            break;
        }

        cerr<<"[client] connect failed, retrying["<<i+1<<"/"<<CONNECT_RETRIES<<"]\n";
        this_thread::sleep_for(chrono::seconds(1));
    }

    if(sock<0)
    {
        cerr<<"[client] could not connect to tracker\n";
        return 1;
    }
    cerr<<"[client] connected to "<<ip<<":"<<port<<"\n";
    current_sock =sock;

    if(argc >= 4) {
        peer_listen_port = atoi(argv[3]);
        if(peer_listen_port <= 0) peer_listen_port = 6881;
    }
    thread(peer_server_thread_func, peer_listen_port).detach();

    string init_buff;
    string init_resp;
    if(recv_line(sock,init_buff,init_resp))
    {
        if(init_resp.rfind("TRACKERS",0) == 0)
        {
            handle_trackers_line(init_resp);
        }
    }

    string recv_buff;
    string line;
    while(true)
    {
        cout<<"> ";//prompt lhs
        if(!getline(cin,line))
        {
            break;
        }

        if(line.empty())
        {
            continue;
        }

        //exit command
        if(line == "quit"|| line == "exit")
        {
            cerr<<"[client] exiting\n";
            break;
        }
        if(!logged_in_user.empty())
        {
            istringstream iss(line);
            string cmd;
            iss>>cmd;

            //insert current user name in commands that need it
            vector<string>no_user = {"create_user","login","list_groups","list_members","list_files","quit","exit"};

            if(find(no_user.begin(), no_user.end(),cmd) == no_user.end())
            {
                line = cmd + " " + logged_in_user+ " " + line.substr(cmd.size());
            }
        }

        if(line.rfind("upload_file",0) == 0)
        {
            istringstream iss(line);
            string cmd, groupid, filepath;
            iss>>cmd>>groupid>>filepath;

            if(groupid.empty() || filepath.empty())
            {
                cerr<<"Usage: upload_file <group> <filepath> \n";
                continue;
            }

            if(logged_in_user.empty())
            {
                cerr<<"You must be logged in to upload\n";
                continue;
            }

            if(!upload_file_to_tracker(filepath, groupid, logged_in_user, sock))
            {
                cerr<<"Upload failed\n";
            }

            continue;//skip sending totracker as metadata already sent
        }

        if(line.rfind("download_file", 0) == 0)
        {
            istringstream iss(line);
            string cmd, groupid, filename, destpath;
            iss>>cmd>>groupid>>filename>>destpath;

            if(groupid.empty() || filename.empty() ||destpath.empty())
            {
                cerr<<"Usage: download_file <groupid> <filename> <destination>\n";
                continue;
            }

            if(logged_in_user.empty())
            {
                cerr<<"You must be logged in to download\n";
                continue;
            }

            //ask tracker for file info
            string getinfo_cmd = "get_file_info " + groupid + " " + filename;
            if(!send_line(sock,getinfo_cmd))
            {
                cerr<<"Failed to request file info\n";
                continue;
            }

            string tracker_resp;
            if(!recv_line(sock, recv_buff, tracker_resp))
            {
                cerr<<"Tracker did not respond for file info\n";
                continue;
            }

            if(tracker_resp.rfind("FILE_INFO", 0) != 0)
            {
                cerr<<"Tracker response: "<<tracker_resp<<"\n";
                continue;
            }

            //parse FILE_INFO response
            istringstream pres(tracker_resp);
            string token;
            pres >> token; //file info

            string resp_filename;
            pres>>resp_filename;

            size_t filesize = 0;
            pres>>filesize;

            string owner_marker;
            string owner_str;
            pres>>token;
            if(token == "OWNER")
            {
                pres >> owner_str;
                pres >> token; // next token after owner
            }

            vector<string> piece_hashes;
            //token currently holds first piece hash or "SEEDERS"
            while(token != "SEEDERS" && pres)
            {
                piece_hashes.push_back(token);
                if(!(pres >> token))
                {
                    break;
                }
            }

            vector<string> seeder_entries;
            string s;
            while(pres>>s)
            {
                seeder_entries.push_back(s);
            }

            if(piece_hashes.empty())
            {
                cerr<<"No piece hashes available from tracker\n";
                continue;
            }

            if(seeder_entries.empty())
            {
                cerr<<"No seeders availble for this file\n";
                continue;
            }

            // convert seeder_entries to vector of pairs (ip,port)
            vector<pair<string,int>>seeder_addr;
            for(auto &se: seeder_entries)
            {
                //se could be "user@ip:port" or "ip:port"
                string ipport = se;
                size_t atpos = se.find('@');
                if(atpos != string::npos)
                {
                    ipport = se.substr(atpos+1);
                }

                size_t colon = ipport.find(':');
                if(colon == string::npos)
                {
                    continue;
                }
                string sip = ipport.substr(0,colon);
                int sport = atoi(ipport.substr(colon+1).c_str());
                seeder_addr.push_back({sip, sport});
            }

            if(seeder_addr.empty())
            {
                cerr<<"No valid seeder address parsed\n";
                continue;
            }

            //prepare destination file
            int outfd = open(destpath.c_str(), O_CREAT|O_RDWR|O_TRUNC,0666);
            if(outfd<0)
            {
                cerr<<"Cannot open destination file: "<<destpath<<"\n";
                continue;
            }
            //set file size
            if(ftruncate(outfd,(off_t)filesize) < 0)
            {
                cerr<<"Failed to set file size\n";
                close(outfd);
                continue;
            }

            size_t total_pieces = (filesize + PIECE_SIZE -1)/PIECE_SIZE;
            vector<char> piece_ok(total_pieces,0);
            mutex piece_mtx;
            atomic<size_t> pieces_done{0};

            //worker func to fetch a piece from a chosen seeder
            auto fetch_piece = [&](size_t piece_idx, pair<string,int> peer_addr) -> bool{
                //connect to peer
                int psock = connect_to_server(peer_addr.first, peer_addr.second);
                if(psock < 0)
                {
                    return false;
                }

                // request piece
                ostringstream req;
                req << "GET_PIECE " << groupid << " " << filename << " " << piece_idx << "\n";
                string req_str = req.str();
                if(!send_all(psock, req_str.c_str(), req_str.size())) 
                {
                    close(psock); 
                    return false; 
                }

                // read header line
                string peerbuf;
                string header;
                if(!recv_line(psock, peerbuf, header))
                {
                    close(psock);
                    return false;
                }
                // header "PIECE <bytes>"
                istringstream hs(header);
                string htag;
                size_t bytes;
                hs >> htag >> bytes;
                if(htag != "PIECE" || bytes == 0)
                {
                    close(psock);
                    return false;
                }
                // read bytes exactly
                vector<unsigned char> piecebuf(bytes);
                size_t got = 0;
                while(got < bytes)
                {
                    ssize_t r = recv(psock, (char*)piecebuf.data() + got, bytes - got, 0);
                    if(r <= 0) { close(psock); return false; }
                    got += (size_t)r;
                }
                close(psock);

                // verify sha1
                string hash = sha1_bytes(piecebuf.data(), bytes);
                if(hash != piece_hashes[piece_idx])
                {
                    cerr<<"Piece "<<piece_idx<<" failed hash (from "<<peer_addr.first<<":"<<peer_addr.second<<")\n";
                    return false;
                }

                // write piece to correct offset
                off_t offset = (off_t)piece_idx * (off_t)PIECE_SIZE;
                ssize_t w = pwrite(outfd, piecebuf.data(), bytes, offset);
                if(w != (ssize_t)bytes)
                {
                    cerr<<"Write error for piece "<<piece_idx<<"\n";
                    return false;
                }

                {
                    lock_guard<mutex> lg(piece_mtx);
                    if(!piece_ok[piece_idx]) {
                        piece_ok[piece_idx] = 1;
                        ++pieces_done;
                    }
                }
                return true;
            };

            // Launch thread pool limited to number of seeders or 8 whichever smaller
            size_t max_threads = min((size_t)seeder_addr.size(), (size_t)8);
            vector<thread> workers;
            atomic<size_t> next_piece{0};

            // Each worker will pick pieces in round-robin and attempt retries
            for(size_t t_i=0; t_i<max_threads; ++t_i)
            {
                workers.emplace_back([&]() {
                    while(true)
                    {
                        size_t piece_idx = next_piece.fetch_add(1);
                        if(piece_idx >= total_pieces) break;
                        // attempt to fetch from multiple peers (round-robin with retries)
                        bool success = false;
                        for(size_t attempt_peer = 0; attempt_peer < seeder_addr.size(); ++attempt_peer)
                        {
                            pair<string,int> peer = seeder_addr[(piece_idx + attempt_peer) % seeder_addr.size()];
                            if(fetch_piece(piece_idx, peer))
                            {
                                success = true;
                                break;
                            }
                            // else try next peer
                        }
                        if(!success)
                        {
                            // put back on queue for retry later by increasing next_piece? Simpler: mark failure and continue
                            cerr<<"Failed to download piece "<<piece_idx<<" from all peers\n";
                        }
                    }
                });
            }

            for(auto &th : workers) if(th.joinable()) th.join();

            close(outfd);

            if(pieces_done.load() == total_pieces)
            {
                cerr << "Download completed: " << destpath << "\n";
            }
            else
            {
                cerr << "Download incomplete: " << pieces_done.load() << " / " << total_pieces << " pieces\n";
            }

            continue; // skip sending this line to tracker
            

        }

        if(!send_line(sock,line))
        {
            cerr<<"[client] send failed ... connection lost\n";
            close(sock);

            //try reconnecting
            int new_sock = -1;
            for(int i=0;i<CONNECT_RETRIES;i++)
            {
                new_sock = connect_to_server(ip,port);
                if(new_sock >= 0)
                {
                    break;
                }
                this_thread::sleep_for(chrono::seconds(1));
            }

            if(new_sock<0)
            {
                cerr<<"[client] reconnect failed, trying other trackers\n";

                if(!connect_any_tracker())
                {
                    cerr<<"[client] all trackers unreachable, exiting\n";
                    return 1;
                }
                sock = current_sock;
                recv_buff.clear();

                string tmp_buff, tmp_resp;
                if(recv_line(sock, tmp_buff, tmp_resp))
                {
                    if(tmp_resp.rfind("TRACKERS",0) == 0)
                    {
                        handle_trackers_line(tmp_resp);
                    }
                }
                cerr<<"[client] switched to another tracker\n";
            }

            else
            {
                sock = new_sock;
                recv_buff.clear();
                cerr<<"[client] reconnected\n";
            }


            if(!send_line(sock,line))
            {
                cerr<<"Resend failed, exiting...\n";
                return 1;
            }
        }

        //wait for response
        string response;
        if(!recv_line(sock,recv_buff,response))
        {
            cerr<<"[client] server closed connection \n";
            close(sock);

            if(!connect_any_tracker())
            {
                cerr<<"[client] all trackers unreachable, exiting\n";
                return 1;
            }
            sock = current_sock;
            recv_buff.clear();

            string tmp_buf, tmp_resp;
            if(recv_line(sock,tmp_buf, tmp_resp))
            {
                if(tmp_resp.rfind("TRACKERS", 0) == 0)
                {
                    handle_trackers_line(tmp_resp);
                }
            }
            cerr<<"[client] switched to another tracker\n";
            continue;//dont print blank line when switching trackers
        }

        //recieve trackers list from tracker on connection
        if(response.rfind("TRACKERS",0) == 0)
        {
            handle_trackers_line(response);
            continue;//skip cout<<response line so that client doesent print tracker update request sent 
        }

        cout<<response<<"\n";

        //track login/logout status
        if(response.find("LOGIN successful") != string::npos)
        {
            istringstream iss(line);
            string cmd,user;
            iss>>cmd>>user;
            logged_in_user = user;
        }

        else if(response.find("Logout successful") != string::npos)
        {
            logged_in_user.clear();
        }
    }

    close(sock);
    return 0;
}