#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <filesystem>
#include <cstdio>
#include <cstdarg>
#include <syslog.h>
#include <format>

#define SPLIT_SIZE 100048576 // 100MB split size
#define MAX_SPLITS 1000    // Maximum number of splits


using std::format;
using std::format_string;

class SysLogger {
public:
    SysLogger(int options = LOG_PID, int facility = LOG_USER) {
        openlog("splinterfs", options, facility);
    }

    ~SysLogger() {
        closelog();
    }   

    template<typename... Args>
    void critical(format_string<Args...> fmt, Args&&... args) {
        log(LOG_CRIT, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void error(format_string<Args...> fmt, Args&&... args) {
        log(LOG_ERR, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warning(format_string<Args...> fmt, Args&&... args) {
        log(LOG_WARNING, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void info(format_string<Args...> fmt, Args&&... args) {
        log(LOG_INFO, format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    void debug(format_string<Args...> fmt, Args&&... args) {
        log(LOG_DEBUG, format(fmt, std::forward<Args>(args)...));
    }

private:
    void log(int priority, const std::string& message) {
        syslog(priority, "%s", message.c_str());
    }
};

static char *source_path;
static char *mountpoint;

SysLogger logger;

static int get_attr(const char *path, struct stat *stbuf) {
    logger.debug("get_attr called with path: {}", path);

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        logger.debug("Root directory attributes requested");
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    char filename[256];
    int split_num;
    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) == 2) {
        logger.debug("Split file attributes requested for {}_{}",split_num, filename);
        struct stat st;
        if (stat(source_path, &st) == -1) {
            logger.debug("stat failed for source path: {}, errno: {}", source_path, errno);
            return -errno;
        }

        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = (st.st_size > (split_num + 1) * SPLIT_SIZE) ? SPLIT_SIZE : (st.st_size - split_num * SPLIT_SIZE);
        logger.debug("Returning file size: %ld", (long)stbuf->st_size);
        return 0;
    }

    logger.debug("File not found: {}", path);
    return -ENOENT;
}

static int read_dir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    logger.debug("read_dir called with path: {}", path);

    (void)offset;
    (void)fi;

    if (strcmp(path, "/") != 0) {
        logger.debug("Invalid directory path: {}", path);
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct stat st;
    if (stat(source_path, &st) == -1) {
        logger.debug("stat failed for source path: {}, errno: {}", source_path, errno);
        return -errno;
    }

    int num_splits = (st.st_size + SPLIT_SIZE - 1) / SPLIT_SIZE;
    char split_name[256];
    char *base_name = strrchr(source_path, '/');
    base_name = base_name ? base_name + 1 : (char *)source_path;

    logger.debug("Creating {} splits for file size %ld", num_splits, (long)st.st_size);

    for (int i = 0; i < num_splits && i < MAX_SPLITS; i++) {
        snprintf(split_name, sizeof(split_name), "%d_%s", i, base_name);
        logger.debug("Adding split file: {}", split_name);
        filler(buf, split_name, NULL, 0);
    }

    return 0;
}

static int open_file(const char *path, struct fuse_file_info *fi) {
   logger. debug("open_file called with path: {}", path);

    char filename[256];
    int split_num;
    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) != 2) {
        logger.debug("Failed to parse split file path: {}", path);
        return -ENOENT;
    }

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        logger.debug("Attempted write access, denied");
        return -EACCES;
    }

    logger.debug("File opened successfully");
    return 0;
}

static int read_file(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    logger.debug("read_file called with path: {}, size: %zu, offset: %ld for source {}",
          path, size, (long)offset, source_path);

    (void)fi;
    char filename[256];
    int split_num;

    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) != 2) {
        logger.debug("Failed to parse split file path: {}", path);
        return -ENOENT;
    }

    int fd = open(source_path, O_RDONLY);
    if (fd == -1){
        logger.debug("Failed to open source file: {}, errno: {}", source_path, errno);
        return -errno;
    }

    off_t file_offset = split_num * SPLIT_SIZE + offset;
    logger.debug("lseeking with file_offset: %ld using split_num {}", file_offset, split_num);
    if (lseek(fd, file_offset, SEEK_SET) == -1){
        logger.debug("lseek failed, offset: %ld, errno: {}", (long)file_offset, errno);
        close(fd);
        return -errno;
    }

    int res = read(fd, buf, size);
    if (res == -1){
        logger.debug("read failed, errno: {}", errno);
        res = -errno;
    }
    else {
        logger.debug("Successfully read {} bytes", res);
    }

    close(fd);
    return res;
}

static struct fuse_operations split_file_oper = {
    .getattr = get_attr,
    .open = open_file,
    .read = read_file,
    .readdir = read_dir,
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <source_file> <mountpoint> [FUSE options]\n", argv[0]);
        exit(1);
    }

    source_path = argv[1];
    mountpoint = argv[2];

    // Create mountpoint directory if it doesn't exist
    std::filesystem::create_directory(std::string(mountpoint));

    logger.debug("------------");
    logger.debug("Starting FUSE filesystem");
    logger.debug("Source path: {}", source_path);
    logger.debug("Mount point: {}", mountpoint);

    // Pass all arguments directly to fuse_main, just skip the source_file argument
    argv[1] = argv[2];  // Move mountpoint to position 1
    int ret = fuse_main(argc - 1, &argv[0], &split_file_oper, NULL);

    logger.debug("Finished, good bye");
    fflush(stdout);
    return ret;
}
