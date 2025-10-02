#include <unistd.h>    
#include <sys/types.h>
#include <thread> // for threads
#include <sys/socket.h> // for sockets
#include <arpa/inet.h> 
#include <netinet/in.h>
#include <iostream> 
#include <vector>
#include <string.h> 
#include <string> 
#include <unordered_map> 
#include <stdlib.h> 
#include <unordered_set> 
#include <sstream> 

using namespace std;

// representing peer/client in the P2P network
struct client 
{
    string hostip, hostport, peername, passcode;    // info for peer
    unordered_map<string, string> filmaptopath;     // file map
    bool connected = false;     // checking for connected

    client(string& username, string& code)
        : peername(username), passcode(code), connected(false) {} // constructor

    void login(string& ip, string& port) 
    {
        hostip = ip;        // setting ip
        hostport = port;    // setting port
        connected = true;   // setting connected
    }

    void logout() 
    {
        connected = false;  // setting not connected
        hostip.clear();     // clearing ip
        hostport.clear();   // clearing port
    }
};

// representing a group in the P2P network
class group 
{
public:
    string gid;         // group id
    string groupmaster; // group master
    unordered_set<string> participants, applicants; // members and requests

    group(string id, string name) 
    {
        gid = id; // setting id
        groupmaster = name; // setting master
        participants.insert(name); // adding master to group
    }

    // checking applicant
    bool isapplicant(string s) 
    { 
        return applicants.find(s) != applicants.end(); 
    } 

    // checking if member of a group 
    bool partofgroup(string s) 
    { 
        return participants.find(s) != participants.end(); 
    } 

    void deluser(string s) 
    {
        participants.erase(s);  // removing user
        if (s == groupmaster)   // cehcking if user is master 
        { 
            if (!participants.empty()) 
            {
                groupmaster = *participants.begin(); // new master
                cout << "Group " << gid << " new owner is: " << groupmaster << endl;
            } 
            else 
            {
                groupmaster = ""; // no master
                cout << "Group " << gid << " has no members left" << endl;
            }
        }
    }

    void acceptreq(string s) 
    {
        applicants.erase(s); // removing request
        participants.insert(s); // adding to  group
    }

};

// metadata for shared file : size, hashes, seeders
struct FileMeta 
{
    long long size = 0;     // size of file
    string fullhash;        // hashing of full file
    int num_pieces = 0;     // pieces
    vector<string> piece_hashes;    // piece hashes
    unordered_set<string> peers;    // who has file
};

// maps for tracking users, groups, files, and group-file
unordered_map<string, client*> peers;   // peername to client
unordered_map<string, group*> groups;   // group id to group
unordered_map<string, unordered_set<string>> group_files; // group to files
unordered_map<string, FileMeta> files;  // file to meta

// checking existence of group and user
bool isgrouppresent(string str) 
{
    return groups.find(str) != groups.end(); // checking group
}

bool isuserpresent(string str) 
{
    return peers.find(str) != peers.end(); // checking user
}

