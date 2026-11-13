// Guest filesystem namespace.
#include "kernel.h"
#include <algorithm>
#include <map>

int readlink_proc_p(Process& p, const std::string& guest_path, std::string& out);
namespace { bool find_override(const std::string& g, std::string& host); }

namespace {
struct Mount {
    std::string guest;  // prefix, no trailing slash ("/" for root)
    std::string host;
    bool writable;
    bool virt;
};
std::vector<Mount> g_mounts;
std::string g_rootfs;
std::map<std::string, std::string> g_overrides;  // canonical guest path -> host file

const Mount* find_mount(const std::string& g) {
    const Mount* best = nullptr;
    size_t best_len = 0;
    for (auto& m : g_mounts) {
        if (m.guest == "/") {
            if (!best) best = &m;
            continue;
        }
        size_t L = m.guest.size();
        if (g.compare(0, L, m.guest) == 0 && (g.size() == L || g[L] == '/') && L > best_len) {
            best = &m;
            best_len = L;
        }
    }
    return best;
}

// Single files replaced only while a condition holds (checked at every lookup), e.g. a translated
// splash screen that depends on the language the console currently has saved.
struct CondOverride {
    std::string host;
    std::function<bool()> active;
};
std::map<std::string, CondOverride> g_cond_overrides;

// Exact-case overrides (stub libraries, case-colliding firmware paths). Matches whole subtrees.
bool find_override(const std::string& g, std::string& host) {
    if (!g_cond_overrides.empty())
        if (auto it = g_cond_overrides.find(g); it != g_cond_overrides.end() && it->second.active()) {
            host = it->second.host;
            return true;
        }
    for (auto& [k, v] : g_overrides) {
        if (g == k) {
            host = v;
            return true;
        }
        if (g.size() > k.size() && g.compare(0, k.size(), k) == 0 && g[k.size()] == '/') {
            host = v + g.substr(k.size());
            return true;
        }
    }
    return false;
}

std::string host_of(const Mount* m, const std::string& g) {
    if (m->guest == "/") return m->host + g;
    std::string rest = g.substr(m->guest.size());
    return m->host + rest;
}

std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') i++;
        size_t j = i;
        while (j < p.size() && p[j] != '/') j++;
        if (j > i) out.push_back(p.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string joinv(const std::vector<std::string>& v) {
    if (v.empty()) return "/";
    std::string s;
    for (auto& c : v) s += "/" + c;
    return s;
}
}  // namespace

void host_stat_to_linux(const hfs::Stat& st, LinuxStat64& o) {
    memset(&o, 0, sizeof o);
    o.st_dev = 0x801;
    o.st_ino = st.ino;
    o.st_ino_lo = (u32)st.ino;
    o.st_mode = st.mode;
    o.st_nlink = st.nlink;
    o.st_uid = 0;
    o.st_gid = 0;
    o.st_size = (s64)st.size;
    o.st_blksize = 4096;
    o.st_blocks = st.blocks;
    o.st_atime_ = (u32)(st.atime_ns / 1000000000ull);
    o.st_atime_nsec = (u32)(st.atime_ns % 1000000000ull);
    o.st_mtime_ = (u32)(st.mtime_ns / 1000000000ull);
    o.st_mtime_nsec = (u32)(st.mtime_ns % 1000000000ull);
    o.st_ctime_ = (u32)(st.ctime_ns / 1000000000ull);
    o.st_ctime_nsec = (u32)(st.ctime_ns % 1000000000ull);
}

LinuxStat64 make_stat(u32 mode, u64 size, u64 rdev, u64 ino) {
    LinuxStat64 o;
    memset(&o, 0, sizeof o);
    o.st_dev = 0x10;
    o.st_ino = ino;
    o.st_ino_lo = (u32)ino;
    o.st_mode = mode;
    o.st_nlink = 1;
    o.st_rdev = rdev;
    o.st_size = (s64)size;
    o.st_blksize = 4096;
    o.st_blocks = (size + 511) / 512;
    u32 t = (u32)(realtime_ns() / 1000000000ull);
    o.st_atime_ = o.st_mtime_ = o.st_ctime_ = t;
    return o;
}

// ---------------------------------------------------------------- HostFile
HostFile::~HostFile() {
    if (fd >= 0) hfs::close(fd);
}

s64 HostFile::read(Thread& t, u8* buf, u32 n) {
    if (is_dir) return -lnx::EISDIR_;
    return hfs::read(fd, buf, n);
}
s64 HostFile::write(Thread& t, const u8* buf, u32 n) {
    if (is_dir) return -lnx::EISDIR_;
    return hfs::write(fd, buf, n, flags & lnx::L_O_APPEND);
}
s64 HostFile::pread(Thread&, u8* buf, u32 n, s64 off) {
    if (is_dir) return -lnx::EISDIR_;
    return hfs::pread(fd, buf, n, off);
}
s64 HostFile::pwrite(Thread&, const u8* buf, u32 n, s64 off) {
    if (is_dir) return -lnx::EISDIR_;
    return hfs::pwrite(fd, buf, n, off);
}
s64 HostFile::lseek(s64 off, int whence) {
    if (is_dir) {
        if (off == 0 && whence == 0) {
            dir_pos = 0;
            dir_loaded = false;
            return 0;
        }
        if (whence == 0) {
            dir_pos = (size_t)off;
            return off;
        }
        return -lnx::EINVAL_;
    }
    return hfs::lseek(fd, off, whence);
}
int HostFile::ioctl(Thread& t, u32 req, u32 arg) {
    AddressSpace& m = *t.proc->mem;
    switch (req) {
    case 0x541B: {  // FIONREAD
        if (is_dir) return -lnx::EISDIR_;
        hfs::Stat st;
        int r = hfs::fstat(fd, st);
        if (r < 0) return r;
        s64 pos = hfs::lseek(fd, 0, 1);
        m.wr<s32>(arg, pos >= 0 && (s64)st.size > pos ? (s32)(st.size - pos) : 0);
        return 0;
    }
    case 0x5421: {  // FIONBIO
        int on = m.rd<s32>(arg);
        if (on) flags |= lnx::L_O_NONBLOCK; else flags &= ~lnx::L_O_NONBLOCK;
        return 0;
    }
    }
    return -lnx::ENOTTY_;
}
int HostFile::fstat(LinuxStat64& st) {
    hfs::Stat hs;
    int r = is_dir ? hfs::stat(host_path, hs) : hfs::fstat(fd, hs);
    if (r < 0) return r;
    host_stat_to_linux(hs, st);
    return 0;
}
int HostFile::ftruncate(s64 len) {
    if (is_dir) return -lnx::EISDIR_;
    return hfs::ftruncate(fd, len);
}
int HostFile::fsync() { return 0; }

static int emit_dirents(AddressSpace& m, u8* buf, u32 n, const std::vector<std::pair<std::string, u8>>& list, size_t& pos) {
    u32 off = 0;
    while (pos < list.size()) {
        auto& [name, type] = list[pos];
        u32 reclen = (u32)((19 + name.size() + 1 + 7) & ~7ull);
        if (off + reclen > n) break;
        u8* d = buf + off;
        memset(d, 0, reclen);
        u64 ino = std::hash<std::string>{}(name) | 1;
        s64 next = (s64)pos + 1;
        memcpy(d, &ino, 8);
        memcpy(d + 8, &next, 8);
        u16 rl = (u16)reclen;
        memcpy(d + 16, &rl, 2);
        d[18] = type;
        memcpy(d + 19, name.c_str(), name.size() + 1);
        off += reclen;
        pos++;
    }
    if (off == 0 && pos < list.size()) return -lnx::EINVAL_;
    return (int)off;
}

int HostFile::getdents64(Thread& t, u8* buf, u32 n) {
    if (!is_dir) return -lnx::ENOTDIR_;
    if (!dir_loaded) {
        dir_list.clear();
        int r = hfs::list_dir(host_path, dir_list);
        if (r < 0) return r;
        for (auto& x : extra_entries)
            if (std::none_of(dir_list.begin(), dir_list.end(), [&](auto& y) { return y.first == x; })) {
                hfs::Stat est;
                std::string h;
                bool isdir = find_override(path + (path == "/" ? "" : "/") + x, h) && hfs::stat(h, est) == 0 &&
                             (est.mode & lnx::L_S_IFMT) == lnx::L_S_IFDIR;
                dir_list.push_back({x, (u8)(isdir ? 4 : 8)});
            }
        dir_loaded = true;
    }
    return emit_dirents(*t.proc->mem, buf, n, dir_list, dir_pos);
}

// ---------------------------------------------------------------- MemFile / VirtDir
s64 MemFile::read(Thread& t, u8* buf, u32 n) {
    s64 r = pread(t, buf, n, pos);
    if (r > 0) pos += r;
    return r;
}
s64 MemFile::pread(Thread&, u8* buf, u32 n, s64 off) {
    if (off >= (s64)data.size()) return 0;
    u32 c = (u32)std::min<s64>(n, (s64)data.size() - off);
    memcpy(buf, data.data() + off, c);
    return c;
}
s64 MemFile::write(Thread&, const u8* buf, u32 n) {
    std::string s((const char*)buf, n);
    if (on_write) on_write(s);
    return n;
}
s64 MemFile::lseek(s64 off, int whence) {
    s64 np = whence == 0 ? off : whence == 1 ? pos + off : (s64)data.size() + off;
    if (np < 0) return -lnx::EINVAL_;
    pos = np;
    return np;
}
int MemFile::fstat(LinuxStat64& st) {
    st = make_stat(mode, data.size());
    return 0;
}
int VirtDir::fstat(LinuxStat64& st) {
    st = make_stat(lnx::L_S_IFDIR | 0755, 0);
    return 0;
}
int VirtDir::getdents64(Thread& t, u8* buf, u32 n) { return emit_dirents(*t.proc->mem, buf, n, entries, pos); }

// ---------------------------------------------------------------- namespace
namespace vfs {

const std::string& rootfs() { return g_rootfs; }
void add_override(const std::string& guest, const std::string& host) { g_overrides[guest] = host; }
void add_conditional_override(const std::string& guest, const std::string& host, std::function<bool()> active) {
    g_cond_overrides[guest] = {host, std::move(active)};
}

void init(const std::string& root, const std::string& datadir, const std::string& rundir) {
    g_rootfs = root;
    g_mounts = {
        {"/", root, false, false},
        {"/var", rundir + "/var", true, false},
        {"/var/lib", datadir, true, false},
        {"/tmp", rundir + "/tmp", true, false},
        {"/run", rundir + "/run", true, false},
        {"/dev", "", false, true},
        {"/dev/shm", rundir + "/shm", true, false},
        {"/proc", "", false, true},
        {"/sys", rundir + "/sys", true, false},
    };
}

bool is_virtual_path(const std::string& g) {
    const Mount* m = find_mount(g);
    return m && m->virt;
}

std::string join(const std::string& cwd, const std::string& path) {
    if (!path.empty() && path[0] == '/') return path;
    if (cwd == "/") return "/" + path;
    return cwd + "/" + path;
}

static std::string dirfd_path(Process& p, int dirfd, int* err) {
    if (dirfd == lnx::L_AT_FDCWD) return p.cwd;
    FilePtr f = p.fds->get(dirfd);
    if (!f) {
        *err = lnx::EBADF_;
        return "";
    }
    return f->path;
}

// virtual symlinks in /proc and /dev
static bool virt_readlink(Process& p, const std::string& g, std::string& out) {
    if (g == "/proc/self") { out = "/proc/" + std::to_string(p.pid); return true; }
    if (g == "/proc/thread-self") { out = "/proc/" + std::to_string(p.pid); return true; }
    if (g == "/dev/stdin") { out = "/proc/self/fd/0"; return true; }
    if (g == "/dev/stdout") { out = "/proc/self/fd/1"; return true; }
    if (g == "/dev/stderr") { out = "/proc/self/fd/2"; return true; }
    if (g == "/dev/fd") { out = "/proc/self/fd"; return true; }
    if (g.compare(0, 6, "/proc/") == 0) {
        return readlink_proc_p(p, g, out) == 0;
    }
    return false;
}

Resolved resolve(Process& p, const std::string& path, bool follow_last, int dirfd) {
    Resolved r;
    if (path.empty()) {
        r.err = lnx::ENOENT_;
        return r;
    }
    std::string start;
    if (path[0] == '/') start = path;
    else {
        int err = 0;
        std::string base = dirfd_path(p, dirfd, &err);
        if (err) {
            r.err = err;
            return r;
        }
        start = join(base, path);
    }
    std::vector<std::string> todo = split(start);
    std::reverse(todo.begin(), todo.end());  // stack: back = next component
    std::vector<std::string> done;
    int links = 0;
    while (!todo.empty()) {
        std::string c = todo.back();
        todo.pop_back();
        if (c == ".") continue;
        if (c == "..") {
            if (!done.empty()) done.pop_back();
            continue;
        }
        done.push_back(c);
        bool last = todo.empty();
        if (last && !follow_last) break;
        std::string g = joinv(done);
        const Mount* m = find_mount(g);
        std::string target;
        bool is_link = false;
        std::string ovh;
        bool ov = find_override(g, ovh);
        if (m->virt && !ov) {
            is_link = virt_readlink(p, g, target);
        } else {
            std::string h = ov ? ovh : host_of(m, g);
            hfs::Stat st;
            if (hfs::lstat(h, st) != 0) {
                if (!last) {
                    r.err = lnx::ENOENT_;
                    // still return canonical for creation errors
                    return r;
                }
                break;
            }
            u32 type = st.mode & lnx::L_S_IFMT;
            if (type == lnx::L_S_IFLNK) {
                if (hfs::readlink(h, target) == 0 && !target.empty()) is_link = true;
            } else if (!last && type != lnx::L_S_IFDIR) {
                r.err = lnx::ENOTDIR_;
                return r;
            }
        }
        if (is_link) {
            if (++links > 40) {
                r.err = lnx::ELOOP_;
                return r;
            }
            done.pop_back();
            if (!target.empty() && target[0] == '/') done.clear();
            auto tc = split(target);
            for (auto it = tc.rbegin(); it != tc.rend(); ++it) todo.push_back(*it);
        }
    }
    r.guest = joinv(done);
    std::string ovh;
    if (find_override(r.guest, ovh)) {
        r.kind = Resolved::HOST;
        r.host = ovh;
        r.writable = false;
        return r;
    }
    const Mount* m = find_mount(r.guest);
    if (m->virt) {
        r.kind = Resolved::VIRT;
    } else {
        r.kind = Resolved::HOST;
        r.host = host_of(m, r.guest);
        r.writable = m->writable;
    }
    return r;
}

FilePtr open(Thread& t, const std::string& path, u32 flags, u32 mode, int* err, int dirfd) {
    Process& p = *t.proc;
    bool follow = !(flags & lnx::L_O_NOFOLLOW) || true;
    Resolved r = resolve(p, path, follow, dirfd);
    if (r.err) {
        *err = r.err;
        return nullptr;
    }
    if (r.kind == Resolved::VIRT) {
        FilePtr f;
        if (r.guest.compare(0, 5, "/proc") == 0) f = open_proc(t, r.guest, flags, err);
        else f = open_device(t, r.guest, flags, err);
        if (f) {
            f->path = r.guest;
            f->flags = flags & (lnx::L_O_ACCMODE | lnx::L_O_NONBLOCK | lnx::L_O_APPEND);
        }
        return f;
    }
    u32 acc = flags & 3;
    hfs::Stat st;
    bool exists = hfs::stat(r.host, st) == 0;
    u32 type = exists ? (st.mode & lnx::L_S_IFMT) : 0;
    if (!r.writable && (acc != 0 || (flags & lnx::L_O_TRUNC) || ((flags & lnx::L_O_CREAT) && !exists))) {
        *err = exists ? lnx::EROFS_ : lnx::EROFS_;
        if (!exists && !(flags & lnx::L_O_CREAT)) *err = lnx::ENOENT_;
        return nullptr;
    }
    if (exists && type == lnx::L_S_IFDIR) {
        if (acc != 0) {
            *err = lnx::EISDIR_;
            return nullptr;
        }
        auto f = std::make_shared<HostFile>();
        f->fd = hfs::open(r.host, lnx::L_O_DIRECTORY, 0);
        if (f->fd < 0) {
            *err = -f->fd;
            f->fd = -1;
            return nullptr;
        }
        f->is_dir = true;
        f->host_path = r.host;
        // overridden children (case-colliding entries) of this directory
        for (auto& [k, v] : g_overrides) {
            size_t sl = k.rfind('/');
            if (sl != std::string::npos && k.substr(0, sl ? sl : 1) == r.guest) f->extra_entries.push_back(k.substr(sl + 1));
        }
        f->path = r.guest;
        f->flags = flags & (lnx::L_O_ACCMODE | lnx::L_O_NONBLOCK);
        return f;
    }
    if ((flags & lnx::L_O_DIRECTORY) && exists) {
        *err = lnx::ENOTDIR_;
        return nullptr;
    }
    if (exists && type == lnx::L_S_IFSOCK) {
        *err = lnx::ENXIO_;
        return nullptr;
    }
    if (exists && type == lnx::L_S_IFIFO) {  // named pipe: an in-emulator pipe keyed by its host path
        FilePtr f = ipc_open_fifo(t, r.host, flags, err);
        if (f) f->path = r.guest;
        return f;
    }
    u32 hflags = flags & (lnx::L_O_ACCMODE | lnx::L_O_CREAT | lnx::L_O_EXCL | lnx::L_O_TRUNC | lnx::L_O_NOFOLLOW | lnx::L_O_NOCTTY);
    int fd = hfs::open(r.host, hflags, (mode & ~p.umask) & 07777);
    if (fd < 0) {
        *err = -fd;
        return nullptr;
    }
    auto f = std::make_shared<HostFile>();
    f->fd = fd;
    f->host_path = r.host;
    f->path = r.guest;
    f->flags = flags & (lnx::L_O_ACCMODE | lnx::L_O_NONBLOCK | lnx::L_O_APPEND);
    LOGC(LOG_VFS, "[vfs] open %s -> %s\n", path.c_str(), r.host.c_str());
    return f;
}

int stat(Thread& t, const std::string& path, LinuxStat64& st, bool follow, int dirfd) {
    Resolved r = resolve(*t.proc, path, follow, dirfd);
    if (r.err) return -r.err;
    if (r.kind == Resolved::VIRT) {
        if (r.guest.compare(0, 5, "/proc") == 0) return stat_proc(t, r.guest, st);
        if (device_exists(r.guest, &st)) return 0;
        // /proc/self etc. as symlink when not following
        std::string tgt;
        if (!follow && virt_readlink(*t.proc, r.guest, tgt)) {
            st = make_stat(lnx::L_S_IFLNK | 0777, tgt.size());
            return 0;
        }
        return -lnx::ENOENT_;
    }
    hfs::Stat hs;
    int rc = follow ? hfs::stat(r.host, hs) : hfs::lstat(r.host, hs);
    if (rc != 0) return rc;
    host_stat_to_linux(hs, st);
    return 0;
}

int readlink(Thread& t, const std::string& path, std::string& out, int dirfd) {
    Resolved r = resolve(*t.proc, path, false, dirfd);
    if (r.err) return -r.err;
    if (r.kind == Resolved::VIRT) {
        if (virt_readlink(*t.proc, r.guest, out)) return 0;
        return -lnx::EINVAL_;
    }
    return hfs::readlink(r.host, out);
}

bool read_file(Process& p, const std::string& path, std::vector<u8>& out, std::string* canonical) {
    Resolved r = resolve(p, path, true);
    if (r.err || r.kind != Resolved::HOST) {
        // virtual stub libraries are served from the HLE layer
        return false;
    }
    int fd = hfs::open(r.host, 0, 0);
    if (fd < 0) return false;
    hfs::Stat st;
    if (hfs::fstat(fd, st) != 0 || (st.mode & lnx::L_S_IFMT) == lnx::L_S_IFDIR) {
        hfs::close(fd);
        return false;
    }
    out.resize((size_t)st.size);
    size_t got = 0;
    while (got < out.size()) {
        s64 n = hfs::pread(fd, out.data() + got, (u32)std::min<size_t>(out.size() - got, 1u << 24), (s64)got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    hfs::close(fd);
    out.resize(got);
    if (canonical) *canonical = r.guest;
    return true;
}

}  // namespace vfs
