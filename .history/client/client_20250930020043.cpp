// ================= client.cpp =================
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <thread>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <functional>
#include <openssl/evp.h>
#include <atomic>

using namespace std;

static const size_t PIECE_SIZE = 512 * 1024; // 512KB

// globals
string peername;
bool connected;
int serversock;
bool noaccept = false;
int listenSock;
unordered_map<string,string> uploaded_files; // fname -> fullpath

// Download tracking
struct DownloadInfo {
    string group_id;
    string filename;
    string dest_path;
    long long total_size;
    int total_pieces;
    int completed_pieces;
    vector<int> piece_status; // 0=pending, 1=downloading, 2=completed, 3=failed
    vector<string> piece_hashes;
    string full_hash;
    bool is_active;
    time_t start_time;
};

unordered_map<string, DownloadInfo> active_downloads; // filename -> download info
mutex downloads_mtx;

// forward declaration
string filehash(const string &filepath);
vector<string> compute_piece_hashes(const string &filepath, int &num_pieces);

void displaycomds() {
    cout << "\n==================== Available Commands ====================\n";
    cout << "create_user <username> <password>\n";
    cout << "login <username> <password>\n";
    cout << "logout\n";
    cout << "create_group <groupid>\n";
    cout << "join_group <groupid>\n";
    cout << "leave_group <groupid>\n";
    cout << "list_requests <groupid>\n";
    cout << "accept_request <groupid> <username>\n";
    cout << "list_groups\n";
    cout << "list_files <groupid>\n";
    cout << "upload_file <groupid> <filepath>\n";
    cout << "download_file <groupid> <filename> <dest_path>\n";
    cout << "show_downloads\n";
    cout << "commands\n";
    cout << "exit\n";
    cout << "============================================================\n\n";
}

void login_local(string str) { peername = str; connected = true; }
void logout_local() { peername = ""; connected = false; }
void logincheck(function<void()> action) {
    if (!connected) { cout << "------- You must log in first. ---------" << endl; return; }
    action();
}

// worker pool
queue<int> quetask;
mutex mtx;
condition_variable cv;
bool poolstop = false;

// serve peer requests
void handling_peer_req(int peersock) {
    char buff[4096];
    memset(buff, 0, sizeof(buff));
    int bytrd = read(peersock, buff, sizeof(buff) - 1);
    if (bytrd <= 0) { close(peersock); return; }

    vector<string> comds;
    char *token = strtok(buff, " ");
    while (token) { comds.push_back(token); token = strtok(NULL, " "); }
    if (comds.empty()) { close(peersock); return; }

    if (comds[0] == string("GET_PIECE")) {
        if (comds.size() < 3) { close(peersock); return; }
        string fname = comds[1];
        int index = stoi(comds[2]);
        if (index < 0) { close(peersock); return; }
        if (uploaded_files.find(fname) == uploaded_files.end()) { close(peersock); return; }
        string fullpath = uploaded_files[fname];

        ifstream fin(fullpath, ios::binary);
        if (!fin) { close(peersock); return; }
        fin.seekg((long long)index * PIECE_SIZE);
        vector<char> buf(PIECE_SIZE);
        fin.read(buf.data(), PIECE_SIZE);
        streamsize n = fin.gcount();
        fin.close();
        
        if (n > 0) {
            // Send piece size first, then piece data
            uint32_t piece_size = htonl(n);
            size_t total_sent = 0;
            while (total_sent < sizeof(piece_size)) {
                ssize_t sent = send(peersock, (char*)&piece_size + total_sent, sizeof(piece_size) - total_sent, 0);
                if (sent <= 0) break;
                total_sent += sent;
            }
            
            // Send piece data
            total_sent = 0;
            while (total_sent < n) {
                ssize_t sent = send(peersock, buf.data() + total_sent, n - total_sent, 0);
                if (sent <= 0) break;
                total_sent += sent;
            }
        }
    }
    close(peersock);
}

void worker_thread() {
    while (true) {
        int s;
        {
            unique_lock<mutex> lock(mtx);
            cv.wait(lock, [] { return !quetask.empty() || poolstop; });
            if (poolstop && quetask.empty()) return;
            s = quetask.front();
            quetask.pop();
        }
        handling_peer_req(s);
    }
}

