// Host filesystem access for macOS / Linux: thin wrappers over POSIX.
#include "hostfs.h"
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>

namespace hfs {

static int err() { return -host_errno_to_linux(errno); }

static void conv(const struct stat& s, Stat& o) {
    u32 mode = s.st_mode & 07777;
    switch (s.st_mode & S_IFMT) {
    case S_IFREG: mode |= lnx::L_S_IFREG; break;
    case S_IFDIR: mode |= lnx::L_S_IFDIR; break;
    case S_IFLNK: mode |= lnx::L_S_IFLNK; break;
    case S_IFIFO: mode |= lnx::L_S_IFIFO; break;
    case S_IFSOCK: mode |= lnx::L_S_IFSOCK; break;
    case S_IFCHR: mode |= lnx::L_S_IFCHR; break;
    case S_IFBLK: mode |= lnx::L_S_IFBLK; break;
    }
    o.mode = mode;
    o.size = (u64)s.st_size;
    o.ino = (u64)s.st_ino;
    o.nlink = (u32)s.st_nlink;
    o.blocks = (u64)s.st_blocks;
#if defined(__APPLE__)
    o.atime_ns = (u64)s.st_atimespec.tv_sec * 1000000000ull + s.st_atimespec.tv_nsec;
    o.mtime_ns = (u64)s.st_mtimespec.tv_sec * 1000000000ull + s.st_mtimespec.tv_nsec;
    o.ctime_ns = (u64)s.st_ctimespec.tv_sec * 1000000000ull + s.st_ctimespec.tv_nsec;
#else
    o.atime_ns = (u64)s.st_atim.tv_sec * 1000000000ull + s.st_atim.tv_nsec;
    o.mtime_ns = (u64)s.st_mtim.tv_sec * 1000000000ull + s.st_mtim.tv_nsec;
    o.ctime_ns = (u64)s.st_ctim.tv_sec * 1000000000ull + s.st_ctim.tv_nsec;
#endif
}

int open(const std::string& path, u32 f, u32 mode) {
    int h = f & 3;
    if (f & lnx::L_O_CREAT) h |= O_CREAT;
    if (f & lnx::L_O_EXCL) h |= O_EXCL;
    if (f & lnx::L_O_TRUNC) h |= O_TRUNC;
    if (f & lnx::L_O_NOFOLLOW) h |= O_NOFOLLOW;
    if (f & lnx::L_O_NOCTTY) h |= O_NOCTTY;
    if (f & lnx::L_O_DIRECTORY) h |= O_DIRECTORY;
    int fd = ::open(path.c_str(), h | O_CLOEXEC, mode & 07777);
    return fd < 0 ? err() : fd;
}
int close(int fd) { return ::close(fd) ? err() : 0; }
s64 read(int fd, void* buf, u32 n) {
    while (true) {
        ssize_t r = ::read(fd, buf, n);
        if (r >= 0) return r;
        if (errno != EINTR) return err();
    }
}
s64 write(int fd, const void* buf, u32 n, bool append) {
    if (append) ::lseek(fd, 0, SEEK_END);
    while (true) {
        ssize_t r = ::write(fd, buf, n);
        if (r >= 0) return r;
        if (errno != EINTR) return err();
    }
}
s64 pread(int fd, void* buf, u32 n, s64 off) {
    ssize_t r = ::pread(fd, buf, n, (off_t)off);
    return r < 0 ? err() : r;
}
s64 pwrite(int fd, const void* buf, u32 n, s64 off) {
    ssize_t r = ::pwrite(fd, buf, n, (off_t)off);
    return r < 0 ? err() : r;
}
s64 lseek(int fd, s64 off, int whence) {
    off_t r = ::lseek(fd, (off_t)off, whence);
    return r < 0 ? err() : r;
}
int fstat(int fd, Stat& st) {
    struct stat s;
    if (::fstat(fd, &s)) return err();
    conv(s, st);
    return 0;
}
int ftruncate(int fd, s64 len) { return ::ftruncate(fd, (off_t)len) ? err() : 0; }
int stat(const std::string& p, Stat& st) {
    struct stat s;
    if (::stat(p.c_str(), &s)) return err();
    conv(s, st);
    return 0;
}
int lstat(const std::string& p, Stat& st) {
    struct stat s;
    if (::lstat(p.c_str(), &s)) return err();
    conv(s, st);
    return 0;
}
int readlink(const std::string& p, std::string& target) {
    char buf[4096];
    ssize_t n = ::readlink(p.c_str(), buf, sizeof buf);
    if (n < 0) return err();
    target.assign(buf, (size_t)n);
    return 0;
}
int symlink(const std::string& target, const std::string& p) { return ::symlink(target.c_str(), p.c_str()) ? err() : 0; }
int link(const std::string& o, const std::string& n) { return ::link(o.c_str(), n.c_str()) ? err() : 0; }
int mkfifo(const std::string& p, u32 mode) { return ::mkfifo(p.c_str(), mode & 0777) ? err() : 0; }
int unlink(const std::string& p) { return ::unlink(p.c_str()) ? err() : 0; }
int rmdir(const std::string& p) { return ::rmdir(p.c_str()) ? err() : 0; }
int mkdir(const std::string& p, u32 mode) { return ::mkdir(p.c_str(), mode & 0777) ? err() : 0; }
int rename(const std::string& o, const std::string& n) { return ::rename(o.c_str(), n.c_str()) ? err() : 0; }
int truncate(const std::string& p, s64 len) { return ::truncate(p.c_str(), (off_t)len) ? err() : 0; }
bool exists(const std::string& p) {
    struct stat s;
    return ::lstat(p.c_str(), &s) == 0;
}

static u8 dtype_of(u8 hostd) {
    switch (hostd) {
    case DT_DIR: return 4; case DT_REG: return 8; case DT_LNK: return 10; case DT_FIFO: return 1;
    case DT_SOCK: return 12; case DT_CHR: return 2; case DT_BLK: return 6;
    }
    return 0;
}

int list_dir(const std::string& p, std::vector<std::pair<std::string, u8>>& out) {
    DIR* d = opendir(p.c_str());
    if (!d) return err();
    while (dirent* e = readdir(d)) out.push_back({e->d_name, dtype_of(e->d_type)});
    closedir(d);
    return 0;
}

bool write_file(const std::string& p, const void* data, size_t n, u32 mode) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    bool ok = ::write(fd, data, n) == (ssize_t)n;
    ::close(fd);
    return ok;
}

}  // namespace hfs
