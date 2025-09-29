import os

# Define the file path and the size in bytes (1 GB = 1024^3 bytes)
file_path = "4GB_file.bin"
file_size = 4 * 1024 * 1024 * 1024  # 1 GB in bytes

# Open the file in write-binary mode
with open(file_path, "wb") as f:
    # Generate random data and write it in chunks until the desired size is reached
    chunk_size = 1024 * 1024  # 1 MB per chunk
    while file_size > 0:
        # Determine how much data is left to write
        current_chunk_size = min(chunk_size, file_size)
        # Write a chunk of random bytes to the file
        f.write(os.urandom(current_chunk_size))
        file_size -= current_chunk_size

print(f"4 GB file created at {file_path}")