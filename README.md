
# Peer-to-Peer File Sharing System

## Compilation and Execution Instructions

### Compilation
Compile both client and tracker:
```bash
cd ../client
cd ../tracker
```
Or manually:
```bash
g++ -o client client.cpp -lssl -lcrypto -lpthread
g++ -o tracker tracker.cpp -lpthread
```

### Execution
1. **Start the Tracker:**
   ```bash
   ./tracker <tracker_config_file> 1
   ```
   - `<tracker_config_file>`: Text file with tracker IP and port (e.g., `127.0.0.1 9000`)
2. **Start a Client:**
   ```bash
   ./client <host_ip:host_port> <tracker_config_file>
   ```
   - `<host_ip:host_port>`: IP and port for this client to listen for peer connections
   - `<tracker_config_file>`: Same format as above

## Architectural Overview

### Design
- **Tracker**: Centralized metadata server. Handles authentication, group management, file registration and peer discovery. Does not store file data.
- **Client**: Each peer can upload/download files, serve pieces to others and interact with the tracker. Clients communicate directly for file transfers.
- **Decentralized Data**: File data is distributed among peers. Tracker only coordinates metadata and peer lists.
- **Threading**: Both tracker and client use threads for concurrent socket handling and downloads.

## Key Algorithms


### Piecewise File Transfer & Round-Robin Peer Selection
- Files are split into fixed-size pieces (default: 512KB).
- Each piece is hashed (SHA1) for integrity.
- Downloaded pieces are verified before being written to disk.
- Multiple threads download pieces in parallel from available peers.
- **Round-Robin Peer Selection:**
  - To balance load and avoid contention, each download worker thread rotates the peer list so that it starts downloading from a different peer.
  - This is implemented by rotating the vector of available peers for each thread, ensuring that requests for pieces are distributed evenly across all peers.
  - If a piece download fails from one peer, the thread tries the next peer in its rotated order, up to a maximum number of retries.
  - This approach helps maximize bandwidth usage and avoids overloading any single peer.

### Hash Verification
- Each file and piece is hashed using OpenSSL SHA1.
- Piece hashes are checked after download, full file hash is checked after all pieces are assembled.

### Download Progress Tracking
- Each download is tracked with a `DownloadInfo` struct, recording status of each piece (pending, downloading, completed, failed).
- Progress and status are shown via the `show_downloads` command.

## Data Structures and Rationale

### Tracker
- `client`: Stores peer info, connection state, and files shared.
- `group`: Manages group membership, applicants, and ownership.
- `FileMeta`: Stores file size, hashes, piece hashes, and list of seeders.
- Maps for users, groups, files, and group-files for fast lookup.

### Client
- `DownloadInfo`: Tracks all metadata and status for each download.
- `active_downloads`: Map of filename to `DownloadInfo` for concurrent downloads.
- Mutexes for thread safety in download tracking and peer serving.

## Network Protocol Design and Message Formats

- **Transport**: All communication uses TCP sockets.
- **Tracker Commands**: Text-based commands sent over sockets, e.g.:
  - `create_user <username> <password>`
  - `login <username> <password> <ip> <port>`
  - `upload_file <groupid> <filename> <username> <size> <hash> <num_pieces> <piece_hashes...>`
  - `download_file <groupid> <filename> <username>`
- **Peer-to-Peer File Transfer**:
  - Request: `GET_PIECE <filename> <piece_index>`
  - Response: [4-byte piece size][piece data]
- **File Metadata Response**:
  - `FILE <filename> SIZE <size> HASH <fullhash> PIECES <num_pieces> PIECE_HASHES <hash1> ... <hashN>\nPEERS\n<peername> <ip> <port> ...`

## Assumptions
- All peers and tracker run on reachable IPs/ports.
- Files are not modified during sharing.
- Peers are trusted to serve correct data (integrity checked via hashes).

## Implemented Features
- User registration and login
- Group creation, join, leave, and membership management
- File upload with piecewise hashing
- Piecewise file download from multiple peers
- Download progress and status tracking
- Full file and piece hash verification
- Stop sharing files
- Console commands for all major operations

## Testing Procedures

### Functional Testing
1. **Start tracker and multiple clients on different terminals.**
2. **Register users and create groups.**
3. **Upload files from one client.**
4. **Download files from another client in the same group.**
5. **Verify file integrity (hashes match).**
6. **Test concurrent downloads.**
7. **Test group membership changes and file sharing controls.**

## File Integrity
- All files are split into pieces (default 512KB).
- Each piece and the full file are hashed using SHA1 for integrity verification.
- Downloads are verified piecewise and as a whole before completion.
