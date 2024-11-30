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


#define SPLIT_SIZE 100048576 // 100MB split size
#define MAX_SPLITS 1000    // Maximum number of splits

// Add debug logging
#define LOG_FILE "/home/tguinot/fuse_debug.log"
#define DEBUG(fmt, ...)                                     \
    do                                                      \
    {                                                       \
        FILE *f = fopen(LOG_FILE, "a");                     \
        if (f)                                              \
        {                                                   \
            fprintf(f, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
            fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
            fclose(f);                                      \
        }                                                   \
    } while (0)

static char *source_path;
static char *mountpoint;

static int get_attr(const char *path, struct stat *stbuf) {
    DEBUG("get_attr called with path: %s", path);

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        DEBUG("Root directory attributes requested");
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    char filename[256];
    int split_num;
    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) == 2) {
        DEBUG("Split file attributes requested for %d_%s",split_num, filename);
        struct stat st;
        if (stat(source_path, &st) == -1) {
            DEBUG("stat failed for source path: %s, errno: %d", source_path, errno);
            return -errno;
        }

        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = (st.st_size > (split_num + 1) * SPLIT_SIZE) ? SPLIT_SIZE : (st.st_size - split_num * SPLIT_SIZE);
        DEBUG("Returning file size: %ld", (long)stbuf->st_size);
        return 0;
    }

    DEBUG("File not found: %s", path);
    return -ENOENT;
}

static int read_dir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    DEBUG("read_dir called with path: %s", path);

    (void)offset;
    (void)fi;

    if (strcmp(path, "/") != 0) {
        DEBUG("Invalid directory path: %s", path);
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct stat st;
    if (stat(source_path, &st) == -1) {
        DEBUG("stat failed for source path: %s, errno: %d", source_path, errno);
        return -errno;
    }

    int num_splits = (st.st_size + SPLIT_SIZE - 1) / SPLIT_SIZE;
    char split_name[256];
    char *base_name = strrchr(source_path, '/');
    base_name = base_name ? base_name + 1 : (char *)source_path;

    DEBUG("Creating %d splits for file size %ld", num_splits, (long)st.st_size);

    for (int i = 0; i < num_splits && i < MAX_SPLITS; i++) {
        snprintf(split_name, sizeof(split_name), "%d_%s", i, base_name);
        DEBUG("Adding split file: %s", split_name);
        filler(buf, split_name, NULL, 0);
    }

    return 0;
}

static int open_file(const char *path, struct fuse_file_info *fi) {
    DEBUG("open_file called with path: %s", path);

    char filename[256];
    int split_num;
    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) != 2) {
        DEBUG("Failed to parse split file path: %s", path);
        return -ENOENT;
    }

    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        DEBUG("Attempted write access, denied");
        return -EACCES;
    }

    DEBUG("File opened successfully");
    return 0;
}

static int read_file(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    DEBUG("read_file called with path: %s, size: %zu, offset: %ld for source %s",
          path, size, (long)offset, source_path);

    (void)fi;
    char filename[256];
    int split_num;

    if (sscanf(path, "/%d_%[^\n]", &split_num, filename) != 2) {
        DEBUG("Failed to parse split file path: %s", path);
        return -ENOENT;
    }

    int fd = open(source_path, O_RDONLY);
    if (fd == -1){
        DEBUG("Failed to open source file: %s, errno: %d", source_path, errno);
        return -errno;
    }

    off_t file_offset = split_num * SPLIT_SIZE + offset;
    DEBUG("lseeking with file_offset: %ld using split_num %d", file_offset, split_num);
    if (lseek(fd, file_offset, SEEK_SET) == -1){
        DEBUG("lseek failed, offset: %ld, errno: %d", (long)file_offset, errno);
        close(fd);
        return -errno;
    }

    int res = read(fd, buf, size);
    if (res == -1){
        DEBUG("read failed, errno: %d", errno);
        res = -errno;
    }
    else {
        DEBUG("Successfully read %d bytes", res);
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

    DEBUG("------------");
    DEBUG("Starting FUSE filesystem");
    DEBUG("Source path: %s", source_path);
    DEBUG("Mount point: %s", mountpoint);

    // Pass all arguments directly to fuse_main, just skip the source_file argument
    argv[1] = argv[2];  // Move mountpoint to position 1
    int ret = fuse_main(argc - 1, &argv[0], &split_file_oper, NULL);

    DEBUG("Finished, good bye");
    fflush(stdout);
    return ret;
}
