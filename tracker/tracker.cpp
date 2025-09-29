// tracker.cpp  (updated)
// Adds file metadata: per-piece hashes, upload_file, list_files, download_file

#include <unistd.h>
#include <sys/types.h>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <iostream>
#include <vector>
#include <string.h>
#include <string>
#include <unordered_map>
#include <stdlib.h>
#include <fstream>
#include <unordered_set>
#include <sstream>

using namespace std;

// representing peers
struct client {
    string hostip, hostport, peername, passcode;
    unordered_map<string, string> filmaptopath;
    bool connected = false;

    client(string& username, string& code)
        : peername(username), passcode(code), connected(false) {}

    void login(string& ip, string& port) {
        hostip = ip;
        hostport = port;
        connected = true;
    }

    void logout() {
        connected = false;
        hostip.clear();
        hostport.clear();
    }
};

// representing group
class group {
public:
    string gid;
    string groupmaster;
    unordered_set<string> participants, applicants;

    group(string id, string name) {
        gid = id;
        groupmaster = name;
        participants.insert(name);
    }

    bool isapplicant(string s) { return applicants.find(s) != applicants.end(); }
    bool partofgroup(string s) { return participants.find(s) != participants.end(); }

    void deluser(string s) {
        participants.erase(s);
        if (s == groupmaster) {
            if (!participants.empty()) {
                groupmaster = *participants.begin();
                cout << "Group " << gid << " new owner is: " << groupmaster << endl;
            } else {
                groupmaster = "";
                cout << "Group " << gid << " has no members left" << endl;
            }
        }
    }

    void acceptreq(string s) {
        applicants.erase(s);
        participants.insert(s);
    }
};

struct FileMeta {
    long long size = 0;
    string fullhash;
    int num_pieces = 0;
    vector<string> piece_hashes; // per-piece SHA1 hex
    unordered_set<string> peers; // peernames sharing this file
};

unordered_map<string, client*> peers;   // peername -> client*
unordered_map<string, group*> groups;   // gid -> group*
unordered_map<string, unordered_set<string>> group_files; // gid -> filenames
unordered_map<string, FileMeta> files;  // filename -> metadata

bool isgrouppresent(string str) {
    return groups.find(str) != groups.end();
}

bool isuserpresent(string str) {
    return peers.find(str) != peers.end();
}

