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

//******todo:make it so that all missed updates are synced when peer gets online, also if pne tracker goes offline client should sitch to other peer tracker availible************/
using namespace std;

unordered_map<string,string> users; //username,password
unordered_set<string> online_users;

atomic<size_t> next_seq{0};//sequence number for updates

int listen_at_port = 0;//needed to send tracker list

vector<string> update_log;

struct FileInfo
{
    string owner;
    size_t size;
    vector<string> piece_hashes; // hashes for each piece
    unordered_set<string> seeders;
};
//groupid => filename => fileinfo
unordered_map<string, unordered_map<string, FileInfo>> group_files;


struct Group
{
    string owner;
    unordered_set<string> members;
    unordered_set<string> pending;
};

unordered_map<string, Group>groups;

mutex state_mtx;//just a lock/unlock variable to handle race condition if multiple clients try to modify same resource
mutex peer_mtx;
mutex log_mtx;
vector<int> peer_sockets;// connections to other peers
vector<size_t> peer_last_seq;

vector<pair<string,int>> peer_addrs; //remember peer address

int connect_to_peer(const string &ip,int port);
std::string state_filename;

void append_update_to_file(const std::string &update)// store updates to file so that data is saved even if all trackers go down
{
    lock_guard<mutex> log_lock(log_mtx);
    size_t seq = next_seq++;
    ostringstream oss;
    oss<<seq<<" "<<update;
    string line = oss.str();

    update_log.push_back(line);
    ofstream ofs(state_filename, ios::app);

    if(ofs)
    {
        ofs<<line<<"\n";
    }
}

bool send_all(int sock, const string &msg)
{
    string out = msg;
    if(out.empty() || out.back() != '\n') 
    {
        out.push_back('\n');
    }
    size_t total = 0;
    while(total < out.size())
    {
        ssize_t n = send(sock,out.c_str() + total, out.size() - total, 0);
        if(n<=0)
        {
            return false;
        }
        total +=n;
    }
    return true;
}



void peer_reconnect_thread()
{
    while(true)
    {
        this_thread::sleep_for(chrono::seconds(5));
        for(size_t i = 0; i<peer_addrs.size(); i++)
        {
            if(i<peer_sockets.size() && peer_sockets[i] >= 0)
            {
                continue; //already connected
            }

            auto[ip,port] = peer_addrs[i];
            int s = connect_to_peer(ip,port);
            if(s>=0)
            {
                lock_guard<mutex> log_lock(log_mtx);
                size_t last_sent = (i<peer_last_seq.size()) ? peer_last_seq[i]:0;
                for(size_t j = last_sent; j<update_log.size(); j++)
                {
                    send_all(s,"SYNC "+update_log[j]);
                }
                peer_last_seq[i] = update_log.size();//update last seen index
                cerr<<"[tracker] reconnected to peer "<<ip<<":"<<port<<"\n";
                lock_guard<mutex> lock(peer_mtx);
                if(i<peer_sockets.size())
                {
                    peer_sockets[i] = s;
                }
                else
                {
                    peer_sockets.push_back(s);
                }
            }
        }
    }
}