void handling_peer_conn(string ip, string port) {
    int sock;
    int choice = 1;
    struct sockaddr_in addr;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        cout << "------- Failed to create socket -------" << endl;
        return;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &choice, sizeof(choice))) {
        cout << "------- Failed to set socket options -------" << endl;
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(stoi(port));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("Bind Error"); exit(EXIT_FAILURE); }
    if (listen(sock, 20) < 0) { perror("listen"); exit(EXIT_FAILURE); }

    listenSock = sock;
    int len = sizeof(addr), newsck;
    while (!noaccept) {
        newsck = accept(sock, (struct sockaddr *)&addr, (socklen_t *)&len);
        if (newsck < 0) {
            if (noaccept) break;
            cout << "-------- Failed to establish client connection --------" << endl;
            continue;
        }
        {
            lock_guard<mutex> lock(mtx);
            quetask.push(newsck);
        }
        cv.notify_one();
    }
    close(sock);
}

string sendcomd(int sock, const string &cmd) {
    ssize_t sent = send(sock, cmd.c_str(), cmd.size(), 0);
    if (sent < 0) { perror("send failed"); return ""; }
    char buff[2097152]; // 2MB buffer (increase as needed)
    memset(buff, 0, sizeof(buff));
    ssize_t recvd = read(sock, buff, sizeof(buff) - 1);
    if (recvd < 0) { perror("read failed"); return ""; }
    return string(buff, recvd);
}

string filehash(const string &filepath) {
    ifstream f(filepath, ios::binary);
    if (!f) return "";
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    char buf[524288];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        EVP_DigestUpdate(ctx, buf, f.gcount());
    }
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int len;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    char hex[41];
    for (unsigned int i = 0; i < len; ++i) sprintf(hex + i*2, "%02x", hash[i]);
    hex[40] = 0;
    return string(hex);
}

vector<string> compute_piece_hashes(const string &filepath, int &num_pieces) {
    vector<string> hashes;
    ifstream fin(filepath, ios::binary | ios::ate);
    long long filesize = fin.tellg();
    fin.seekg(0, ios::beg);
    num_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE;
    for (int i = 0; i < num_pieces; ++i) {
        vector<char> buf(PIECE_SIZE);
        fin.read(buf.data(), PIECE_SIZE);
        streamsize n = fin.gcount();
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
        EVP_DigestUpdate(ctx, buf.data(), n);
        unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int hlen;
        EVP_DigestFinal_ex(ctx, hash, &hlen);
        EVP_MD_CTX_free(ctx);
        char hex[41]; for (unsigned int j=0;j<hlen;j++) sprintf(hex+j*2,"%02x",hash[j]); hex[40]=0;
        hashes.push_back(string(hex));
    }
    fin.close();
    return hashes;
}

// ========== main() ==========
// (continues with command handlers: create_user, login, upload_file, download_file, exit, etc.)
// Keep the structure from the code I gave earlier.