void managepeer(int peersocket) {
    while (1) {
        // read
        char buff[512000];
        memset(buff, 0, sizeof(buff));
        int bytrd = read(peersocket, buff, sizeof(buff));
        if (bytrd == 0) {
            cout << "Socket received 0 bytes: " << peersocket << endl;
            return;
        }
        cout << "Incoming command from socket " << peersocket << ": " << buff << endl;

        // tokenize
        vector<string> comds;
        char *token = strtok(buff, " ");
        while (token != NULL) {
            comds.push_back(token);
            token = strtok(NULL, " ");
        }

        // handle commands (ensure at least one token)
        if (comds.empty()) {
            string msg = "Invalid command";
            send(peersocket, msg.c_str(), msg.size(), 0);
            continue;
        }

        // create_user
        if (comds[0] == "create_user") {
            if (comds.size() != 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (isuserpresent(comds[1])) {
                string msg = "-----Cannot create user: ID already in use.-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                client* peer = new client(comds[1], comds[2]);
                peers[comds[1]] = peer;
                string msg = "***** ID number " + comds[1] + " registered successfully! ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
                cout << "****** ID " << comds[1] << " has been registered as a new user. ******" << endl;
            }
        }

        // login
        else if (comds[0] == "login") {
            if (comds.size() < 5) {
                string msg = "-----Invalid Arguments for login-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[1])) {
                string msg = "------ User ID " + comds[1] + " is not registered ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (peers[comds[1]]->passcode != comds[2]) {
                string msg = "------ Authentication failed: incorrect passcode for ID " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                peers[comds[1]]->login(comds[3], comds[4]);
                string msg = "Successful Login for User ID " + comds[1] + "! ******\n";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // logout
        else if (comds[0] == "logout") {
            if (comds.size() < 2) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[1])) {
                string msg = "------- No such User ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                peers[comds[1]]->logout();
                string msg = "***** User ID " + comds[1] + " logged out successfully ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // create_group
        else if (comds[0] == "create_group") {
            if (comds.size() < 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[2])) {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (isgrouppresent(comds[1])) {
                string msg = "------- This Group ID is already taken ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                group * initgrp = new group(comds[1], comds[2]);
                groups[comds[1]] = initgrp;
                string msg = "******* Group creation successful. Assigned ID: " + comds[1] + " *******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // join_group
        else if (comds[0] == "join_group") {
            if (comds.size() < 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[2])) {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isgrouppresent(comds[1])) {
                string msg = "------- No such group ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (groups[comds[1]]->partofgroup(comds[2])) {
                string msg = "------- You have already joined this group: " + comds[1] + " -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                groups[comds[1]]->applicants.insert(comds[2]);
                string msg = "******* Request to join group " + comds[1] + " has been sent ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // leave_group
        else if (comds[0] == "leave_group") {
            if (comds.size() < 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[2])) {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isgrouppresent(comds[1])) {
                string msg = "------- No such group ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!groups[comds[1]]->partofgroup(comds[2])) {
                string msg = "------ Access denied. You are not part of Group ID " + comds[1] + " -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                groups[comds[1]]->deluser(comds[2]);
                string msg = "****** Left group successfully. ID: " + comds[1] + " ******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // list_requests
        else if (comds[0] == "list_requests") {
            if (comds.size() < 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[2])) {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isgrouppresent(comds[1])) {
                string msg = "------- No such group ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (groups[comds[1]]->groupmaster != comds[2]) {
                string msg = "------ Access denied. You are not the group owner of ID " + comds[1] + " -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                string msg = "";
                for (const auto &user : groups[comds[1]]->applicants) msg += user + "\n";
                if (msg == "") msg = "------- Group ID " + comds[1] + " has no pending join requests -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // accept_request
        else if (comds[0] == "accept_request") {
            if (comds.size() < 4) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isuserpresent(comds[2])) {
                string msg = "------- No such User ID: " + comds[2] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!isgrouppresent(comds[1])) {
                string msg = "------- No such group ID: " + comds[1] + " ------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (groups[comds[1]]->groupmaster != comds[3]) {
                string msg = "------ Access denied. You are not the group owner of ID " + comds[1] + " -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else if (!groups[comds[1]]->isapplicant(comds[2])) {
                string msg = "------- This user (ID: " + comds[2] + ") has no pending requests -------";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                groups[comds[1]]->acceptreq(comds[2]);
                string msg = "******* Approval granted for User ID: " + comds[2] + " *******";
                send(peersocket, msg.c_str(), msg.size(), 0);
            }
        }

        // list_groups
        else if (comds[0] == "list_groups") {
            string msg = "############### Available groups on the network ###############";
            for (auto it = groups.begin(); it != groups.end(); it++) {
                msg += "\n" + it->first;
            }
            if (msg == "") msg = "-------- Currently, no groups are available. -------";
            send(peersocket, msg.c_str(), msg.size(), 0);
        }

        // upload_file
        else if (comds[0] == "upload_file") {
            // format:
            // upload_file <gid> <filename> <peername> <filesize> <fullhash> <num_pieces> <piecehash1> <piecehash2> ...
            if (comds.size() < 7) {
                string msg = "-----Invalid Arguments for upload_file-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                string gid = comds[1];
                string fname = comds[2];
                string uname = comds[3];
                long long fsize = atoll(comds[4].c_str());
                string fhash = comds[5];
                int num_pieces = stoi(comds[6]);

                if (!isgrouppresent(gid)) {
                    string msg = "------- No such group ID: " + gid + " ------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) {
                    string msg = "------ You are not part of Group ID " + gid + " -------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else {
                    // read piece hashes from comds[7...]
                    FileMeta &fm = files[fname];
                    fm.size = fsize;
                    fm.fullhash = fhash;
                    fm.num_pieces = num_pieces;
                    fm.piece_hashes.clear();
                    for (int i = 0; i < num_pieces; ++i) {
                        if ((int)comds.size() > 7 + i) fm.piece_hashes.push_back(comds[7 + i]);
                        else fm.piece_hashes.push_back(string());
                    }
                    fm.peers.insert(uname);
                    group_files[gid].insert(fname);

                    string msg = "******* File " + fname + " uploaded to group " + gid + " successfully *******";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                    cout << "Tracker: Registered file " << fname << " size " << fsize << " pieces " << num_pieces << endl;
                }
            }
        }

        // list_files
        else if (comds[0] == "list_files") {
            if (comds.size() < 3) {
                string msg = "-----Invalid Arguments-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                string gid = comds[1];
                string uname = comds[2];
                if (!isgrouppresent(gid)) {
                    string msg = "------- No such group ID: " + gid + " ------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) {
                    string msg = "------ Access denied. You are not part of Group ID " + gid + " -------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else {
                    string msg;
                    if (group_files.find(gid) == group_files.end() || group_files[gid].empty()) {
                        msg = "------- No files uploaded in group " + gid + " -------";
                    } else {
                        msg = "######## Files in Group " + gid + " ########\n";
                        for (const auto &fname : group_files[gid]) {
                            FileMeta &fm = files[fname];
                            msg += fname + " SIZE:" + to_string(fm.size) + " PIECES:" + to_string(fm.num_pieces) + " HASH:" + fm.fullhash + "\n";
                        }
                    }
                    send(peersocket, msg.c_str(), msg.size(), 0);
                }
            }
        }

        // download_file (tracker returns metadata + peers)
        else if (comds[0] == "download_file") {
            if (comds.size() < 4) {
                string msg = "-----Invalid Arguments for download_file-----";
                send(peersocket, msg.c_str(), msg.size(), 0);
            } else {
                string gid = comds[1];
                string fname = comds[2];
                string uname = comds[3];

                if (!isgrouppresent(gid)) {
                    string msg = "------- No such group ID: " + gid + " ------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else if (!isuserpresent(uname) || !groups[gid]->partofgroup(uname)) {
                    string msg = "------ Access denied. You are not part of Group ID " + gid + " -------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else if (group_files[gid].find(fname) == group_files[gid].end()) {
                    string msg = "------- No such file in group " + gid + " -------";
                    send(peersocket, msg.c_str(), msg.size(), 0);
                } else {
                    FileMeta &fm = files[fname];
                    // header
                string msg = "FILE " + fname + " SIZE " + to_string(fm.size) + 
                     " HASH " + fm.fullhash + " PIECES " + to_string(fm.num_pieces) + " PIECE_HASHES";
                for (auto &h : fm.piece_hashes) msg += " " + h;
                msg += "\nPEERS\n";
                for (const string &peer : fm.peers) {
                if (isuserpresent(peer) && peers[peer]->connected) {
                    msg += peer + " " + peers[peer]->hostip + " " + peers[peer]->hostport + "\n";
                }
                }
                msg += "\n"; // <- ensure trailing newline
                send(peersocket, msg.c_str(), msg.size(), 0);

                }
            }
        }

        else {
            string msg = "Unrecognized command";
            send(peersocket, msg.c_str(), msg.size(), 0);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cout << "-----Invalid Arguments-----" << endl;
        return 0;
    }

    fstream filconfig;
    filconfig.open(argv[1]);
    string serverip, serverport;
    filconfig >> serverip >> serverport;

    int serversock;
    struct sockaddr_in serveradd;

    serversock = socket(AF_INET, SOCK_STREAM, 0);
    if (serversock == 0) {
        cout << "------- Error: Could not create socket -------" << endl;
        return 0;
    }

    int choice = 1;
    if (setsockopt(serversock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &choice, sizeof(choice)) != 0) {
        cout << "------- Unable to configure socket options ------" << endl;
        return 0;
    }

    serveradd.sin_family = AF_INET;
    serveradd.sin_addr.s_addr = INADDR_ANY;
    int port = stoi(serverport);
    serveradd.sin_port = htons(port);

    if (bind(serversock, (struct sockaddr *)&serveradd, sizeof(serveradd)) < 0) {
        cout << "------ Unable to bind socket ------" << endl;
        return 0;
    }

    if (listen(serversock, 20) < 0) {
        cout << "------- Unable to start listening on socket ---------" << endl;
        return 0;
    }

    cout << "\n=========================================\n";
    cout << "          TRACKER SERVER STARTED         \n";
    cout << "=========================================\n";
    cout << "Listening on IP: " << serverip << "  Port: " << serverport << endl;
    cout << "Tracker is now running...\n";
    cout << "-----------------------------------------\n";
    cout << "Available Tracker Commands (from console):\n";
    cout << "   quit   -> Stop the tracker server\n";
    cout << "-----------------------------------------\n\n";

    int incomsock;
    vector<thread> peerss;
    int length = sizeof(serveradd);

    thread exit_thread([]() {
        string inp;
        while (true) {
            getline(cin, inp);
            if (inp == "quit") exit(0);
        }
    });
    exit_thread.detach();

    while (1) {
        if ((incomsock = accept(serversock, (struct sockaddr *)&serveradd, (socklen_t *)&length)) < 0) {
            cout << "------- Unable to accept incoming connection -------" << endl;
            continue;
        }
        cout << "******* Client accepted at socket: " << incomsock << " ******" << endl;
        peerss.push_back(thread(managepeer, incomsock));
    }

    for (int i = 0; i < peerss.size(); i++) peerss[i].join();
    return 0;
}
