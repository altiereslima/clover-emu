// Host filesystem access with POSIX semantics, used by the VFS for host-backed guest files.
// All functions take UTF-8 host paths with '/' separators and return >= 0 on success or a
// negative *Linux* errno on failure. Implemented by hostfs_posix.cpp and hostfs_win.cpp.
//
// On Windows, symlinks and FIFOs are stored as small marker files (Cygwin-compatible symlinks),
// and characters NTFS forbids in names (":*?\"<>|\\") are mapped to the Unicode private-use area
// the way Cygwin does, so the guest sees the original names.
#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <utility>

namespace hfs {

struct Stat {
    u32 mode = 0;   // Linux S_IF* type bits | permission bits
    u64 size = 0;
    u64 ino = 0;
    u32 nlink = 1;
    u64 blocks = 0;
    u64 atime_ns = 0, mtime_ns = 0, ctime_ns = 0;
};

int open(const std::string& path, u32 linux_flags, u32 mode);  // returns host fd
int close(int fd);
s64 read(int fd, void* buf, u32 n);
s64 write(int fd, const void* buf, u32 n, bool append);
s64 pread(int fd, void* buf, u32 n, s64 off);
s64 pwrite(int fd, const void* buf, u32 n, s64 off);
s64 lseek(int fd, s64 off, int whence);
int fstat(int fd, Stat& st);
int ftruncate(int fd, s64 len);

int stat(const std::string& path, Stat& st);   // does not follow guest symlinks (the VFS resolves them)
int lstat(const std::string& path, Stat& st);
int readlink(const std::string& path, std::string& target);
int symlink(const std::string& target, const std::string& path);
int link(const std::string& oldpath, const std::string& newpath);
int mkfifo(const std::string& path, u32 mode);
int unlink(const std::string& path);
int rmdir(const std::string& path);
int mkdir(const std::string& path, u32 mode);
int rename(const std::string& oldpath, const std::string& newpath);
int truncate(const std::string& path, s64 len);
bool exists(const std::string& path);
// Directory listing including "." and "..". Types are Linux d_type values (DT_DIR=4, DT_REG=8, DT_LNK=10, DT_FIFO=1).
int list_dir(const std::string& path, std::vector<std::pair<std::string, u8>>& out);

// Write a whole file (creating parent directories is the caller's job).
bool write_file(const std::string& path, const void* data, size_t n, u32 mode = 0644);

}  // namespace hfs
