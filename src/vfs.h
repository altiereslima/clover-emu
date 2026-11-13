// Virtual filesystem: guest path resolution onto the extracted rootfs + overlays + virtual devices.
#pragma once
#include "common.h"
#include "hostfs.h"
#include <memory>
#include <mutex>
#include <functional>

struct Thread;
struct Process;

// Linux ARM EABI struct stat64 (104 bytes)
struct LinuxStat64 {
    u64 st_dev;
    u8 pad0[4];
    u32 st_ino_lo;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u8 pad3[4];
    u8 pad3b[4];
    s64 st_size;
    u32 st_blksize;
    u32 pad4;
    u64 st_blocks;
    u32 st_atime_, st_atime_nsec;
    u32 st_mtime_, st_mtime_nsec;
    u32 st_ctime_, st_ctime_nsec;
    u64 st_ino;
};
static_assert(sizeof(LinuxStat64) == 104, "stat64 layout");

void host_stat_to_linux(const hfs::Stat& st, LinuxStat64& out);
LinuxStat64 make_stat(u32 mode, u64 size = 0, u64 rdev = 0, u64 ino = 1);

// Open file description.
struct File : std::enable_shared_from_this<File> {
    virtual ~File() = default;
    u32 flags = 0;          // linux status flags (O_NONBLOCK, O_APPEND, accmode)
    std::string path;       // guest path
    virtual s64 read(Thread&, u8* buf, u32 n) { return -lnx::EINVAL_; }
    virtual s64 write(Thread&, const u8* buf, u32 n) { return -lnx::EINVAL_; }
    virtual s64 pread(Thread& t, u8* buf, u32 n, s64 off) { return -lnx::ESPIPE_; }
    virtual s64 pwrite(Thread& t, const u8* buf, u32 n, s64 off) { return -lnx::ESPIPE_; }
    virtual s64 lseek(s64 off, int whence) { return -lnx::ESPIPE_; }
    virtual int ioctl(Thread&, u32 req, u32 arg) { return -lnx::ENOTTY_; }
    virtual int fstat(LinuxStat64& st) = 0;
    virtual int getdents64(Thread&, u8* buf, u32 n) { return -lnx::ENOTDIR_; }
    // Readiness for poll/select/epoll: mask of POLLIN(1) / POLLOUT(4) / POLLERR(8) / POLLHUP(0x10).
    // Files whose readiness can change (pipes, sockets, eventfd) call io_notify() when it does.
    virtual u32 poll_mask() { return 1 | 4; }
    // mmap hook. Return -errno, or 0 and fill *done=true if handled (shared mapping); otherwise
    // default implementation copies contents via pread.
    virtual int mmap(Thread&, u32 addr, u32 len, u32 prot, u32 flags, u64 off, bool* handled) { *handled = false; return 0; }
    // Non-zero: the mapping must live at this guest address (shared device memory on Windows).
    virtual u32 mmap_fixed(u64 off) { return 0; }
    virtual int ftruncate(s64 len) { return -lnx::EINVAL_; }
    virtual int fsync() { return 0; }
    virtual bool is_socket() const { return false; }
    virtual std::string link_name() { return path; }  // /proc/self/fd/N target
    virtual void on_close() {}
};
using FilePtr = std::shared_ptr<File>;

// File backed by a host file or directory (hostfs fd)
struct HostFile : File {
    int fd = -1;
    bool is_dir = false;
    std::string host_path;
    // directory iteration state
    std::vector<std::string> extra_entries; // merged entries (e.g. virtual mounts)
    size_t dir_pos = 0;
    std::vector<std::pair<std::string, u8>> dir_list; bool dir_loaded = false;
    ~HostFile() override;
    s64 read(Thread&, u8* buf, u32 n) override;
    s64 write(Thread&, const u8* buf, u32 n) override;
    s64 pread(Thread&, u8* buf, u32 n, s64 off) override;
    s64 pwrite(Thread&, const u8* buf, u32 n, s64 off) override;
    s64 lseek(s64 off, int whence) override;
    int ioctl(Thread&, u32 req, u32 arg) override;
    int fstat(LinuxStat64& st) override;
    int getdents64(Thread&, u8* buf, u32 n) override;
    int ftruncate(s64 len) override;
    int fsync() override;
};

// In-memory file with fixed contents (procfs/sysfs text)
struct MemFile : File {
    std::string data;
    s64 pos = 0;
    u32 mode = lnx::L_S_IFREG | 0444;
    std::function<void(const std::string&)> on_write;
    s64 read(Thread&, u8* buf, u32 n) override;
    s64 write(Thread&, const u8* buf, u32 n) override;
    s64 pread(Thread&, u8* buf, u32 n, s64 off) override;
    s64 lseek(s64 off, int whence) override;
    int fstat(LinuxStat64& st) override;
};

// Virtual directory listing (for /proc, /dev, ...)
struct VirtDir : File {
    std::vector<std::pair<std::string, u8>> entries;  // name, d_type
    size_t pos = 0;
    int fstat(LinuxStat64& st) override;
    int getdents64(Thread&, u8* buf, u32 n) override;
    s64 lseek(s64 off, int whence) override { if (off == 0 && whence == 0) { pos = 0; return 0; } return -lnx::EINVAL_; }
};

// Result of resolving a guest path.
struct Resolved {
    enum Kind { NONE, HOST, VIRT } kind = NONE;
    std::string guest;      // canonical guest path
    std::string host;       // host path (for HOST)
    bool writable = false;  // whether on writable mount
    int err = 0;            // linux errno if failed
};

namespace vfs {
void init(const std::string& rootfs, const std::string& datadir, const std::string& rundir);
// Resolve guest path relative to cwd. follow_last: follow final symlink.
Resolved resolve(Process& p, const std::string& path, bool follow_last = true, int dirfd = lnx::L_AT_FDCWD);
std::string join(const std::string& cwd, const std::string& path);
// Open file. Returns file or sets *err (linux errno).
FilePtr open(Thread& t, const std::string& path, u32 flags, u32 mode, int* err, int dirfd = lnx::L_AT_FDCWD);
int stat(Thread& t, const std::string& path, LinuxStat64& st, bool follow, int dirfd = lnx::L_AT_FDCWD);
int readlink(Thread& t, const std::string& path, std::string& out, int dirfd = lnx::L_AT_FDCWD);
bool is_virtual_path(const std::string& guest);
const std::string& rootfs();
void add_override(const std::string& guest, const std::string& host);
// Replace one file only while active() returns true (evaluated whenever the path is looked up).
// Register before the guest starts.
void add_conditional_override(const std::string& guest, const std::string& host, std::function<bool()> active);
// read an entire guest file (for loader)
bool read_file(Process& p, const std::string& path, std::vector<u8>& out, std::string* canonical = nullptr);
}  // namespace vfs

// Device registration (devices.cpp)
FilePtr open_device(Thread& t, const std::string& guest_path, u32 flags, int* err);
bool device_exists(const std::string& guest_path, LinuxStat64* st);
std::vector<std::pair<std::string, u8>> device_list(const std::string& dir);
FilePtr open_proc(Thread& t, const std::string& guest_path, u32 flags, int* err);
int stat_proc(Thread& t, const std::string& guest_path, LinuxStat64& st);
int readlink_proc(Thread& t, const std::string& guest_path, std::string& out);
