#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <thread>
#include <fcntl.h>
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
#include <algorithm>
#include <openssl/evp.h>
#include <atomic>

using namespace std;

static const size_t PIECE_SIZE = 512 * 1024; // 512KB piece size

// globals
string peername; // name of peer
bool connected; // is connected
int serversock; // server socket
bool noaccept = false; // flag for accept
int listenSock; // listen socket
unordered_map<string,string> uploaded_files; // fname to fullpath

// Download tracking
struct DownloadInfo 
{
    string group_id; // group id
    string filename; // file name
    string dest_path; // destination path
    long long total_size; // total size
    long long total_pieces; // total pieces
    long long completed_pieces; // completed pieces
    vector<int> piece_status; // 0=pending, 1=downloading, 2=completed, 3=failed
    vector<string> piece_hashes; // piece hashes
    string full_hash; // full file hash
    bool is_active; // is download active
};

unordered_map<string, DownloadInfo> active_downloads; // filename to download info
mutex downloads_mtx; // mutex for downloads

string filehash(const string &filepath); // function for file hash
vector<string> compute_piece_hashes(const string &filepath, long long &num_pieces); // function for piece hashes

void displaycomds() 
{
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
    cout << "stop_share <groupid> <filename>\n";
    cout << "show_downloads\n";
    cout << "commands\n";
    cout << "exit\n";
    cout << "============================================================\n\n";
}

// set peername and connected
void login_local(string str) 
{ 
    peername = str; connected = true; 
} 

// reset peername and connected
void logout_local() 
{ 
    peername = ""; connected = false; 
} 

void logincheck(function<void()> action) 
{
    // check login
    if (!connected) 
    { 
        cout << "------- You must log in first. ---------" << endl; 
        return; 
    } 
    action();
}

// worker pool
queue<int> quetask; // queue for tasks
mutex mtx; // mutex for queue
condition_variable cv; // condition variable
bool poolstop = false; // stop flag

// serve peer requests
void handling_peer_req(int peersock) 
{
    char buff[4096]; // buffer
    memset(buff, 0, sizeof(buff)); // clear buffer
    int bytrd = read(peersock, buff, sizeof(buff) - 1); // read from socket
    
    // if nothing read, close
    if (bytrd <= 0) 
    { 
        close(peersock); 
        return; 
    } 

    vector<string> comds;
    char *token = strtok(buff, " "); // splitting by space
    
    while (token) 
    { 
        comds.push_back(token); 
        token = strtok(NULL, " "); 
    } 
    
    if (comds.empty()) 
    { 
        close(peersock); 
        return; 
    } 

    // if get piece
    if (comds[0] == string("GET_PIECE")) 
    { 
        if (comds.size() < 3) 
        { 
            close(peersock); 
            return; 
        }

        string fname = comds[1]; // file name
        int index = stoi(comds[2]); // piece index

        if (index < 0) 
        { 
            close(peersock); 
            return; 
        }
        if (uploaded_files.find(fname) == uploaded_files.end()) 
        { 
            close(peersock); 
            return; 
        }
        string fullpath = uploaded_files[fname]; // get path

        int fd = open(fullpath.c_str(), O_RDONLY); // open file
        if (fd < 0) 
        { 
            close(peersock); 
            return; 
        }

        off_t offset = (off_t)index * PIECE_SIZE; // offset for piece
        vector<char> buf(PIECE_SIZE);
        ssize_t n = pread(fd, buf.data(), PIECE_SIZE, offset);
        close(fd);
        
        if (n > 0) 
        {
            // sending piece size first, then piece data
            uint32_t piece_size = htonl(n); // piece size
            size_t total_sent = 0; // sent bytes
            while (total_sent < sizeof(piece_size)) 
            {
                ssize_t sent = send(peersock, (char*)&piece_size + total_sent, sizeof(piece_size) - total_sent, 0); // send size
                if (sent <= 0) break; // checking sent
                total_sent += sent; // adding sent
            }
            
            // sending piece data
            total_sent = 0; // resetting sent
            while (total_sent < n) 
            {
                ssize_t sent = send(peersock, buf.data() + total_sent, n - total_sent, 0); // send data
                if (sent <= 0) break; // checking sent
                total_sent += sent; // adding sent
            }
        }
    }
    close(peersock);
}

