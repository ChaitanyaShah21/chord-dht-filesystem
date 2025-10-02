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
#include "sha1.h"

using namespace std;

#define PIECE_SIZE 512*1024 //512KB

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

    string cmd = "upload_file " + groupid + " " + username + " " + filepath + " " + to_string(filesize);
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

bool start_download(const string &groupid, const string &filename, const string &dest_path, int sock)
{
    //request fileinfo from tracker
    string cmd = "get_file_info "+groupid+" "+filename;
    if(!send_line(sock,cmd))
    {
        return false;
    }

    string line;
    string recv_buf;

    if(!recv_line(sock,recv_buf,line))
    {
        return false;
    }
    if(line.rfind("FILE_INFO",0) != 0)
    {
        cerr<<"File not found or error\n";
        return false;
    }

    istringstream iss(line);
    string token;
    size_t filesize;
    iss>>token>>token;//skip file info, filename
    iss>>filesize>>token>>token;//skip owner label, get owner

    vector<string> piece_hashes;
    while(iss>>token && token != "SEEDERS")
    {
        piece_hashes.push_back(token);
    }

    DownloadTask task;
    task.groupid = groupid;
    task.filename = filename;
    task.dest_path = dest_path;
    task.filesize = filesize;
    task.piece_hashes = piece_hashes;
    task.received.assign(piece_hashes.size(), false);

    // create file
    int fd = open(dest_path.c_str(),O_CREAT|O_WRONLY, 0666);
    if(fd <0)
    {
        cerr<<"Cannot create destination file\n";
        return false;
    }

    // download pieces sequentially
    char buffer[PIECE_SIZE];
    for(size_t i=0; i<piece_hashes.size();i++)
    {
        size_t len;
        if(!download_piece(sock,groupid,filename,i,buffer,len))
        {
            cerr<<"Failed to download piece "<<i<<"\n";
            close(fd);
            return false;
        }

        string hash = sha1_bytes((unsigned char*)buffer, len);
        if(hash != piece_hashes[i])
        {
            cerr<<"Piece hash mismatch at piece "<<i<<"\n";
            close(fd);
            return false;
        }

        //write to file at offset
        if(pwrite(fd, buffer,len,i*PIECE_SIZE) != (ssize_t)len)
        {
            cerr<<"Write failed at piece "<<i<<"\n";
            close(fd);
            return false;
        }

        task.received[i] = true;
        cerr<<"Downloaded piece "<<i+1<<"/"<<piece_hashes.size()<<"\n"; 
    }

    close(fd);
    cerr<<"Download complete: "<<dest_path<<"\n";
    return true;
}

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
    const int CONNECT_RETRIES = 3;

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
            string cmd, groupid, filename, dest;
            iss>>cmd>>groupid>>filename>>dest;

            if(groupid.empty() || filename.empty() ||dest.empty())
            {
                cerr<<"Usage: download_file <groupid> <filename> <destination>\n";
                continue;
            }

            if(logged_in_user.empty())
            {
                cerr<<"You must be logged in to download\n";
                continue;
            }

            start_download(groupid, filename, dest, sock);
            continue;
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