int connect_to_peer(const string &ip,int port)
{
    int s = socket(AF_INET, SOCK_STREAM,0);
    if(s<0)
    {
        return -1;
    }    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
    {
        close(s);
        return -1;
    }

    if(connect(s,(sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }

    return s;
}

void broadcast_sync(const string &cmdline, int exclude_sock = -1)
{
    lock_guard<mutex> lock(log_mtx);
    string msg = "SYNC " + update_log.back();
    lock_guard<mutex> lock(peer_mtx);
    for(size_t i=0; i<peer_sockets.size();i++)
    {
        int sock = peer_sockets[i];
        if(sock<0 || sock == exclude_sock)
        {
            continue;
        }
        if(send_all(sock,msg))
        {
            peer_last_seq[i] = update_log.size();//all updates sent to peer
        }
        else
        {
            cerr<<"[tracker] peer socket "<<sock<<" disconnected\n";
            close(sock);
            peer_sockets[i] = -1;
        }
    }
}

string handle_command(const string &cmdline, const string &client_user="", bool record = true) 
//*********right now any client can send command for any user, change that later*******
{
    istringstream iss(cmdline);
    string cmd;
    iss>>cmd;
    if(cmd.empty())
    {
        return "Error- Empty Command";
    }

    lock_guard<mutex> lock(state_mtx);

    //user commands

    if(cmd == "create_user")
    {
        string user,pass;
        iss>>user>>pass;
        if(user.empty() || pass.empty())
        {
            return "Invalid input\nUse: create_user <user> <pass>";
        }
        if(users.count(user))
        {
            return "User already exists";
        }

        users[user] = pass;
        if(record)
        {
            append_update_to_file(cmdline);
        }
        return "User created succesfully";
    }

    else if(cmd=="login")
    {
        string user,pass;
        iss>>user>>pass;
        if(user.empty()||pass.empty())
        {
            return "Invalid Input\nUse:login <user> <pass>";
        }
        if(!users.count(user))
        {
            return "User doesn't exist";
        }
        if(users[user] != pass)
        {
            return "Invalid password";
        }
        if(online_users.count(user))
        {
            return "Error - User already logged in";
        }

        online_users.insert(user);

        if(record)
        {
            append_update_to_file(cmdline);
        }    
        return "LOGIN_SUCCESS " + user;
    }

    else if(cmd == "logout")
    {
        string user;
        iss>>user;
        if(user.empty() || client_user.empty() || user!= client_user)
        {
            return "Error: You can only log out yourself";
        }
        if(!online_users.count(user))
        {
            return "User not logged in";
        }
        online_users.erase(user);
        if(record)
        {
            append_update_to_file(cmdline);
        }    
        return "Logged out succesfully";
    }

    //group commands

    else if(cmd == "create_group")
    {
        string gid,owner;
        iss>>gid>>owner;

        if(gid.empty()||owner.empty())
        {
            return "Invalid input.\nUse: create_group <groupid> <owner>";
        } 
        
        if(client_user.empty() || owner != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }
        if(!online_users.count(owner))
        {
            return "Owner must be logged in";
        }
        if(groups.count(gid))
        {
            return "Group already exists";
        }

        Group g;
        g.owner = owner;
        g.members.insert(owner);
        groups[gid] = g;
        if(record)
        {
            append_update_to_file(cmdline);
        }    
        return "Group Created";
    }

    else if(cmd == "join_group")
    {
        string gid,user;
        iss>>gid>>user;

        if(gid.empty()||user.empty())
        {
            return "Invalid input.\nUse: join_group <groupid> <user>";
        }

        if(client_user.empty() || user != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }
        if(!online_users.count(user))
        {
            return "User must be logged in";
        }
        if(!groups.count(gid))
        {
            return "Group not found";
        }
        if(groups[gid].members.count(user))
        {
            return "Already a member";
        }

        groups[gid].pending.insert(user);
        if(record)
        {
            append_update_to_file(cmdline);
        }    
        return "Joining request sent(waiting for approval)";
    }

    else if(cmd == "list_requests")
    {
        string gid,owner;
        iss>>gid>>owner;
        
        if(gid.empty()||owner.empty())
        {
            return "Invalid input.\nUse: list_requests <groupid> <owner>";
        }

        if(client_user.empty() || owner != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }

        if(!groups.count(gid))
        {
            return "Group not found";
        }
        Group &g = groups[gid];
        if(g.owner != owner)
        {
            return "Only owner can view reuests";
        }
        if(g.pending.empty())
        {
            return "No pending requests";
        }

        ostringstream oss;
        oss<<"Pending:";
        for(auto &u : g.pending)
        {
            oss<<" "<<u;
        }

        return oss.str();
    }

    else if(cmd == "accept_request")
    {
        string gid,owner,user;
        iss>>gid>>owner>>user;

        if(gid.empty()||owner.empty()||user.empty())
        {
            return "Invalid input.\nUse: accept_request <groupid> <owner> <user>";
        }
        if(client_user.empty() || owner != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }
        if(!groups.count(gid))
        {
            return "No such group";
        }
        Group &g = groups[gid];
        if(g.owner != owner)
        {
            return "Only owner can accept requests";
        }

        if(!g.pending.count(user))
        {
            return "No pending request by the user";
        }

        g.pending.erase(user);
        g.members.insert(user);
        if(record)
        {
            append_update_to_file(cmdline);
        }    
        return "User added to group";
    }

    else if(cmd == "list_groups")
    {
        if(groups.empty())
        {
            return "No groups";
        }
        ostringstream oss;
        oss<<"Groups:";
        for(auto &p:groups)
        {
            oss<<" "<<p.first<<"(leader:"<<p.second.owner<<")";
        }


        return oss.str();
    }

    else if(cmd == "list_members")
    {
        string gid;
        iss>>gid;
        if(gid.empty())
        {
            return "Invalid input.\nUse: list_members <groupid>";
        }
        if(!groups.count(gid))
        {
            return "Group not found";
        }

        Group &g = groups[gid];
        if(g.members.empty())
        {
            return "Group is empty";
        }

        ostringstream oss;
        oss<<"Members:";
        for(auto &m: g.members)
        {
            oss<<" "<<m;
        }
        return oss.str();
    }

    else if(cmd == "leave_group")
    {
        string gid,user;
        iss>>gid>>user;

        if(gid.empty()||user.empty())
        {
            return "Invalid input.\nUse: leave_group <groupid> <user>";
        }

        if(client_user.empty() || user != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }

        if(!groups.count(gid))
        {
            return "Group not found";
        }

        Group &g = groups[gid];
        if(!g.members.count(user))
        {
            return "User not a member of this group";
        }

        if(g.owner == user)//owner wants to leave, new owner needed
        {
            g.members.erase(user);

            if(g.members.empty())
            {
                groups.erase(gid);
                if(record)
                {
                    append_update_to_file(cmdline);
                }    
                return "No members left, deleting group";
            }
            else
            {
                string new_owner = *g.members.begin();
                g.owner = new_owner;
                if(record)
                {
                    append_update_to_file(cmdline);
                }    
                return "Owner left, new leader: " + new_owner;
            }
        }

        else
        {
            g.members.erase(user);
            if(record)
            {
                append_update_to_file(cmdline);
            }    
            return "User left";
        }
    }

    else if(cmd == "upload_file")
    {
        string gid, user, filename, size_str;
        iss>>gid>>user>>filename>>size_str;

        if(gid.empty() || user.empty() || filename.empty() || size_str.empty())
        {
            return "Invalid input.\nUse: upload_file <groupid> <user> <filename> <size> <piecehashes...>";
        }
        if(client_user.empty() || user != client_user)
        {
            return "Error: you can only perform this command as yourself";
        }
        if(!groups.count(gid))
        {
            return "Group not found";
        }
        if(!groups[gid].members.count(user))
        {
            return "User not in group";
        }

        size_t filesize = stoull(size_str);

        //read piece hashes from command
        vector<string> hashes;
        string h;
        while(iss>>h)
        {
            hashes.push_back(h);
        }

        if(hashes.empty())
        {
            return "Error: no piece hashes given";
        }

        FileInfo fi;
        fi.owner = user;
        fi.size = filesize;
        fi.piece_hashes = hashes;
        fi.seeders.insert(user);

        group_files[gid][filename] = fi;
        if(record)
        {
            append_update_to_file(cmdline);
        }    

        return "File uploaded: " + filename;

    }

    else if(cmd == "list_files")
    {
        string gid;
        iss >> gid;

        if(gid.empty())
        {
            return "Invalid input.\nUse: list_files <groupid>";
        }

        if(!groups.count(gid))
        {
            return "Group not found";
        }
        
        if(!group_files.count(gid) || group_files[gid].empty())
        {
            return "No files in the group";
        }

        ostringstream oss;
        oss<<"Files in group "<<gid<<":";

        for(auto &p: group_files[gid])
        {
            const FileInfo &fi = p.second;
            oss<< " "<<p.first<<"(size:"<<fi.size<<" bytes, owner: "<<fi.owner<<")"; 
        }

        return oss.str();
    }

    else
    {
        return "Unknown command";
    }

}


void client_handler(int client_sock)//create tcp connection with client and peers
{
    string logged_in_user;
    ostringstream oss;
    oss<<"TRACKERS 127.0.0.1:"<<listen_at_port;
    for(auto [ip,port]:peer_addrs)
    {
        oss<<" "<<ip<<":"<<port;
    }
    send_all(client_sock, oss.str());


    char buffer[1024];
    string partial;
    while(true)//keep running until cleint closes connection or error
    {
        
        ssize_t n = recv(client_sock, buffer, sizeof(buffer)-1,0);
        if(n<=0)//0=client discconected, negative= connection failed
        {
            cerr<<"[tracker] client disconnect\n";
            break;
        }

        buffer[n] = '\0';//add null termination
        partial.append(buffer,n);

        size_t pos;
        while((pos = partial.find('\n')) != string::npos)
        {
            string line = partial.substr(0,pos);
            partial.erase(0,pos+1);

            size_t endpos = line.find_last_not_of(" \t\r\n");
            if(endpos != string::npos) 
            {
                line.erase(endpos +1);
            }

            size_t startpos = line.find_first_not_of(" \t\r\n");
            if(startpos != string::npos)
            {
                line.erase(0,startpos);
            }

            if(line.empty())
            {
                continue;
            }

            if(line.rfind("SYNC",0) == 0)//syncing tracker
            {
                string rest = line.substr(5);   
                size_t space_pos = rest.find(' ');
                if(space_pos == string::npos)
                {
                    continue;
                }
                size_t seq = stoull(rest.substr(0,space_pos));
                string cmd = rest.substr(space_pos+1);
                if(seq<next_seq)//skip if already done
                {
                    continue;
                }
                handle_command(cmd,"",false);
                lock_guard<mutex> log_lock(log_mtx);
                update_log.push_back(rest);
                next_seq = seq+1;
            }
            else
            {
                cerr<<"[tracker] recieved: "<<line<<"\n";
                string resp = handle_command(line, logged_in_user);

                if(resp.rfind("LOGIN_SUCCESS ",0) == 0)
                {
                    logged_in_user = resp.substr(14);
                    resp = "LOGIN successfull";
                }
                send_all(client_sock, resp);
                broadcast_sync(line);
            }    

        }
    }
    close(client_sock);//close client socket from server
}


int main(int argc, char** argv)
{
    if(argc<2)
    {
        cerr<<"Invalid Input\nUse: ./tracker <listen_port>\n";
        return 1;
    }


    listen_at_port = atoi(argv[1]); 

    state_filename = "state_" + std::to_string(listen_at_port)+".log";

    ifstream ifs(state_filename);
    string prev;
    while(getline(ifs,prev))//load log changes in memory from file
    {
        if(prev.empty())
        {
            continue;
        }

        lock_guard<mutex> lock(log_mtx);
        update_log.push_back(prev);

        //update next_seq
        size_t space_pos = prev.find(' ');
        if(space_pos != string::npos)
        {
            size_t seq = stoull(prev.substr(0,space_pos));
            if(seq >= next_seq)
            {
                next_seq = seq +1;
            }
        }
    }

    for(const auto &u: update_log)
    {
        size_t space = u.find(' ');
        string cmdline = (space!=string::npos)?u.substr(space+1):u;//to removesequence number before handling command
        handle_command(u,"",false);
    }

    //connecting to peer
    //*******in future implement peer groups so that every tracker automically syns to all linked trackers without mentioning at time of creation********/
    for(int i=2;i<argc;i++)
    {
        string peer = argv[i];
        size_t colon = peer.find(':');
        if(colon == string::npos)
        {
            continue;
        }
        string ip = peer.substr(0,colon);
        int port = stoi(peer.substr(colon+1));
        peer_addrs.push_back({ip,port});//*********future: check if valid ip before pushing it*********/
        int psock = connect_to_peer(ip,port);
        if(psock>=0)
        {
            cerr<<"[tracker] connected to peer"<<peer<<"\n";
            peer_sockets.push_back(psock);
            peer_last_seq.push_back(update_log.size());
        }

        else
        {
            cerr<<"[tracker] failed to connect peer "<<peer<<"\n";
            peer_sockets.push_back(-1);//not connected
            peer_last_seq.push_back(0);
        }
    }

    thread(peer_reconnect_thread).detach();//create a new thread that just attempts to reconnect to peers

    //creating socket
    int listen_at_socket = socket(AF_INET, SOCK_STREAM, 0);//afinet- ipv4, sock_stream connection based tcp, 0-default protoc (tcp)
    if(listen_at_socket < 0)
    {
        perror("socket");
        return 1;
    }

    //binding socket to port

    sockaddr_in sa{};
    sa.sin_family = AF_INET;//ipv4
    sa.sin_port = htons(listen_at_port);//set port, but first convert port to network byte order using htons
    sa.sin_addr.s_addr = INADDR_ANY;//accept connections on any local id


    if(bind(listen_at_socket, (sockaddr*)&sa, sizeof(sa)) < 0)
    {
        perror("bind");
        return 1;
    }  
    
    
    //start listening

    if(listen(listen_at_socket,10)<0)//listen at port,keep utpo 10 request in queue until it is accepted
    {
        perror("listen");
        return 1;
    }

    cerr<<"[tracker] listening on port "<<listen_at_port<<"\n";
    //keep accepting clients
    while(true)
    {
        sockaddr_in cli{};//zero-initialiaises{} a variable of type sockaddr
        socklen_t len = sizeof(cli);//so that we can tell the size of variable where we have to store client details
        
        int client_sock = accept(listen_at_socket, (sockaddr*)&cli,&len);
        if(client_sock < 0)//failed to accept
        {
            perror("accept");
            break;
        }

        cerr<<"[tracker] client connected\n";
        thread t(client_handler, client_sock);//create a thread for each client
        t.detach();//detach thread from program, so that program doesnt have to wait for thread to exit
    }

    close(listen_at_socket);
    
    return 0;
}