void worker_thread() 
{
    while (true) 
    {
        int s;
        {
            unique_lock<mutex> lock(mtx); // lock
            cv.wait(lock, [] { return !quetask.empty() || poolstop; }); // wait for task or stop
            // if stop and no task, return
            if (poolstop && quetask.empty()) 
            {
                return; 
            }
            s = quetask.front(); // getting task
            quetask.pop();
        }
        handling_peer_req(s);
    }
}

void handling_peer_conn(string ip, string port) 
{
    int sock; // socket
    int choice = 1; // option
    struct sockaddr_in addr; // address
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) 
    {
        cout << "------- Failed to create socket -------" << endl; 
        return;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &choice, sizeof(choice))) 
    {
        cout << "------- Failed to set socket options -------" << endl; 
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // any address
    addr.sin_port = htons(stoi(port)); // port

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    { 
        perror("Bind Error"); 
        exit(EXIT_FAILURE); 
    }

    if (listen(sock, 20) < 0) 
    { 
        perror("listen"); 
        exit(EXIT_FAILURE); 
    }

    listenSock = sock; // setting listening socket
    int len = sizeof(addr), newsck;
    
    while (!noaccept) 
    {
        newsck = accept(sock, (struct sockaddr *)&addr, (socklen_t *)&len);
        if (newsck < 0) 
        {
            if (noaccept) break;
            cout << "-------- Failed to establish client connection --------" << endl;
            continue;
        }
        {
            lock_guard<mutex> lock(mtx);
            quetask.push(newsck); // pushing task
        }
        cv.notify_one();
    }
    close(sock);
}

string sendcomd(int sock, const string &cmd) 
{
    ssize_t sent = send(sock, cmd.c_str(), cmd.size(), 0);
    if (sent < 0) 
    { 
        perror("send failed"); 
        return ""; 
    } 
    char buff[2097152];
    memset(buff, 0, sizeof(buff));
    ssize_t recvd = read(sock, buff, sizeof(buff) - 1);
    if (recvd < 0) 
    { 
        perror("read failed"); 
        return ""; 
    } 
    return string(buff, recvd); 
}

string filehash(const string &filepath) 
{
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) return "";
    
    EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // hash context
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL); // init hash
    char buf[524288]; // buffer
    ssize_t n;
    
    while ((n = read(fd, buf, sizeof(buf))) > 0) 
    {
        EVP_DigestUpdate(ctx, buf, n); // updating hash
    }
    close(fd);
    
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int len;
    EVP_DigestFinal_ex(ctx, hash, &len); // finishes hashing
    EVP_MD_CTX_free(ctx); // freeingg
    
    char hex[41];
    for (unsigned int i = 0; i < len; ++i) 
    {
        sprintf(hex + i*2, "%02x", hash[i]); // to hex
    }
    
    hex[40] = 0;
    return string(hex);
}

vector<string> compute_piece_hashes(const string &filepath, long long &num_pieces) 
{
    vector<string> hashes; // hashes vector
    int fd = open(filepath.c_str(), O_RDONLY); 
    if (fd < 0) 
    { 
        num_pieces = 0; 
        return hashes; 
    }
    
    off_t filesize = lseek(fd, 0, SEEK_END); // get size of file
    lseek(fd, 0, SEEK_SET); // resetting
    num_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE; // calc pieces
    
    for (long long i = 0; i < num_pieces; ++i) 
    {
        vector<char> buf(PIECE_SIZE); 
        ssize_t n = read(fd, buf.data(), PIECE_SIZE); // read piece
        if (n <= 0) break; 
        EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // hash context
        EVP_DigestInit_ex(ctx, EVP_sha1(), NULL); // init hash
        EVP_DigestUpdate(ctx, buf.data(), n); // update hash
        unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int hlen;
        EVP_DigestFinal_ex(ctx, hash, &hlen); // finish hash
        EVP_MD_CTX_free(ctx); // free context
        char hex[41]; for (unsigned int j=0;j<hlen;j++) sprintf(hex+j*2,"%02x",hash[j]); hex[40]=0;
        hashes.push_back(string(hex)); // add hash
    }
    close(fd);
    return hashes;
}

