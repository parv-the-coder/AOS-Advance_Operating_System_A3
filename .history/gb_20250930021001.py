import os
import random
import string

def generate_random_chunk(size):
    """Generate a random string of the given size."""
    characters = string.ascii_letters + string.digits
    return ''.join(random.choices(characters, k=size))

def create_large_text_file(filename, target_size_gb=1):
    """Create a text file with random characters up to the given size in GB."""
    chunk_size = 1024 * 1024  # 1 MB per chunk
    total_size = target_size_gb * 1024 * 1024 * 1024  # 1 GB in bytes
    written = 0

    with open(filename, 'w') as f:
        while written < total_size:
            remaining = total_size - written
            current_chunk_size = min(chunk_size, remaining)
            chunk = generate_random_chunk(current_chunk_size)
            f.write(chunk)
            written += current_chunk_size
            print(f"\rWritten: {written / (1024 * 1024):.2f} MB", end='')

    print(f"\nDone. File '{filename}' created with size {target_size_gb} GB.")

if __name__ == "__main__":
    create_large_text_file("1GB_file.txt", target_size_gb=1)