// for handling all commands from connected client
void managepeer(int peersocket) 
{
    string disconnecting_user; // user to disconnect
    // main loop which read and process commands from peer
    while (1) 
    {
        // incoming command from socket
        char buff[512000];              // buffer
        memset(buff, 0, sizeof(buff));  // clear buffer

        int bytrd = read(peersocket, buff, sizeof(buff)); // read

        if (bytrd == 0) 
        {
            cout << "Socket received 0 bytes: " << peersocket << endl;
            
            // on disconnect, transfer group ownership if required
            if (!disconnecting_user.empty()) 
            {
                for (auto &gpair : groups) 
                {
                    group *grp = gpair.second;
                    if (grp->groupmaster == disconnecting_user) 
                    {
                        grp->deluser(disconnecting_user); // removing master
                    }
                }
            }
            return;
        }
        
        cout << "Incoming command from socket " << peersocket << ": " << buff << endl;

        // tokenizse command string into args
        vector<string> comds;               // commands
        char *token = strtok(buff, " ");    // spliting
        
        while (token != NULL) 
        {
            comds.push_back(token); // adding
            token = strtok(NULL, " ");
        }
        
        // handling commands checking that it should have at least one token
        if (comds.empty()) 
        {
            string msg = "Invalid command";
            send(peersocket, msg.c_str(), msg.size(), 0);
            continue;
        }

        // tracking user for disconnect  for group ownership transfer
        if ((comds[0] == "login" || comds[0] == "logout") && comds.size() > 1) 
        {
            disconnecting_user = comds[1]; // setting user
        }

        // create_user <username> <passcode>
        if (comds[0] == "create_user") 
        {
            if (comds.size() != 3) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (isuserpresent(comds[1])) 
            {
                string msg = "-----Cannot create user: ID already in use.-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else 
            {
                client* peer = new client(comds[1], comds[2]); // new user
                peers[comds[1]] = peer; // add user
                string msg = "***** ID number " + comds[1] + " registered successfully! ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
                cout << "****** ID " << comds[1] << " has been registered as a new user. ******" << endl;
            }
        }

        // login <username> <passcode> <ip> <port>
        else if (comds[0] == "login") 
        {
            if (comds.size() < 5) 
            {
                string msg = "-----Invalid Arguments for login-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
            else if (!isuserpresent(comds[1])) 
            {
                string msg = "------ User ID " + comds[1] + " is not registered ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (peers[comds[1]]->passcode != comds[2]) 
            {
                string msg = "------ Authentication failed: incorrect passcode for ID " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else 
            {
                peers[comds[1]]->login(comds[3], comds[4]);
                string msg = "Successful Login for User ID " + comds[1] + "! ******\n";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // logout <username>
        else if (comds[0] == "logout") 
        {
            if (comds.size() < 2) 
            {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (!isuserpresent(comds[1])) 
            {
                string msg = "------- No such User ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else 
            {
                peers[comds[1]]->logout(); // logout
                string msg = "***** User ID " + comds[1] + " logged out successfully ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // create_group <groupid> <owner_username>
        else if (comds[0] == "create_group") 
        {
            if (comds.size() < 3) 
            {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (!isuserpresent(comds[2])) 
            {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (isgrouppresent(comds[1])) 
            {
                string msg = "------- This Group ID is already taken ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                group * initgrp = new group(comds[1], comds[2]); // creating new group
                groups[comds[1]] = initgrp; // adding group
                string msg = "******* Group creation successful. Assigned ID: " + comds[1] + " *******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // join_group <groupid> <username>
        else if (comds[0] == "join_group") 
        {
            if (comds.size() < 3) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (!isuserpresent(comds[2])) 
            {
                string msg = "------- No such User ID: " + comds[2] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            }
            else if (!isgrouppresent(comds[1])) 
            {
                string msg = "------- No such group ID: " + comds[1] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (groups[comds[1]]->partofgroup(comds[2])) 
            {
                string msg = "------- You have already joined this group: " + comds[1] + " -------";
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                groups[comds[1]]->applicants.insert(comds[2]); // add request
                string msg = "******* Request to join group " + comds[1] + " has been sent ******";
                send(peersocket, msg.c_str(), msg.size(), 0); 
            }
        }

        // leave_group <groupid> <username>
        else if (comds[0] == "leave_group") 
        {
            if (comds.size() < 3) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!isuserpresent(comds[2])) 
            {
                string msg = "------- No such User ID: " + comds[2] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else if (!isgrouppresent(comds[1])) 
            {
                string msg = "------- No such group ID: " + comds[1] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!groups[comds[1]]->partofgroup(comds[2])) 
            {
                string msg = "------ Access denied. You are not part of Group ID " + comds[1] + " -------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                groups[comds[1]]->deluser(comds[2]); // removing user
                string msg = "****** Left group successfully. ID: " + comds[1] + " ******";
                send(peersocket, msg.c_str(), msg.size(), 0); 
            }
        }

        // list_requests <groupid> <owner_username>
        else if (comds[0] == "list_requests") 
        {
            if (comds.size() < 3) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!isuserpresent(comds[2])) 
            {
                string msg = "------- No such User ID: " + comds[2] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!isgrouppresent(comds[1])) 
            {
                string msg = "------- No such group ID: " + comds[1] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (groups[comds[1]]->groupmaster != comds[2]) 
            {
                string msg = "------ Access denied. You are not the group owner of ID " + comds[1] + " -------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                string msg = ""; // message
                for (const auto &user : groups[comds[1]]->applicants) msg += user + "\n"; // add requests
                if (msg == "") msg = "------- Group ID " + comds[1] + " has no pending join requests -------"; // no requests
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // accept_request <groupid> <applicant_username> <owner_username>
        else if (comds[0] == "accept_request") 
        {
            if (comds.size() < 4) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!isuserpresent(comds[2])) 
            {
                string msg = "------- No such User ID: " + comds[2] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!isgrouppresent(comds[1])) 
            {
                string msg = "------- No such group ID: " + comds[1] + " ------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (groups[comds[1]]->groupmaster != comds[3]) 
            {
                string msg = "------ Access denied. You are not the group owner of ID " + comds[1] + " -------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else if (!groups[comds[1]]->isapplicant(comds[2])) 
            {
                string msg = "------- This user (ID: " + comds[2] + ") has no pending requests -------"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                groups[comds[1]]->acceptreq(comds[2]); // accept
                string msg = "******* Approval granted for User ID: " + comds[2] + " *******"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            }
        }

        // list_groups
        else if (comds[0] == "list_groups") 
        {
            string msg = "############### Available groups on the network ###############";
            for (auto it = groups.begin(); it != groups.end(); it++) 
            {
                msg += "\n" + it->first; // adding group
            }
            if (msg == "") msg = "-------- Currently, no groups are available. -------"; // no groups are there
            send(peersocket, msg.c_str(), msg.size(), 0);
        }

    // upload_file <gid> <filename> <filePath>
        else if (comds[0] == "upload_file") 
        {
            if (comds.size() < 4) 
            {
                string msg = "-----Invalid Arguments for upload_file-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else 
            {
                string gid = comds[1]; // group id
                string fname = comds[2]; // file name
                string uname = comds[3]; // user name
                long long fsize = atoll(comds[4].c_str()); // file size
                string fhash = comds[5]; // file hash
                int num_pieces = stoi(comds[6]); // pieces

                if (!isgrouppresent(gid)) 
                {
                    string msg = "------- No such group ID: " + gid + " ------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) 
                {
                    string msg = "------ You are not part of Group ID " + gid + " -------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else 
                {
                    // read piece hashes from comds[7...]
                    FileMeta &fm = files[fname]; // file meta
                    fm.size = fsize; // seting size
                    fm.fullhash = fhash; // seting hash
                    fm.num_pieces = num_pieces; // seting pieces
                    fm.piece_hashes.clear(); // clearing hashes
                    for (int i = 0; i < num_pieces; ++i) 
                    {
                        if ((int)comds.size() > 7 + i)
                        {
                            fm.piece_hashes.push_back(comds[7 + i]); // adding hash
                        }
                        else 
                        {
                            fm.piece_hashes.push_back(string()); // empty
                        }
                    }
                    fm.peers.insert(uname); // adding peer
                    group_files[gid].insert(fname); // adding file

                    string msg = "******* File " + fname + " uploaded to group " + gid + " successfully *******"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                    cout << "Tracker: Registered file " << fname << " size " << fsize << " pieces " << num_pieces << endl; 
                }
            }
        }

        // list_files <gid>
        else if (comds[0] == "list_files") 
        {
            if (comds.size() < 3) 
            {
                string msg = "-----Invalid Arguments-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                string gid = comds[1]; // group id
                string uname = comds[2]; // user name
                if (!isgrouppresent(gid)) 
                {
                    string msg = "------- No such group ID: " + gid + " ------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) 
                {
                    string msg = "------ Access denied. You are not part of Group ID " + gid + " -------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else 
                {
                    string msg;
                    if (group_files.find(gid) == group_files.end() || group_files[gid].empty()) 
                    {
                        msg = "------- No files uploaded in group " + gid + " -------"; // no files
                    } 
                    else 
                    {
                        msg = "######## Files in Group " + gid + " ########\n";
                        for (const auto &fname : group_files[gid]) 
                        {
                            FileMeta &fm = files[fname]; // file meta
                            msg += fname + " SIZE:" + to_string(fm.size) + " PIECES:" + to_string(fm.num_pieces) + "\n"; // adding file
                        }
                    }
                    send(peersocket, msg.c_str(), msg.size(), 0);
                }
            }
        }

        // download_file <gid> <filename> <dest path>
        // file metadata and list of seeders
        else if (comds[0] == "download_file") 
        {
            if (comds.size() < 4) 
            {
                string msg = "-----Invalid Arguments for download_file-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } 
            else 
            {
                string gid = comds[1]; // group id
                string fname = comds[2]; // file name
                string uname = comds[3]; // user name

                if (!isgrouppresent(gid)) 
                {
                    string msg = "------- No such group ID: " + gid + " ------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) 
                {
                    string msg = "------ Access denied. You are not part of Group ID " + gid + " -------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (group_files[gid].find(fname) == group_files[gid].end()) 
                {
                    string msg = "------- No such file in group " + gid + " -------"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else 
                {
                    FileMeta &fm = files[fname]; // file meta
                    string msg = "FILE " + fname + " SIZE " + to_string(fm.size) + " HASH " + fm.fullhash + " PIECES " + to_string(fm.num_pieces) + " PIECE_HASHES";
                    for (auto &h : fm.piece_hashes)
                    {
                        msg += " " + h; // adding hashes
                    }
                    msg += "\nPEERS\n";
                    
                    for (const string &peer : fm.peers) 
                    {
                        if (isuserpresent(peer) && peers[peer]->connected) 
                        {
                            msg += peer + " " + peers[peer]->hostip + " " + peers[peer]->hostport + "\n"; // add peer
                        }
                    }
                    msg += "\n";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                }
            }
        }

        // file_downloaded <gid> <filename> <peername>
        // will notify tracker that peer completed download and can now serve file
        else if (comds[0] == "file_downloaded") 
        {
            if (comds.size() != 4) 
            {
                string msg = "-----Invalid Arguments for file_downloaded-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } 
            else 
            {
                string gid = comds[1]; // group id
                string filename = comds[2]; // file name
                string peername = comds[3]; // peer name

                if (groups.find(gid) != groups.end() && groups[gid]->participants.find(peername) != groups[gid]->participants.end()) 
                {
                    if (group_files.find(gid) != group_files.end() && group_files[gid].find(filename) != group_files[gid].end()) 
                    {
                        if (peers.find(peername) != peers.end()) 
                        {
                            peers[peername]->filmaptopath[filename] = filename; // adding file
                            if (files.find(filename) != files.end()) 
                            {
                                files[filename].peers.insert(peername); // adding peer
                            }
                            string msg = "SUCCESS: Peer " + peername + " registered as seeder for " + filename;
                            send(peersocket, msg.c_str(), msg.size(), 0); 
                        } 
                        else 
                        {
                            string msg = "ERROR: Peer not found"; 
                            send(peersocket, msg.c_str(), msg.size(), 0); 
                        }
                    } 
                    else 
                    {
                        string msg = "ERROR: File not found in group"; 
                        send(peersocket, msg.c_str(), msg.size(), 0); 
                    }
                } 
                else 
                {
                    string msg = "ERROR: Group not found or peer not member"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                }
            }
        }

        // stop_share <gid> <filename> <peername>
        // removing peer from file's seeder list for group
        else if (comds[0] == "stop_share") 
        {
            if (comds.size() != 4) 
            {
                string msg = "-----Invalid Arguments for stop_share-----"; 
                send(peersocket, msg.c_str(), msg.size(), 0); 
            } else {
                string gid = comds[1]; // group id
                string filename = comds[2]; // file name
                string peername = comds[3]; // peer name
                if (!isgrouppresent(gid)) 
                {
                    string msg = "ERROR: Group not found"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (!isuserpresent(peername) || groups[gid]->participants.find(peername) == groups[gid]->participants.end()) 
                {
                    string msg = "ERROR: Peer not found or not member of group"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (group_files[gid].find(filename) == group_files[gid].end()) 
                {
                    string msg = "ERROR: File not found in group"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else if (files.find(filename) == files.end()) 
                {
                    string msg = "ERROR: File metadata not found"; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                } 
                else 
                {
                    files[filename].peers.erase(peername); // removing peer
                    if (peers.find(peername) != peers.end()) 
                    {
                        peers[peername]->filmaptopath.erase(filename); // removing file
                    }
                    string msg = "SUCCESS: Peer " + peername + " stopped sharing " + filename + " in group " + gid; 
                    send(peersocket, msg.c_str(), msg.size(), 0); 
                }
            }
        }

        // unrecogniszed command
        else 
        {
            string msg = "Unrecognized command"; 
            send(peersocket, msg.c_str(), msg.size(), 0); 
        }
    }
}

int main(int argc, char *argv[]) 
{
    if (argc != 3) 
    {
        cout << "-----Invalid Arguments-----" << endl;
        return 0;
    }

    // reading tracker IP and port from config file
    FILE *filconfig = fopen(argv[1], "r"); // opening file
    if (!filconfig) 
    { 
        cout << "Failed to open tracker info file" << endl; 
        return 0; 
    } 

    char ipbuf[128], portbuf[32]; // buffers
    if (fscanf(filconfig, "%127s %31s", ipbuf, portbuf) != 2) 
    { 
        cout << "Failed to read tracker info" << endl; 
        fclose(filconfig); 
        return 0; 
    } 

    fclose(filconfig); // closing file
    string serverip = ipbuf; // setting ip
    string serverport = portbuf; // setting port

    // TCP socket
    int serversock; // socket
    struct sockaddr_in serveradd; // address

    // creating server socket
    serversock = socket(AF_INET, SOCK_STREAM, 0); // socket
    if (serversock == 0) 
    {
        cout << "------- Error: Could not create socket -------" << endl;
        return 0;
    }

    // setting socket options for address reuse
    int choice = 1; // option
    if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &choice, sizeof(choice)) != 0) 
    {
        cout << "------- Unable to configure socket options ------" << endl;
        return 0;
    }

    // binding server to IP and port
    serveradd.sin_family = AF_INET;
    serveradd.sin_addr.s_addr = INADDR_ANY; // any address
    int port = stoi(serverport); // port
    serveradd.sin_port = htons(port); // setting port

    if (bind(serversock, (struct sockaddr *)&serveradd, sizeof(serveradd)) < 0) 
    {
        cout << "------ Unable to bind socket ------" << endl;
        return 0;
    }

    // listening for incoming connections
    if (listen(serversock, 20) < 0) 
    {
        cout << "------- Unable to start listening on socket ---------" << endl;
        return 0;
    }

    // server startup info and available commands
    cout << "\n=========================================\n"; 
    cout << "          TRACKER SERVER STARTED         \n"; 
    cout << "=========================================\n"; 
    cout << "Listening on IP: " << serverip << "  Port: " << serverport << endl; 
    cout << "Tracker is now running...\n"; 
    cout << "-----------------------------------------\n"; 
    cout << "Available Tracker Commands (from console):\n"; 
    cout << "   quit   -> Stop the tracker server\n"; 
    cout << "-----------------------------------------\n\n"; 

    int incomsock; // incoming socket
    vector<thread> peerss; // threads
    int length = sizeof(serveradd);

    // thread to handle console input 
    thread exit_thread([]() 
    {
        string inp;
        while (true) 
        {
            getline(cin, inp);
            if (inp == "quit")
            {
                exit(0);
            } 
        }
    });
    exit_thread.detach(); // detach

    // handle incoming client connections
    while (1) 
    {
        if ((incomsock = accept(serversock, (struct sockaddr *)&serveradd, (socklen_t *)&length)) < 0) 
        {
            cout << "------- Unable to accept incoming connection -------" << endl;
            continue;
        }
        cout << "******* Client accepted at socket: " << incomsock << " ******" << endl;
        peerss.push_back(thread(managepeer, incomsock)); // adding thread
    }

    // join all peer threads before exiting
    for (int i = 0; i < peerss.size(); i++)
    {
        peerss[i].join(); // join
    }
    return 0;
}