int main(int argc, char *argv[]) 
{
    if (argc != 3) 
    { 
        cout << "----- Invalid Arguments ------" << endl; 
        return 0; 
    } 
    logout_local(); // logout

    string hostip, hostport;
    int idx = 0;
    for (; argv[1][idx] != ':'; ++idx)
    {
        hostip.push_back(argv[1][idx]); // get ip
    }
    idx++;
    while (argv[1][idx] != '\0')
    {
        hostport.push_back(argv[1][idx]);
        idx++;
    }

    string serverip, serverport;
    FILE *file = fopen(argv[2], "r");
    if (!file) 
    { 
        cout << "Failed to open tracker info file" << endl; 
        return 0; 
    }
    char ipbuf[128], portbuf[32];
    
    if (fscanf(file, "%127s %31s", ipbuf, portbuf) != 2) 
    { 
        cout << "Failed to read tracker info" << endl; fclose(file); 
        return 0; 
    }

    fclose(file); 
    serverip = ipbuf; 
    serverport = portbuf; 

    // connect to tracker
    if ((serversock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    { 
        cout << "-------- Unable to create socket -------" << endl; 
        return 0; 
    }

    struct sockaddr_in server_addr; 
    server_addr.sin_family = AF_INET; 
    int port = stoi(serverport); 
    server_addr.sin_port = htons(port); 
    
    if (inet_pton(AF_INET, serverip.c_str(), &server_addr.sin_addr) <= 0) 
    { 
        cout << "------- Error: Unable to parse address -------" << endl; 
        return 0; 
    }
    
    if (connect(serversock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
    { 
        cout << "-------- Failed to establish socket connection --------" << endl; 
        return 0; 
    }

    // thread pool to serve peers
    vector<thread> workers; // workers
    int threadsno = 4; // number of threads
    for (int i = 0; i < threadsno; ++i) 
    {
        workers.emplace_back(worker_thread); // start threads
    }
    thread help_object(handling_peer_conn, hostip, hostport); // thread for peer conn
    displaycomds(); // show commands

    while (1) 
    {
        cout << ">>> "; 
        string comd; 
        getline(cin, comd); 
        vector<string> cmds; 
        string token; 
        stringstream ss(comd); 
        while (ss >> token)
        {
            cmds.push_back(token); 
        } 
        int length = cmds.size(); 
        if (length == 0) 
        { 
            cout << "-------- Unrecognized command. Enter a valid command. --------" << endl; 
            continue; 
        }
            
        unordered_map<string, function<void()>> cmdMap;
        // stop_share command
        cmdMap["stop_share"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 3) 
                { 
                    cout << "Usage: stop_share <groupid> <filename>\n"; 
                    return; 
                }
                string gid = cmds[1], fname = cmds[2];
                // Remove from local uploaded_files
                if (uploaded_files.find(fname) != uploaded_files.end()) 
                {
                    uploaded_files.erase(fname); // erase
                    cout << "Stopped sharing file: " << fname << " in group " << gid << endl;
                } 
                else 
                {
                    cout << "File " << fname << " is not being shared by you." << endl;
                }
                // telling tracker to remove this peer as seeder
                string msg = "stop_share " + gid + " " + fname + " " + peername; 
                string resp = sendcomd(serversock, msg); 
                cout << resp << endl;
            });
        };

        cmdMap["create_user"] = [&]() 
        {
            if (length != 3) 
            { 
                cout << "Usage: create_user <user> <pass>\n"; 
                return; 
            }
            string msg = "create_user " + cmds[1] + " " + cmds[2];
            cout << sendcomd(serversock, msg) << endl; 
        };

        cmdMap["login"] = [&]() 
        {
            if (length != 3) 
            { 
                cout << "Usage: login <user> <pass>\n"; 
                return; 
            }
            if (connected) 
            { 
                cout << "-------- User session already active --------" << endl; 
                return; 
            }
            string msg = "login " + cmds[1] + " " + cmds[2] + " " + hostip + " " + hostport; 
            string r = sendcomd(serversock, msg); 
            if (!r.empty() && r[0] == 'S') 
            { 
                logout_local(); 
                login_local(cmds[1]); 
                cout << "********* You are now logged in *********" << endl; 
            }
            else cout << r << endl;
        };

        cmdMap["logout"] = [&]() 
        {
            logincheck([&]() 
            {
                string r = sendcomd(serversock, "logout " + peername);
                cout << r << endl; 
                logout_local(); 
            });
        };

        cmdMap["create_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: create_group <groupid>\n"; 
                    return; 
                }
                cout << sendcomd(serversock, "create_group " + cmds[1] + " " + peername) << endl; 
            });
        };

        cmdMap["join_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: join_group <groupid>\n"; 
                    return; 
                }
                cout << sendcomd(serversock, "join_group " + cmds[1] + " " + peername) << endl; 
            });
        };

        cmdMap["leave_group"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: leave_group <groupid>\n"; 
                    return; 
                }
                cout << sendcomd(serversock, "leave_group " + cmds[1] + " " + peername) << endl; 
            });
        };

        cmdMap["list_requests"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: list_requests <groupid>\n"; 
                    return; 
                }
                cout << sendcomd(serversock, "list_requests " + cmds[1] + " " + peername) << endl; 
            });
        };

        cmdMap["accept_request"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 3) 
                { 
                    cout << "Usage: accept_request <groupid> <user>\n"; 
                    return; 
                } 
                cout << sendcomd(serversock, "accept_request " + cmds[1] + " " + cmds[2] + " " + peername) << endl; 
            });
        };

        cmdMap["list_groups"] = [&]() 
        {
            logincheck([&]() 
            { 
                cout << sendcomd(serversock, "list_groups") << endl; 
            }); 
        };

        cmdMap["list_files"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length != 2) 
                { 
                    cout << "Usage: list_files <groupid>\n"; 
                    return; 
                }
                cout << sendcomd(serversock, "list_files " + cmds[1] + " " + peername) << endl; 
            });
        };

        // upload_file: allow spaces in filepath by recombining tokens
        cmdMap["upload_file"] = [&]() 
        {
            logincheck([&]() 
            {
                if (length < 3) 
                { 
                    cout << "Usage: upload_file <groupid> <filepath>\n"; 
                    return; 
                }
                string gid = cmds[1]; // group id
                string fpath; // file path
                for (int i = 2; i < length; ++i) 
                {
                    if (i > 2) fpath += " "; // add space
                    fpath += cmds[i]; // add token
                }
                int fd = open(fpath.c_str(), O_RDONLY);
                if (fd < 0) 
                { 
                    cout << "File not found: " << fpath << endl; 
                    return; 
                } 
                off_t fsize = lseek(fd, 0, SEEK_END);
                close(fd);
                if (fsize == 0) 
                { 
                    cout << "Cannot upload empty file: " << fpath << endl; 
                    return; 
                }
                // compute piece hashes and full hash
                long long num_pieces = 0; // pieces
                vector<string> piece_hashes = compute_piece_hashes(fpath, num_pieces); // get hashes
                string fullhash = filehash(fpath); // get full hash
                size_t pos = fpath.find_last_of("/"); // find last /
                string fname = (pos == string::npos) ? fpath : fpath.substr(pos + 1); // get file name
                // store file locally so peer server can serve pieces
                uploaded_files[fname] = fpath;

                string cmd = "upload_file " + gid + " " + fname + " " + peername + " " + to_string(fsize) + " " + fullhash + " " + to_string(num_pieces);
                for (auto &h : piece_hashes) cmd += " " + h; // add hashes
                string r = sendcomd(serversock, cmd);
                cout << r << endl;
            });
        };

        // download_file command
        cmdMap["download_file"] = [&]() {
          
            
            logincheck([&]() {
                if (length < 4) 
                { 
                    cout << "Usage: download_file <groupid> <filename> <dest_path>\n"; 
                    return; 
                }

                string gid = cmds[1], fname = cmds[2], destpath = cmds[3];
                
                // checking if already downloading this file
                {
                    lock_guard<mutex> lock(downloads_mtx); 
                    if (active_downloads.find(fname) != active_downloads.end() && active_downloads[fname].is_active) 
                    {
                        cout << "File " << fname << " is already being downloaded.\n";
                        return;
                    }
                }

                // read all bytes from a socket
                auto read_all = [](int sock, char *buf, size_t n) -> bool 
                {
                    size_t total = 0;
                    while (total < n) 
                    {
                        ssize_t r = read(sock, buf + total, n - total); 
                        if (r <= 0) return false; 
                        total += r; 
                    }
                    return true; 
                };

                // query tracker for file metadata and peers
                string tracker_cmd = "download_file " + gid + " " + fname + " " + peername; 
                
                string r = sendcomd(serversock, tracker_cmd); 
                if (r.rfind("FILE ", 0) != 0) 
                { 
                    cout << r << endl; 
                    return; 
                }

                // parsing file metadata
                stringstream s(r); 
                string token; 
                long long size = 0; 
                string fullhash;
                long long num_pieces = 0; 
                vector<string> piece_hashes; 
                string word; 
                while (s >> word) 
                {
                    if (word == "SIZE") s >> size; 
                    else if (word == "HASH") s >> fullhash; 
                    else if (word == "PIECES") s >> num_pieces; 
                    else if (word == "PIECE_HASHES") break; 
                }
                piece_hashes.resize(num_pieces); // resize
                for (long long i = 0; i < num_pieces; ++i)
                {
                    s >> piece_hashes[i]; // get hashes
                } 

                // parses available peers
                size_t pos = r.find("\nPEERS\n"); // find peers
                vector<tuple<string,string,string>> peerlist; // peer list
                
                if (pos != string::npos) 
                {
                    string peers_block = r.substr(pos + 7); // get block
                    stringstream sp(peers_block); 
                    string pname, pip, pport; 
                    while (sp >> pname >> pip >> pport) 
                    {
                        peerlist.emplace_back(pname, pip, pport);
                    }
                }

                if (peerlist.empty()) 
                { 
                    cout << "No active peers available for " << fname << ".\n"; 
                    return; 
                }

                if (destpath.back() != '/') 
                {
                    destpath += "/";
                }

                string fullout = destpath + fname;
                FILE *outf = fopen(fullout.c_str(), "rb+"); 
                if (!outf) 
                {
                    outf = fopen(fullout.c_str(), "wb+");
                    if (!outf) 
                    { 
                        cout << "Failed to create output file: " << fullout << endl; 
                        return; 
                    }
                }
                // Pre-allocate file size for large files (>2GB support)
                if (size > 0) 
                {
                    if (fseeko(outf, (off_t)(size - 1), SEEK_SET) != 0) 
                    {
                        cout << "Failed to seek to end of large file (size: " << size << " bytes)\n";
                        fclose(outf);
                        return;
                    }
                    if (fputc(0, outf) == EOF) 
                    {
                        cout << "Failed to write to end of large file\n";
                        fclose(outf);
                        return;
                    }
                }
                if (fflush(outf) != 0) 
                {
                    cout << "Failed to flush file allocation\n";
                }
                fclose(outf);

                vector<int> piece_status(num_pieces, 0); // status
                string statefile = fullout + ".downloading"; // state file
                FILE *statein = fopen(statefile.c_str(), "rb"); // open state
                if (statein) 
                {
                    for (long long i = 0; i < num_pieces; ++i) 
                    {
                        int st = 0; 
                        if (fread(&st, sizeof(int), 1, statein) == 1) piece_status[i] = st;
                    }
                    fclose(statein); 
                }

                // init download tracking
                DownloadInfo download_info;
                download_info.group_id = gid; 
                download_info.filename = fname; 
                download_info.dest_path = fullout; 
                download_info.total_size = size; 
                download_info.total_pieces = num_pieces; 
                download_info.completed_pieces = 0; 
                download_info.piece_status = piece_status; 
                download_info.piece_hashes = piece_hashes; 
                download_info.full_hash = fullhash; 
                download_info.is_active = true; 
                {
                    lock_guard<mutex> lock(downloads_mtx); 
                    active_downloads[fname] = download_info;
                }

                cout << "Starting download of " << fname << " (" << size << " bytes, " << num_pieces << " pieces) from " << peerlist.size() << " peers.\n";

                // thread-safe piece queue and status tracking
                queue<long long> pending_pieces; // queue
                for (long long i = 0; i < num_pieces; ++i)
                { 
                    if (piece_status[i] != 2) 
                    {
                        pending_pieces.push(i);
                    }
                }
                mutex queue_mtx; 
                const int MAX_RETRIES = 5; // max tries
                mutex state_mtx; 
                atomic<long long> completed_count(0); // completed

                // save state helper
                auto save_state = [&]() 
                {
                    FILE *stateout = fopen(statefile.c_str(), "wb"); 
                    if (!stateout) return; 
                    for (long long i = 0; i < num_pieces; ++i)
                    {
                        fwrite(&piece_status[i], sizeof(int), 1, stateout);
                    } 
                    fclose(stateout); 
                };

                // worker function
                auto worker = [&](int worker_id) 
                {
                    // Create peer order with worker-specific preference to avoid contention
                    vector<int> peer_order(peerlist.size()); // peer order
                    for (int i = 0; i < (int)peerlist.size(); ++i) 
                    {
                        peer_order[i] = i;
                    }
                    // start each worker from a different peer to distribute load
                    rotate(peer_order.begin(), peer_order.begin() + (worker_id % peerlist.size()), peer_order.end()); // rotate
                    
                    while (true) 
                    {
                        long long piece_idx = -1; // piece index
                        {
                            lock_guard<mutex> lock(queue_mtx);
                            if (!pending_pieces.empty()) {
                                piece_idx = pending_pieces.front(); 
                                pending_pieces.pop();
                            }
                        }
                        if (piece_idx == -1) return; 

                        {
                            lock_guard<mutex> lock(downloads_mtx);
                            if (active_downloads.find(fname) != active_downloads.end()) 
                            {
                                active_downloads[fname].piece_status[piece_idx] = 1;
                            }
                        }
                        piece_status[piece_idx] = 1; // set downloading
                        save_state();

                        int attempt = 0; // tries
                        bool success = false; // success

                        while (attempt < MAX_RETRIES && !success) 
                        {
                            for (int peer_idx : peer_order) 
                            {
                                auto &p = peerlist[peer_idx]; 
                                string pip, pport, pname;
                                tie(pname, pip, pport) = p; 

                                int psock = socket(AF_INET, SOCK_STREAM, 0); 
                                if (psock < 0) continue; 

                                struct sockaddr_in addr; 
                                addr.sin_family = AF_INET; 
                                addr.sin_port = htons(stoi(pport)); 
                                if (inet_pton(AF_INET, pip.c_str(), &addr.sin_addr) <= 0) 
                                {
                                    close(psock);
                                    continue;
                                }

                                //  timeout for large pieces (512KB can take time on slow connections)
                                struct timeval tv = {30, 0}; // 30 second timeout
                                setsockopt(psock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)); // set timeout
                                setsockopt(psock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)); // set timeout

                                if (connect(psock, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
                                {
                                    close(psock);
                                    continue;
                                }

                                string preq = "GET_PIECE " + fname + " " + to_string(piece_idx);
                                if (send(psock, preq.c_str(), preq.size(), 0) < 0) 
                                {
                                    close(psock);
                                    continue;
                                }

                                uint32_t piece_size;
                                if (!read_all(psock, (char*)&piece_size, sizeof(piece_size))) 
                                { 
                                    close(psock); 
                                    continue; 
                                }
                                piece_size = ntohl(piece_size); 
                                if (piece_size > PIECE_SIZE) 
                                { 
                                    close(psock); 
                                    continue; 
                                }

                                vector<char> buffer(piece_size); 
                                if (!read_all(psock, buffer.data(), piece_size)) 
                                { 
                                    close(psock); 
                                    continue; 
                                }   
                                close(psock);

                                EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // hash context
                                EVP_DigestInit_ex(ctx, EVP_sha1(), NULL); // init hash
                                EVP_DigestUpdate(ctx, buffer.data(), piece_size); // update hash
                                unsigned char hash[EVP_MAX_MD_SIZE];
                                unsigned int hlen;
                                EVP_DigestFinal_ex(ctx, hash, &hlen); // finish hash
                                EVP_MD_CTX_free(ctx); // free context

                                char hex[41];
                                for (unsigned int i=0;i<hlen;i++) sprintf(hex+i*2,"%02x",hash[i]);
                                hex[40]=0;
                                string recv_hex(hex); // get hash

                                if (recv_hex != piece_hashes[piece_idx]) {
                                    cout << "[Piece " << piece_idx << "] Hash mismatch! Expected: " << piece_hashes[piece_idx] << ", Got: " << recv_hex << endl;
                                    attempt++;
                                    continue;
                                }

                                FILE *fw = fopen(fullout.c_str(), "rb+");
                                if (!fw) 
                                { 
                                    cout << "[Piece " << piece_idx << "] Failed to open output file for writing\n";
                                    attempt++; 
                                    continue; 
                                }
                                
                                // Use fseeko for large file support (>2GB)
                                off_t offset = (off_t)piece_idx * PIECE_SIZE; // offset
                                if (fseeko(fw, offset, SEEK_SET) != 0) 
                                {
                                    cout << "[Piece " << piece_idx << "] Failed to seek to position " << offset << "\n";
                                    fclose(fw);
                                    attempt++;
                                    continue;
                                }
                                
                                size_t written = fwrite(buffer.data(), 1, piece_size, fw); // write
                                if (written != piece_size) 
                                {
                                    cout << "[Piece " << piece_idx << "] Failed to write complete piece. Expected: " << piece_size << ", Written: " << written << "\n";
                                    fclose(fw);
                                    attempt++;
                                    continue;
                                }
                                
                                if (fflush(fw) != 0) 
                                {
                                    cout << "[Piece " << piece_idx << "] Failed to flush data to disk\n";
                                    fclose(fw);
                                    attempt++;
                                    continue;
                                }
                                fclose(fw);

                                {
                                    lock_guard<mutex> lk(state_mtx); 
                                    lock_guard<mutex> dl_lock(downloads_mtx); 
                                    if (active_downloads.find(fname) != active_downloads.end()) 
                                    {
                                        active_downloads[fname].piece_status[piece_idx] = 2;
                                        active_downloads[fname].completed_pieces++;
                                    }
                                }
                                piece_status[piece_idx] = 2; // set completed
                                save_state();

                                completed_count++; // add completed
                                long long progress_pct = (completed_count * 100) / num_pieces;
                                cout << "[Piece " << piece_idx << "] Downloaded successfully from " << pip << ":" << pport << " (" << completed_count << "/" << num_pieces << " = " << progress_pct << "%)\n";
                                
                                // report progress at milestones for large files
                                if (num_pieces > 100 && completed_count % (num_pieces / 10) == 0) 
                                {
                                    cout << "*** Download Progress: " << progress_pct << "% complete ***\n";
                                }
                                success = true;
                                break;
                            }
                            if (!success) attempt++;
                        }

                        if (!success) {
                            cout << "[Piece " << piece_idx << "] Failed after " << MAX_RETRIES << " attempts.\n";
                            {
                                lock_guard<mutex> lock(downloads_mtx);
                                if (active_downloads.find(fname) != active_downloads.end()) 
                                {
                                    active_downloads[fname].piece_status[piece_idx] = 3; // set failed
                                }
                            }
                            piece_status[piece_idx] = 3; // set failed
                            save_state();
                        }
                    }
                };

                // limits workers for very large files to prevent memory/resource exhaustion
                int max_workers = (num_pieces > 1000) ? min(4, (int)peerlist.size()) : min((int)peerlist.size(), 8); // workers
                int num_workers = max_workers; // workers
                
                cout << "Using " << num_workers << " download workers for " << num_pieces << " pieces\n";
                
                vector<thread> dthreads; // threads
                for (int i=0;i<num_workers;i++)
                {
                    dthreads.emplace_back(worker,i); // start threads
                } 
                for (auto &t:dthreads) 
                {
                    if (t.joinable()) 
                    {
                        t.join(); // join
                    }
                }

                // final verification
                bool all_completed = true; 
                {
                    lock_guard<mutex> lock(downloads_mtx);
                    if (active_downloads.find(fname) != active_downloads.end()) 
                    {
                        for (long long i=0;i<num_pieces;i++) 
                        {
                            if (active_downloads[fname].piece_status[i] != 2) 
                            {
                                all_completed = false; // not completed
                                break;
                            }
                        }
                    }
                }

                if (!all_completed) 
                {
                    cout << "Download incomplete: some pieces failed.\n";
                    {
                        lock_guard<mutex> lock(downloads_mtx);
                        if (active_downloads.find(fname) != active_downloads.end()) 
                        {
                            active_downloads[fname].is_active = false; // set inactive
                        }
                    }
                    save_state();
                    return;
                }

                // full file hash verification
                string dhash = filehash(fullout); // get hash
                if (dhash == fullhash) 
                {
                    cout << "[C] " << gid << " " << fname << " downloaded successfully.\n";
                    
                    // add downloaded file to uploaded_files so this peer can now serve it to others
                    uploaded_files[fname] = fullout;
                    
                    // tell tracker that this peer now has the file so other peers can download
                    string notify_cmd = "file_downloaded " + gid + " " + fname + " " + peername; 
                    string tracker_response = sendcomd(serversock, notify_cmd); 
                    
                    {
                        lock_guard<mutex> lock(downloads_mtx); 
                        if (active_downloads.find(fname) != active_downloads.end()) 
                        {
                            active_downloads[fname].is_active = false; // set inactive
                        }
                    }
                    remove(statefile.c_str());
                } 
                else 
                {
                    cout << "Full-file hash mismatch! Expected " << fullhash << " got " << dhash << endl;
                    {
                        lock_guard<mutex> lock(downloads_mtx); // lock
                        if (active_downloads.find(fname) != active_downloads.end()) 
                        {
                            active_downloads[fname].is_active = false; // set inactive
                        }
                    }
                    save_state();
                }
            });
        };

        // show_downloads command
        cmdMap["show_downloads"] = [&]() 
        {
            logincheck([&]() 
            {
                // Lock downloads data for thread safety
                lock_guard<mutex> lock(downloads_mtx);
                bool any = false;   // Tracks if any downloads are shown
                cout << "========== Downloads ==========" << endl;
                // Show all active downloads
                
                for (auto &pair : active_downloads) 
                {
                    const DownloadInfo &info = pair.second;
                    // checking if all pieces are completed
                    bool completed = true;
                    for (int status : info.piece_status) 
                    {
                        if (status != 2) 
                        { 
                            completed = false; 
                            break; 
                        }
                    }
                    // show if download is active or completed
                    if (info.is_active || completed) 
                    {
                        any = true;
                        // calc progress percentage
                        int progress = (info.completed_pieces * 100) / info.total_pieces;
                        cout << "File: " << info.filename << "\n";
                        cout << "  Group: " << info.group_id << "\n";
                        cout << "  Size: " << info.total_size << " bytes\n";
                        cout << "  Progress: " << info.completed_pieces << "/" << info.total_pieces << " pieces (" << progress << "%)\n";
                        cout << "  Status: ";
                        // countt pieces by status
                        int pending = 0, downloading = 0, done = 0, failed = 0;
                        
                        for (int status : info.piece_status) 
                        {
                            if (status == 0) pending++;         // not started
                            else if (status == 1) downloading++; // in progress
                            else if (status == 2) done++;        // completed
                            else if (status == 3) failed++;      // failed
                        }

                        if (completed)
                        {
                            cout << "COMPLETED";
                        } 
                        else
                        {
                            cout << pending << " pending, " << downloading << " downloading, " << done << " completed, " << failed << " failed";
                        } 
                        cout << "\n----------------------------------------\n";
                    }
                }
                if (!any) 
                {
                    cout << "No downloads found.\n";
                }
            });
        };

        // handle exit
        if (cmds[0] == "exit") {
            cout << "------- Exiting Client ---------" << endl; 
            if (serversock > 0)
            {
                close(serversock);
            } 
                
            noaccept = true; // set flag
            shutdown(listenSock, SHUT_RDWR); 
            close(listenSock); 
            if (help_object.joinable()) help_object.join(); // join thread
            {
                lock_guard<mutex> lock(mtx); 
                poolstop = true; // set flag
            }
            cv.notify_all();
            for (auto &t : workers)
            {
                t.join(); // join threads
            } 
            return 0;
        } 
        else if (cmds[0] == "commands") 
        {
            displaycomds(); // show commands
        }
        else if (cmdMap.find(cmds[0]) != cmdMap.end()) 
        {
           
            cmdMap[cmds[0]]();
        }
        else 
        {
            for (auto& pair : cmdMap)
            {
                cout << pair.first << " "<<endl;
            } 
            cout << endl;
            cout << "------- Invalid Command --------" << endl; // error
        }
    }
    
    lock_guard<mutex> lock(mtx); 
    poolstop = true;
    cv.notify_all();
    
    for (auto &t : workers)
    {
        t.join();
    } 
    return 0;
}