int main(int argc, char *argv[]) {
    if (argc != 3) { cout << "----- Invalid Arguments ------" << endl; return 0; }
    logout_local();

    // parse hostip:hostport from argv[1]
    string hostip, hostport;
    int idx = 0;
    for (; argv[1][idx] != ':'; ++idx) hostip.push_back(argv[1][idx]);
    idx++;
    while (argv[1][idx] != '\0') { hostport.push_back(argv[1][idx]); idx++; }

    string serverip, serverport;
    fstream file;
    file.open(argv[2]);
    file >> serverip >> serverport;

    // connect to tracker
    if ((serversock = socket(AF_INET, SOCK_STREAM, 0)) < 0) { cout << "-------- Unable to create socket -------" << endl; return 0; }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    int port = stoi(serverport);
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, serverip.c_str(), &server_addr.sin_addr) <= 0) { cout << "------- Error: Unable to parse address -------" << endl; return 0; }
    if (connect(serversock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) { cout << "-------- Failed to establish socket connection --------" << endl; return 0; }

    // thread pool to serve peers
    vector<thread> workers;
    int threadsno = 4;
    for (int i = 0; i < threadsno; ++i) workers.emplace_back(worker_thread);

    thread help_object(handling_peer_conn, hostip, hostport);
    displaycomds();

    while (1) {
        cout << ">>> ";
        string comd;
        getline(cin, comd);
        vector<string> cmds;
        string token;
        stringstream ss(comd);
        while (ss >> token) cmds.push_back(token);
        int length = cmds.size();
        if (length == 0) { cout << "-------- Unrecognized command. Enter a valid command. --------" << endl; continue; }
        

        unordered_map<string, function<void()>> cmdMap;

        cmdMap["create_user"] = [&]() {
            if (length != 3) { cout << "Usage: create_user <user> <pass>\n"; return; }
            string msg = "create_user " + cmds[1] + " " + cmds[2];
            cout << sendcomd(serversock, msg) << endl;
        };

        cmdMap["login"] = [&]() {
            if (length != 3) { cout << "Usage: login <user> <pass>\n"; return; }
            if (connected) { cout << "-------- User session already active --------" << endl; return; }
            string msg = "login " + cmds[1] + " " + cmds[2] + " " + hostip + " " + hostport;
            string r = sendcomd(serversock, msg);
            if (!r.empty() && r[0] == 'S') { logout_local(); login_local(cmds[1]); cout << "********* You are now logged in *********" << endl; }
            else cout << r << endl;
        };

        cmdMap["logout"] = [&]() {
            logincheck([&]() {
                string r = sendcomd(serversock, "logout " + peername);
                cout << r << endl; logout_local();
            });
        };

        cmdMap["create_group"] = [&]() {
            logincheck([&]() {
                if (length != 2) { cout << "Usage: create_group <groupid>\n"; return; }
                cout << sendcomd(serversock, "create_group " + cmds[1] + " " + peername) << endl;
            });
        };

        cmdMap["join_group"] = [&]() {
            logincheck([&]() {
                if (length != 2) { cout << "Usage: join_group <groupid>\n"; return; }
                cout << sendcomd(serversock, "join_group " + cmds[1] + " " + peername) << endl;
            });
        };

        cmdMap["leave_group"] = [&]() {
            logincheck([&]() {
                if (length != 2) { cout << "Usage: leave_group <groupid>\n"; return; }
                cout << sendcomd(serversock, "leave_group " + cmds[1] + " " + peername) << endl;
            });
        };

        cmdMap["list_requests"] = [&]() {
            logincheck([&]() {
                if (length != 2) { cout << "Usage: list_requests <groupid>\n"; return; }
                cout << sendcomd(serversock, "list_requests " + cmds[1] + " " + peername) << endl;
            });
        };

        cmdMap["accept_request"] = [&]() {
            logincheck([&]() {
                if (length != 3) { cout << "Usage: accept_request <groupid> <user>\n"; return; }
                cout << sendcomd(serversock, "accept_request " + cmds[1] + " " + cmds[2] + " " + peername) << endl;
            });
        };

        cmdMap["list_groups"] = [&]() {
            logincheck([&]() { cout << sendcomd(serversock, "list_groups") << endl; });
        };

        cmdMap["list_files"] = [&]() {
            logincheck([&]() {
                if (length != 2) { cout << "Usage: list_files <groupid>\n"; return; }
                cout << sendcomd(serversock, "list_files " + cmds[1] + " " + peername) << endl;
            });
        };

        // upload_file: allow spaces in filepath by recombining tokens
        cmdMap["upload_file"] = [&]() {
            logincheck([&]() {
                if (length < 3) { cout << "Usage: upload_file <groupid> <filepath>\n"; return; }
                string gid = cmds[1];
                string fpath;
                for (int i = 2; i < length; ++i) {
                    if (i > 2) fpath += " ";
                    fpath += cmds[i];
                }
                ifstream fin(fpath, ios::binary | ios::ate);
                if (!fin) { cout << "File not found: " << fpath << endl; return; }
                long long fsize = fin.tellg(); fin.close();
                // compute piece hashes and full hash
                int num_pieces = 0;
                vector<string> piece_hashes = compute_piece_hashes(fpath, num_pieces);
                string fullhash = filehash(fpath);
                // build command: upload_file <gid> <fname> <peername> <filesize> <fullhash> <num_pieces> <h1> <h2> ...
                size_t pos = fpath.find_last_of("/");
                string fname = (pos == string::npos) ? fpath : fpath.substr(pos + 1);
                // store file locally so peer server can serve pieces
                uploaded_files[fname] = fpath;   // >>> FIX <<<
                cout << "[Peer] Registered file for sharing: " << fname 
                    << " (" << fpath << ")\n";  // >>> DEBUG <<<

                string cmd = "upload_file " + gid + " " + fname + " " + peername + " " + to_string(fsize) + " " + fullhash + " " + to_string(num_pieces);
                for (auto &h : piece_hashes) cmd += " " + h;
                string r = sendcomd(serversock, cmd);
                cout << r << endl;
            });
        };

        // Enhanced download_file with progress tracking and better error handling
        cmdMap["download_file"] = [&]() {
            logincheck([&]() {
                if (length < 4) { cout << "Usage: download_file <groupid> <filename> <dest_path>\n"; return; }

                string gid = cmds[1], fname = cmds[2], destpath = cmds[3];

                // Check if already downloading this file
                {
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end() && active_downloads[fname].is_active) {
                        cout << "File " << fname << " is already being downloaded.\n";
                        return;
                    }
                }

                string r = sendcomd(serversock, "download_file " + gid + " " + fname + " " + peername);
                if (r.rfind("FILE ", 0) != 0) { cout << r << endl; return; }

                // Parse file metadata
                stringstream s(r);
                string token;
                long long size = 0;
                string fullhash;
                int num_pieces = 0;
                vector<string> piece_hashes;
                string word;
                while (s >> word) {
                    if (word == "SIZE") s >> size;
                    else if (word == "HASH") s >> fullhash;
                    else if (word == "PIECES") s >> num_pieces;
                    else if (word == "PIECE_HASHES") break;
                }
                piece_hashes.resize(num_pieces);
                for (int i = 0; i < num_pieces; ++i) s >> piece_hashes[i];

                // Parse available peers
                size_t pos = r.find("\nPEERS\n");
                vector<tuple<string,string,string>> peerlist;
                if (pos != string::npos) {
                    string peers_block = r.substr(pos + 7);
                    stringstream sp(peers_block);
                    string pname, pip, pport;
                    while (sp >> pname >> pip >> pport) {
                        peerlist.emplace_back(pname, pip, pport);
                    }
                }

                if (peerlist.empty()) { cout << "No active peers available for " << fname << ".\n"; return; }

                // Prepare output file
                if (destpath.back() != '/') destpath += "/";
                string fullout = destpath + fname;
                FILE *outf = fopen(fullout.c_str(), "wb+");
                if (!outf) { cout << "Failed to create output file: " << fullout << endl; return; }
                if (size > 0) { fseek(outf, size - 1, SEEK_SET); fputc(0, outf); fflush(outf); }
                fclose(outf);

                // Initialize download tracking
                DownloadInfo download_info;
                download_info.group_id = gid;
                download_info.filename = fname;
                download_info.dest_path = fullout;
                download_info.total_size = size;
                download_info.total_pieces = num_pieces;
                download_info.completed_pieces = 0;
                download_info.piece_status.resize(num_pieces, 0);
                download_info.piece_hashes = piece_hashes;
                download_info.full_hash = fullhash;
                download_info.is_active = true;
                download_info.start_time = time(nullptr);

                {
                    lock_guard<mutex> lock(downloads_mtx);
                    active_downloads[fname] = download_info;
                }

                cout << "Starting download of " << fname << " (" << size << " bytes, " << num_pieces << " pieces) from " << peerlist.size() << " peers.\n";

                // Thread-safe piece set and status tracking
                vector<int> all_pieces(num_pieces);
                for (int i = 0; i < num_pieces; ++i) all_pieces[i] = i;
                mutex piece_mtx;
                atomic<int> completed_count(0);
                const int MAX_PEER_RETRIES = peerlist.size();

                // Random piece selection
                auto get_next_piece = [&]() -> int {
                    lock_guard<mutex> lock(piece_mtx);
                    vector<int> pending;
                    for (int i = 0; i < num_pieces; ++i) {
                        lock_guard<mutex> dl_lock(downloads_mtx);
                        if (active_downloads.find(fname) != active_downloads.end() && active_downloads[fname].piece_status[i] == 0)
                            pending.push_back(i);
                    }
                    if (pending.empty()) return -1;
                    int idx = rand() % pending.size();
                    int piece = pending[idx];
                    // Mark as downloading
                    active_downloads[fname].piece_status[piece] = 1;
                    return piece;
                };

                auto worker = [&](int worker_id) {
                    while (true) {
                        int piece_idx = get_next_piece();
                        if (piece_idx == -1) return;

                        bool success = false;
                        vector<int> peer_order(peerlist.size());
                        for (int i = 0; i < (int)peerlist.size(); ++i) peer_order[i] = i;
                        random_shuffle(peer_order.begin(), peer_order.end());

                        for (int attempt = 0; attempt < (int)peer_order.size(); ++attempt) {
                            int peer_idx = peer_order[attempt];
                            string pip, pport, pname;
                            tie(pname, pip, pport) = peerlist[peer_idx];

                            int psock = socket(AF_INET, SOCK_STREAM, 0);
                            if (psock < 0) continue;

                            struct sockaddr_in addr;
                            addr.sin_family = AF_INET;
                            addr.sin_port = htons(stoi(pport));
                            if (inet_pton(AF_INET, pip.c_str(), &addr.sin_addr) <= 0) {
                                close(psock);
                                continue;
                            }

                            struct timeval tv = {10, 0};
                            setsockopt(psock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
                            setsockopt(psock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

                            if (connect(psock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                                close(psock);
                                continue;
                            }

                            string preq = "GET_PIECE " + fname + " " + to_string(piece_idx);
                            if (send(psock, preq.c_str(), preq.size(), 0) < 0) {
                                close(psock);
                                continue;
                            }

                            uint32_t piece_size;
                            ssize_t total_rd = 0;
                            while (total_rd < (ssize_t)sizeof(piece_size)) {
                                ssize_t rd = read(psock, (char*)&piece_size + total_rd, sizeof(piece_size) - total_rd);
                                if (rd <= 0) { close(psock); goto next_peer; }
                                total_rd += rd;
                            }
                            piece_size = ntohl(piece_size);

                            vector<char> buffer(piece_size);
                            total_rd = 0;
                            while (total_rd < piece_size) {
                                ssize_t rd = read(psock, buffer.data() + total_rd, piece_size - total_rd);
                                if (rd <= 0) { close(psock); goto next_peer; }
                                total_rd += rd;
                            }
                            close(psock);

                            if (total_rd <= 0) goto next_peer;

                            // SHA1 verification
                            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                            EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
                            EVP_DigestUpdate(ctx, buffer.data(), total_rd);
                            unsigned char hash[EVP_MAX_MD_SIZE];
                            unsigned int hlen;
                            EVP_DigestFinal_ex(ctx, hash, &hlen);
                            EVP_MD_CTX_free(ctx);

                            char hex[41];
                            for (unsigned int i=0;i<hlen;i++) sprintf(hex+i*2,"%02x",hash[i]);
                            hex[40]=0;
                            string recv_hex(hex);

                            if (recv_hex != piece_hashes[piece_idx]) {
                                cout << "[Piece " << piece_idx << "] Hash mismatch! Expected: " << piece_hashes[piece_idx] << ", Got: " << recv_hex << endl;
                                goto next_peer;
                            }

                            // Write piece to file
                            FILE *fw = fopen(fullout.c_str(), "rb+");
                            if (!fw) goto next_peer;
                            fseek(fw, (long long)piece_idx*PIECE_SIZE, SEEK_SET);
                            fwrite(buffer.data(), 1, total_rd, fw);
                            fflush(fw);
                            fclose(fw);

                            {
                                lock_guard<mutex> dl_lock(downloads_mtx);
                                if (active_downloads.find(fname) != active_downloads.end()) {
                                    active_downloads[fname].piece_status[piece_idx] = 2;
                                    active_downloads[fname].completed_pieces++;
                                }
                            }
                            completed_count++;
                            cout << "[Piece " << piece_idx << "] Downloaded successfully from " << pip << ":" << pport << " (" << completed_count << "/" << num_pieces << ")\n";
                            success = true;
                            break;
                        next_peer:;
                        }
                        if (!success) {
                            cout << "[Piece " << piece_idx << "] Failed from all peers.\n";
                            lock_guard<mutex> dl_lock(downloads_mtx);
                            if (active_downloads.find(fname) != active_downloads.end()) {
                                active_downloads[fname].piece_status[piece_idx] = 3;
                            }
                        }
                    }
                };

                int num_workers = min((int)peerlist.size(), 6);
                vector<thread> dthreads;
                for (int i=0;i<num_workers;i++) dthreads.emplace_back(worker,i);
                for (auto &t:dthreads) if (t.joinable()) t.join();

                // Final verification
                bool all_completed = true;
                {
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end()) {
                        for (int i=0;i<num_pieces;i++) {
                            if (active_downloads[fname].piece_status[i] != 2) {
                                all_completed = false;
                                break;
                            }
                        }
                    }
                }

                if (!all_completed) {
                    cout << "Download incomplete: some pieces failed.\n";
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end()) {
                        active_downloads[fname].is_active = false;
                    }
                    return;
                }

                // Full file hash verification
                string dhash = filehash(fullout);
                if (dhash == fullhash) {
                    cout << "[C] " << gid << " " << fname << " downloaded successfully.\n";
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end()) {
                        active_downloads[fname].is_active = false;
                    }
                } else {
                    cout << "Full-file hash mismatch! Expected " << fullhash << " got " << dhash << endl;
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end()) {
                        active_downloads[fname].is_active = false;
                    }
                }
            });
        };

        // show_downloads command
        cmdMap["show_downloads"] = [&]() {
            logincheck([&]() {
                lock_guard<mutex> lock(downloads_mtx);
                if (active_downloads.empty()) {
                    cout << "No active downloads.\n";
                    return;
                }
                
                cout << "========== Active Downloads ==========\n";
                for (auto &pair : active_downloads) {
                    const DownloadInfo &info = pair.second;
                    if (!info.is_active) continue;
                    
                    time_t now = time(nullptr);
                    int elapsed = now - info.start_time;
                    int progress = (info.completed_pieces * 100) / info.total_pieces;
                    
                    cout << "File: " << info.filename << "\n";
                    cout << "  Group: " << info.group_id << "\n";
                    cout << "  Size: " << info.total_size << " bytes\n";
                    cout << "  Progress: " << info.completed_pieces << "/" << info.total_pieces << " pieces (" << progress << "%)\n";
                    cout << "  Elapsed: " << elapsed << " seconds\n";
                    cout << "  Status: ";
                    
                    int pending = 0, downloading = 0, completed = 0, failed = 0;
                    for (int status : info.piece_status) {
                        if (status == 0) pending++;
                        else if (status == 1) downloading++;
                        else if (status == 2) completed++;
                        else if (status == 3) failed++;
                    }
                    
                    cout << pending << " pending, " << downloading << " downloading, " 
                         << completed << " completed, " << failed << " failed\n";
                    cout << "----------------------------------------\n";
                }
            });
        };

        // handle exit
        if (cmds[0] == "exit") {
            cout << "------- Exiting Client ---------" << endl;
            if (serversock > 0) close(serversock);
            noaccept = true;
            shutdown(listenSock, SHUT_RDWR);
            close(listenSock);
            if (help_object.joinable()) help_object.join();
            {
                lock_guard<mutex> lock(mtx);
                poolstop = true;
            }
            cv.notify_all();
            for (auto &t : workers) t.join();
            return 0;
        } else if (cmds[0] == "commands") displaycomds();
        else if (cmdMap.find(cmds[0]) != cmdMap.end()) cmdMap[cmds[0]]();
        else cout << "------- Invalid Command --------" << endl;
    }

    // cleanup (never reached)
    {
        lock_guard<mutex> lock(mtx);
        poolstop = true;
    }
    cv.notify_all();
    for (auto &t : workers) t.join();
    return 0;
}
