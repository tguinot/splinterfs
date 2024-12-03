// Set the FUSE API version we're targeting
#define FUSE_USE_VERSION 26

// Required header files for FUSE implementation
#include <fuse.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <string>
#include <vector>
#include <sstream>
#include <memory>
#include "logger.h"
#include "config.h"

// Maximum length for filenames in our filesystem
constexpr size_t MAX_FILENAME_LEN = 256;

// Global variables to store the source file path and mount point
static std::string source_path;    // Path to the original file we're splitting
static std::string mountpoint;     // Where our FUSE filesystem will be mounted

// Logger instance for debugging
SysLogger logger;

/**
 * RAII wrapper for file descriptors to ensure they're properly closed
 * This prevents resource leaks by automatically closing the file descriptor
 * when the object goes out of scope
 */
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ != -1) close(fd_); }
    int get() const { return fd_; }
    operator int() const { return fd_; }
private:
    int fd_;
};

/**
 * Parses a path in the format "/<split_number>_<filename>"
 * 
 * @param path The path to parse
 * @param split_num Output parameter for the split number
 * @param base_filename Output parameter for the original filename
 * @return true if parsing successful, false otherwise
 * 
 * Example: "/0_largefile.txt" -> split_num=0, base_filename="largefile.txt"
 */
bool parse_split_path(const char* path, int& split_num, std::string& base_filename) {
    if (path[0] != '/')
        return false;

    std::string p = path + 1;  // skip leading '/'
    size_t underscore_pos = p.find('_');
    if (underscore_pos == std::string::npos)
        return false;

    std::string split_num_str = p.substr(0, underscore_pos);
    base_filename = p.substr(underscore_pos + 1);

    try {
        split_num = std::stoi(split_num_str);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/**
 * FUSE callback: Get attributes of a file or directory
 * Similar to the 'stat' system call
 * 
 * @param path Path to the file/directory
 * @param stbuf Buffer to store the attributes
 * @return 0 on success, -errno on failure
 */
static int get_attr(const char *path, struct stat *stbuf) {
    logger.debug("get_attr called with path: {}", path);

    memset(stbuf, 0, sizeof(struct stat));
    
    // Handle root directory
    if (strcmp(path, "/") == 0) {
        logger.debug("Root directory attributes requested");
        stbuf->st_mode = S_IFDIR | 0755;  // Directory with read/execute permissions
        stbuf->st_nlink = 2;              // Directory has 2 links: . and ..
        return 0;
    }

    // Handle split files
    int split_num;
    std::string base_filename;
    if (parse_split_path(path, split_num, base_filename)) {
        logger.debug("Split file attributes requested for {}_{}", split_num, base_filename);
        struct stat st;
        if (stat(source_path.c_str(), &st) == -1) {
            logger.debug("stat failed for source path: {}, errno: {}", source_path, errno);
            return -errno;
        }

        // Set up file attributes
        stbuf->st_mode = S_IFREG | 0444;  // Regular file, read-only
        stbuf->st_nlink = 1;
        // Calculate the size of this split (either SPLIT_SIZE or remaining bytes)
        stbuf->st_size = (st.st_size > (split_num + 1) * SPLIT_SIZE) ? 
            SPLIT_SIZE : (st.st_size - split_num * SPLIT_SIZE);
        logger.debug("Returning file size: {}", stbuf->st_size);
        return 0;
    }

    logger.debug("File not found: {}", path);
    return -ENOENT;
}

/**
 * FUSE callback: Read directory contents
 * Similar to 'readdir' system call
 * 
 * @param path Directory path
 * @param buf Buffer to store directory entries
 * @param filler Function to fill directory entries
 * @param offset Offset in the directory (unused)
 * @param fi File info (unused)
 * @return 0 on success, -errno on failure
 */
static int read_dir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    logger.debug("read_dir called with path: {}", path);

    (void)offset;  // Suppress unused parameter warnings
    (void)fi;

    // Only root directory is supported
    if (strcmp(path, "/") != 0) {
        logger.debug("Invalid directory path: {}", path);
        return -ENOENT;
    }

    // Add . and .. entries (current and parent directories)
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // Get source file information
    struct stat st;
    if (stat(source_path.c_str(), &st) == -1) {
        logger.debug("stat failed for source path: {}, errno: {}", source_path, errno);
        return -errno;
    }

    // Calculate number of splits needed
    int num_splits = (st.st_size + SPLIT_SIZE - 1) / SPLIT_SIZE;
    std::filesystem::path source(source_path);
    std::string base_name = source.filename().string();

    logger.debug("Creating {} splits for file size {}", num_splits, st.st_size);

    // Create virtual split files
    for (int i = 0; i < num_splits && i < MAX_SPLITS; i++) {
        std::string split_name = std::to_string(i) + "_" + base_name;
        logger.debug("Adding split file: {}", split_name);
        filler(buf, split_name.c_str(), NULL, 0);
    }

    return 0;
}

/**
 * FUSE callback: Open a file
 * Checks if the file exists and if we have permission to access it
 * 
 * @param path Path to the file
 * @param fi File info structure
 * @return 0 on success, -errno on failure
 */
static int open_file(const char *path, struct fuse_file_info *fi) {
    logger.debug("open_file called with path: {}", path);

    int split_num;
    std::string base_filename;
    if (!parse_split_path(path, split_num, base_filename)) {
        logger.debug("Failed to parse split file path: {}", path);
        return -ENOENT;
    }

    // Only allow read-only access
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        logger.debug("Attempted write access, denied");
        return -EACCES;
    }

    logger.debug("File opened successfully");
    return 0;
}

