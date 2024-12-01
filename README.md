<pre>
░██████╗██████╗░██╗░░░░░██╗███╗░░██╗████████╗███████╗██████╗░░░░░░███████╗░██████╗
██╔════╝██╔══██╗██║░░░░░██║████╗░██║╚══██╔══╝██╔════╝██╔══██╗░░░░░██╔════╝██╔════╝
╚█████╗░██████╔╝██║░░░░░██║██╔██╗██║░░░██║░░░█████╗░░██████╔╝░░░░░█████╗░░╚█████╗░
░╚═══██╗██╔═══╝░██║░░░░░██║██║╚████║░░░██║░░░██╔══╝░░██╔══██╗░░░░░██╔══╝░░░╚═══██╗
██████╔╝██║░░░░░███████╗██║██║░╚███║░░░██║░░░███████╗██║░░██║░░░░░██║░░░░░██████╔╝
╚═════╝░╚═╝░░░░░╚══════╝╚═╝╚═╝░░╚══╝░░░╚═╝░░░╚══════╝╚═╝░░╚═╝░░░░░╚═╝░░░░░╚═════╝░
</pre>

Dead simple FUSE filesystem that splits large files into fixed-size chunks for easier handling. Shows files as pieces. Built using FUSE (Filesystem in Userspace), which lets you implement custom filesystems without kernel code.

## Why?
- Break down huge files without actually splitting them
- Offers access to virtual chunks independently
- No actual file splitting/disk space wasted

## Usage
```bash
./splinterfs big_file.mp4 /mnt/chunks
```

Your 40GB video will show up as:
```
/mnt/chunks/0_big_file.mp4
/mnt/chunks/1_big_file.mp4
...
/mnt/chunks/99_big_file.mp4
```

Each chunk appears as a read-only slice of the original file. Zero extra disk space needed - it's all virtual mapping.

## Build

Needs FUSE dev libs. On Ubuntu/Debian:
```bash
sudo apt-get install libfuse-dev
```

To build:
```bash
git clone https://github.com/tguinot/splinterfs.git

cd splinterfs
mkdir build
cd build

cmake ..
make
```

Read-only, no fancy stuff. Just works™

## How it works
Maps file chunks on-the-fly using FUSE callbacks. When you read a chunk, split-fs calculates the offset in the original file and serves the data directly. No temporary files, no copies, just pure virtual files.