/**
 * FUSE callback: Read data from a file
 * 
 * @param path Path to the file
 * @param buf Buffer to store read data
 * @param size Number of bytes to read
 * @param offset Offset in the file to start reading from
 * @param fi File info (unused)
 * @return Number of bytes read on success, -errno on failure
 */
static int read_file(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    logger.debug("read_file called with path: {}, size: {}, offset: {} for source {}", 
                path, size, offset, source_path);

    (void)fi;
    int split_num;
    std::string base_filename;

    if (!parse_split_path(path, split_num, base_filename)) {
        logger.debug("Failed to parse split file path: {}", path);
        return -ENOENT;
    }

    // Open source file using RAII wrapper
    FileDescriptor fd(open(source_path.c_str(), O_RDONLY));
    if (fd == -1){
        logger.debug("Failed to open source file: {}, errno: {}", source_path, errno);
        return -errno;
    }

    // Calculate actual file offset based on split number and requested offset
    off_t file_offset = split_num * SPLIT_SIZE + offset;
    logger.debug("Seeking to file_offset: {} using split_num {}", file_offset, split_num);
    if (lseek(fd, file_offset, SEEK_SET) == -1){
        logger.debug("lseek failed, offset: {}, errno: {}", file_offset, errno);
        return -errno;
    }

    // Read the requested data
    int res = read(fd, buf, size);
    if (res == -1){
        logger.debug("read failed, errno: {}", errno);
        res = -errno;
    } else {
        logger.debug("Successfully read {} bytes", res);
    }

    return res;
}

// Structure containing FUSE operation callbacks
static struct fuse_operations split_file_oper = {};

int main(int argc, char *argv[]) {
    // Check command line arguments
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <source_file> <mountpoint> [FUSE options]\n", argv[0]);
        exit(1);
    }

    // Store source file path and mountpoint
    source_path = argv[1];
    mountpoint = argv[2];

    // Ensure mountpoint directory exists
    std::filesystem::create_directory(mountpoint);

    // Log startup information
    logger.debug("------------");
    logger.debug("Starting FUSE filesystem");
    logger.debug("Source path: {}", source_path);
    logger.debug("Mount point: {}", mountpoint);

    // Prepare arguments for fuse_main
    std::vector<char*> fuse_argv;
    fuse_argv.push_back(argv[0]);      // Program name
    fuse_argv.push_back(argv[2]);      // Mountpoint
    for (int i = 3; i < argc; ++i) {   // Other FUSE options
        fuse_argv.push_back(argv[i]);
    }
    fuse_argv.push_back(nullptr);

    // Set up FUSE operations
    split_file_oper.getattr = get_attr;   // Get file attributes
    split_file_oper.open = open_file;     // Open files
    split_file_oper.read = read_file;     // Read from files
    split_file_oper.readdir = read_dir;   // Read directory contents

    // Start the FUSE filesystem
    int ret = fuse_main(fuse_argv.size() - 1, fuse_argv.data(), &split_file_oper, NULL);

    return ret;
}