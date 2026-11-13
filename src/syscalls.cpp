// Linux ARM EABI syscall implementation (part 1: dispatcher, files, processes, signals, time).
#include "kernel.h"
#include "syscalls.h"
#include <algorithm>

extern std::mutex g_sig_mtx;
static inline u64 sigbit(int s) { return 1ull << (s - 1); }

std::vector<std::string> read_str_array(AddressSpace& m, u32 addr) {
    std::vector<std::string> v;
    if (!addr) return v;
    for (int i = 0; i < 100000; i++) {
        u32 p = m.rd<u32>(addr + i * 4);
        if (!p) break;
        v.push_back(m.read_str(p));
    }
    return v;
}

static int fd_install(Thread& t, FilePtr f, bool cloexec) { return t.proc->fds->install(std::move(f), cloexec); }

// ---------------------------------------------------------------- file syscalls
static s32 sys_openat(Thread& t, int dirfd, u32 pathp, u32 flags, u32 mode) {
    std::string path = t.proc->mem->read_str(pathp);
    int err = 0;
    FilePtr f = vfs::open(t, path, flags, mode, &err, dirfd);
    LOGC(LOG_VFS, "[vfs] pid %d open(%s, %o) -> %s\n", t.proc->pid, path.c_str(), flags, f ? "ok" : "err");
    if (!f) return -err;
    return fd_install(t, f, flags & lnx::L_O_CLOEXEC);
}

static s32 sys_read(Thread& t, int fd, u32 buf, u32 n) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    if ((f->flags & 3) == 1) return -lnx::EBADF_;
    return (s32)f->read(t, t.proc->mem->ptr(buf), n);
}
static s32 sys_write(Thread& t, int fd, u32 buf, u32 n) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    if ((f->flags & 3) == 0 && !f->is_socket()) return -lnx::EBADF_;
    return (s32)f->write(t, t.proc->mem->ptr(buf), n);
}
static s32 sys_readv(Thread& t, int fd, u32 iov, int cnt, bool write) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    AddressSpace& m = *t.proc->mem;
    s64 total = 0;
    for (int i = 0; i < cnt; i++) {
        u32 b = m.rd<u32>(iov + i * 8), l = m.rd<u32>(iov + i * 8 + 4);
        if (!l) continue;
        s64 r = write ? f->write(t, m.ptr(b), l) : f->read(t, m.ptr(b), l);
        if (r < 0) return total ? (s32)total : (s32)r;
        total += r;
        if ((u32)r < l) break;
    }
    return (s32)total;
}

static s32 do_stat(Thread& t, u32 pathp, u32 statp, bool follow, int dirfd = lnx::L_AT_FDCWD, u32 flags = 0) {
    AddressSpace& m = *t.proc->mem;
    std::string path = m.read_str(pathp);
    LinuxStat64 st;
    int r;
    if (path.empty() && (flags & lnx::L_AT_EMPTY_PATH)) {
        FilePtr f = t.proc->fds->get(dirfd);
        if (!f) return -lnx::EBADF_;
        r = f->fstat(st);
    } else r = vfs::stat(t, path, st, follow, dirfd);
    LOGC(LOG_VFS, "[vfs] pid %d stat(%s) -> %d size %lld mode %o\n", t.proc->pid, path.c_str(), r, r ? 0LL : (long long)st.st_size,
         r ? 0u : (unsigned)st.st_mode);
    if (r) return r;
    m.write_bytes(statp, &st, sizeof st);
    return 0;
}

static s32 sys_fstat64(Thread& t, int fd, u32 statp) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    LinuxStat64 st;
    int r = f->fstat(st);
    if (r) return r;
    t.proc->mem->write_bytes(statp, &st, sizeof st);
    return 0;
}

// resolve path for modification (create/unlink etc.); returns host path or error
static int resolve_mod(Thread& t, int dirfd, u32 pathp, std::string& host, bool follow = false, std::string* guest = nullptr) {
    std::string path = t.proc->mem->read_str(pathp);
    Resolved r = vfs::resolve(*t.proc, path, follow, dirfd);
    if (r.err) return -r.err;
    if (r.kind != Resolved::HOST) return -lnx::EPERM_;
    if (!r.writable) return -lnx::EROFS_;
    host = r.host;
    if (guest) *guest = r.guest;
    return 0;
}

static s32 sys_faccessat(Thread& t, int dirfd, u32 pathp, int mode) {
    std::string path = t.proc->mem->read_str(pathp);
    Resolved r = vfs::resolve(*t.proc, path, true, dirfd);
    if (r.err) return -r.err;
    if (r.kind == Resolved::VIRT) {
        LinuxStat64 st;
        int e = vfs::stat(t, path, st, true, dirfd);
        return e;
    }
    hfs::Stat st;
    if (int e = hfs::stat(r.host, st)) return e;
    if ((mode & 2) && !r.writable) return -lnx::EROFS_;
    if ((mode & 1) && !(st.mode & 0111) && (st.mode & lnx::L_S_IFMT) != lnx::L_S_IFDIR) return -lnx::EACCES_;
    return 0;
}

static s32 sys_getdents64(Thread& t, int fd, u32 buf, u32 n) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    return f->getdents64(t, t.proc->mem->ptr(buf), n);
}

// old getdents (linux_dirent, 32-bit)
static s32 sys_getdents(Thread& t, int fd, u32 buf, u32 n) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    std::vector<u8> tmp(n);
    // Convert from dirent64 records (which are larger) — read with half the buffer to be safe
    int r = f->getdents64(t, tmp.data(), n / 2 + 32 > n ? n : n / 2 + 32);
    if (r <= 0) return r;
    AddressSpace& m = *t.proc->mem;
    u32 in = 0, out = 0;
    while (in < (u32)r) {
        u8* d = tmp.data() + in;
        u64 ino; s64 off; u16 rl; u8 type;
        memcpy(&ino, d, 8); memcpy(&off, d + 8, 8); memcpy(&rl, d + 16, 2); type = d[18];
        const char* name = (const char*)d + 19;
        size_t nl = strlen(name);
        u32 nrl = (u32)((10 + nl + 2 + 3) & ~3u);
        if (out + nrl > n) break;
        m.wr<u32>(buf + out, (u32)ino);
        m.wr<u32>(buf + out + 4, (u32)off);
        m.wr<u16>(buf + out + 8, (u16)nrl);
        m.write_bytes(buf + out + 10, name, nl + 1);
        m.wr<u8>(buf + out + nrl - 1, type);
        out += nrl;
        in += rl;
    }
    return out;
}

static s32 sys_fcntl(Thread& t, int fd, int cmd, u32 arg) {
    auto& tbl = *t.proc->fds;
    FilePtr f = tbl.get(fd);
    if (!f) return -lnx::EBADF_;
    switch (cmd) {
    case 0: return tbl.install(f, false, arg);          // F_DUPFD
    case 1030: return tbl.install(f, true, arg);        // F_DUPFD_CLOEXEC
    case 1: {  // F_GETFD
        std::lock_guard lk(tbl.mtx);
        return tbl.fds[fd].cloexec ? 1 : 0;
    }
    case 2: {  // F_SETFD
        std::lock_guard lk(tbl.mtx);
        tbl.fds[fd].cloexec = arg & 1;
        return 0;
    }
    case 3: return f->flags | lnx::L_O_LARGEFILE;  // F_GETFL
    case 4:  // F_SETFL
        f->flags = (f->flags & ~(lnx::L_O_NONBLOCK | lnx::L_O_APPEND)) | (arg & (lnx::L_O_NONBLOCK | lnx::L_O_APPEND));
        io_notify();
        return 0;
    case 5: case 12: {  // F_GETLK / F_GETLK64: report unlocked
        // struct flock64: short l_type ...
        t.proc->mem->wr<s16>(arg, 2 /*F_UNLCK*/);
        return 0;
    }
    case 6: case 7: case 13: case 14: return 0;  // set locks: always succeed
    case 8: case 9: case 10: case 11: return 0;   // F_SETOWN/GETOWN/SETSIG/GETSIG
    case 1031: case 1032: return 0;  // F_SETPIPE_SZ
    }
    LOGI("[sys] fcntl cmd %d unsupported\n", cmd);
    return -lnx::EINVAL_;
}

static s32 sys_pipe2(Thread& t, u32 fdsp, u32 flags) {
    FilePtr r, w;
    ipc_make_pipe(r, w, flags & lnx::L_O_NONBLOCK);
    bool ce = flags & lnx::L_O_CLOEXEC;
    int a = fd_install(t, r, ce);
    if (a < 0) return a;
    int b = fd_install(t, w, ce);
    if (b < 0) {
        t.proc->fds->close(a);
        return b;
    }
    t.proc->mem->wr<s32>(fdsp, a);
    t.proc->mem->wr<s32>(fdsp + 4, b);
    return 0;
}

static s32 sys_dup3(Thread& t, int oldfd, int newfd, u32 flags) {
    FilePtr f = t.proc->fds->get(oldfd);
    if (!f) return -lnx::EBADF_;
    if (oldfd == newfd) return newfd;
    return t.proc->fds->install_at(newfd, f, flags & lnx::L_O_CLOEXEC);
}

static s32 sys_ioctl(Thread& t, int fd, u32 req, u32 arg) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) return -lnx::EBADF_;
    if (req == 0x5451) {  // FIOCLEX
        std::lock_guard lk(t.proc->fds->mtx);
        t.proc->fds->fds[fd].cloexec = true;
        return 0;
    }
    if (req == 0x5450) {  // FIONCLEX
        std::lock_guard lk(t.proc->fds->mtx);
        t.proc->fds->fds[fd].cloexec = false;
        return 0;
    }
    if (req == 0x5421) {  // FIONBIO
        int on = t.proc->mem->rd<s32>(arg);
        if (on) f->flags |= lnx::L_O_NONBLOCK; else f->flags &= ~lnx::L_O_NONBLOCK;
        return 0;
    }
    int r = f->ioctl(t, req, arg);
    LOGC(LOG_DEV, "[ioctl] pid %d fd %d (%s) req %08x arg %08x -> %d\n", t.proc->pid, fd, f->path.c_str(), req, arg, r);
    return r;
}

// sendfile(out_fd, in_fd, off*, count): copy through a buffer using the generic file layer.
static s32 sys_sendfile(Thread& t, int outfd, int infd, u32 offp, u32 count, bool off64) {
    FilePtr out = t.proc->fds->get(outfd), in = t.proc->fds->get(infd);
    if (!out || !in) return -lnx::EBADF_;
    AddressSpace& m = *t.proc->mem;
    s64 off = 0;
    if (offp) off = off64 ? m.rd<s64>(offp) : (s64)m.rd<s32>(offp);
    std::vector<u8> buf(std::min<u32>(count, 1u << 20));
    u32 done = 0;
    while (done < count) {
        u32 chunk = std::min<u32>(count - done, (u32)buf.size());
        s64 r = offp ? in->pread(t, buf.data(), chunk, off) : in->read(t, buf.data(), chunk);
        if (r < 0) {
            if (!done) return (s32)r;
            break;
        }
        if (r == 0) break;
        s64 w = out->write(t, buf.data(), (u32)r);
        if (w < 0) {
            if (!done) return (s32)w;
            break;
        }
        done += (u32)w;
        if (offp) off += w;
        if (w < r) {
            if (!offp) in->lseek(w - r, 1);  // un-read the part that wasn't written
            break;
        }
    }
    if (offp) {
        if (off64) m.wr<s64>(offp, off);
        else m.wr<s32>(offp, (s32)off);
    }
    return (s32)done;
}

static s32 sys_getcwd(Thread& t, u32 buf, u32 size) {
    const std::string& c = t.proc->cwd;
    if (c.size() + 1 > size) return -lnx::ERANGE_;
    t.proc->mem->write_bytes(buf, c.c_str(), c.size() + 1);
    return (s32)c.size() + 1;
}

static s32 sys_chdir(Thread& t, const std::string& path) {
    Resolved r = vfs::resolve(*t.proc, path, true);
    if (r.err) return -r.err;
    if (r.kind == Resolved::HOST) {
        hfs::Stat st;
        if (int e = hfs::stat(r.host, st)) return e;
        if ((st.mode & lnx::L_S_IFMT) != lnx::L_S_IFDIR) return -lnx::ENOTDIR_;
    }
    t.proc->cwd = r.guest;
    return 0;
}

static void write_statfs64(AddressSpace& m, u32 p, bool tmp) {
    // struct statfs64 (ARM EABI, packed to 4): 84 bytes
    u8 b[84] = {};
    u32 type = tmp ? 0x01021994 : 0x73717368;  // TMPFS_MAGIC / SQUASHFS_MAGIC
    u32 bsize = 4096;
    u64 blocks = 65536, bfree = tmp ? 60000 : 0, files = 65536, ffree = tmp ? 60000 : 0;
    memcpy(b + 0, &type, 4);
    memcpy(b + 4, &bsize, 4);
    memcpy(b + 8, &blocks, 8);
    memcpy(b + 16, &bfree, 8);
    memcpy(b + 24, &bfree, 8);
    memcpy(b + 32, &files, 8);
    memcpy(b + 40, &ffree, 8);
    u32 namelen = 255, frsize = 4096;
    memcpy(b + 56, &namelen, 4);
    memcpy(b + 60, &frsize, 4);
    m.write_bytes(p, b, sizeof b);
}

// ---------------------------------------------------------------- process syscalls
static s32 sys_clone(Thread& t, u32 flags, u32 newsp, u32 ptid, u32 tls, u32 ctid) {
    Process& p = *t.proc;
    auto& R = t.R();
    if (flags & lnx::L_CLONE_THREAD) {
        // new thread in same process
        std::array<u32, 16> regs = R;
        regs[0] = 0;
        if (newsp) regs[13] = newsp;
        Thread* nt = create_thread(p.shared_from_this(), regs, t.jit->Cpsr(), t.jit->ExtRegs(), t.jit->Fpscr(),
                                   (flags & lnx::L_CLONE_SETTLS) ? tls : t.tls);
        {
            std::lock_guard lk(g_kernel.mtx);
            nt->tid = g_kernel.alloc_pid();
            g_kernel.threads[nt->tid] = nt;
        }
        nt->sigmask = t.sigmask;
        if (flags & lnx::L_CLONE_PARENT_SETTID) p.mem->wr<s32>(ptid, nt->tid);
        if (flags & lnx::L_CLONE_CHILD_SETTID) p.mem->wr<s32>(ctid, nt->tid);
        if (flags & lnx::L_CLONE_CHILD_CLEARTID) nt->clear_child_tid = ctid;
        LOGC(LOG_PROC, "[proc] pid %d new thread %d\n", p.pid, nt->tid);
        start_thread(nt);
        return nt->tid;
    }
    // new process (fork / vfork)
    auto c = std::make_shared<Process>();
    {
        std::lock_guard lk(g_kernel.mtx);
        c->pid = g_kernel.alloc_pid();
    }
    c->ppid = (flags & lnx::L_CLONE_PARENT) ? p.ppid : p.pid;
    c->pgid = p.pgid;
    c->sid = p.sid;
    c->cwd = p.cwd;
    c->umask = p.umask;
    c->exe = p.exe;
    c->argv = p.argv;
    c->envp = p.envp;
    c->comm = p.comm;
    c->rlimits = p.rlimits;
    c->start_time_ns = now_ns();
    bool vfork = (flags & lnx::L_CLONE_VM) && (flags & lnx::L_CLONE_VFORK);
    if (flags & lnx::L_CLONE_VM) {
        c->mem = p.mem;  // shared until exec (vfork)
    } else {
        c->mem = std::make_shared<AddressSpace>();
        p.suspend_others(&t);
        p.mem->copy_to(*c->mem);
        p.resume_others();
    }
    c->monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(MAX_CPUS);
    c->fds = (flags & lnx::L_CLONE_FILES) ? p.fds : p.fds->clone();
    if (flags & lnx::L_CLONE_SIGHAND) c->sig = p.sig;
    else {
        c->sig = std::make_shared<SigHandlers>();
        std::lock_guard lk(p.sig->mtx);
        c->sig->act = p.sig->act;
    }
    hle_process_fork(p, *c);
    std::array<u32, 16> regs = R;
    regs[0] = 0;
    if (newsp) regs[13] = newsp;
    Thread* nt = create_thread(c, regs, t.jit->Cpsr(), t.jit->ExtRegs(), t.jit->Fpscr(),
                               (flags & lnx::L_CLONE_SETTLS) ? tls : t.tls);
    nt->tid = c->pid;
    nt->sigmask = t.sigmask;
    {
        std::lock_guard lk(g_kernel.mtx);
        g_kernel.procs[c->pid] = c;
        g_kernel.threads[nt->tid] = nt;
    }
    if (flags & lnx::L_CLONE_PARENT_SETTID) p.mem->wr<s32>(ptid, c->pid);
    if (flags & lnx::L_CLONE_CHILD_SETTID) c->mem->wr<s32>(ctid, c->pid);
    if (flags & lnx::L_CLONE_CHILD_CLEARTID) nt->clear_child_tid = ctid;
    std::shared_ptr<std::atomic<bool>> done;
    if (vfork) {
        done = std::make_shared<std::atomic<bool>>(false);
        c->vfork_done = done;
    }
    LOGC(LOG_PROC, "[proc] pid %d (%s) %s -> child %d\n", p.pid, p.comm.c_str(), vfork ? "vfork" : "fork", c->pid);
    start_thread(nt);
    if (vfork) {
        std::unique_lock lk(g_kernel.mtx);
        while (!*done) g_kernel.child_cv.wait_for(lk, std::chrono::milliseconds(20));
    }
    return c->pid;
}

static bool wait_match(Thread& t, Process& c, int pid) {
    if (pid > 0) return c.pid == pid;
    if (pid == -1) return true;
    if (pid == 0) return c.pgid == t.proc->pgid;
    return c.pgid == -pid;
}

static s32 sys_wait4(Thread& t, int pid, u32 statusp, int options, u32 rusage) {
    Process& p = *t.proc;
    std::unique_lock lk(g_kernel.mtx);
    while (true) {
        bool have_child = false;
        for (auto it = g_kernel.procs.begin(); it != g_kernel.procs.end(); ++it) {
            auto& c = it->second;
            if (c->ppid != p.pid || !wait_match(t, *c, pid)) continue;
            have_child = true;
            if (c->zombie && !c->reaped) {
                int cpid = c->pid;
                int st = c->exit_status;
                c->reaped = true;
                g_kernel.procs.erase(it);
                lk.unlock();
                if (statusp) p.mem->wr<s32>(statusp, st);
                if (rusage) memset(p.mem->ptr(rusage), 0, 72);
                // clear SIGCHLD if no more zombies (approximation)
                LOGC(LOG_PROC, "[proc] pid %d reaped %d status 0x%x\n", p.pid, cpid, st);
                return cpid;
            }
        }
        if (!have_child) return -lnx::ECHILD_;
        if (options & 1) return 0;  // WNOHANG
        lk.unlock();
        if (t.signal_pending()) return -ERESTARTSYS;
        lk.lock();
        g_kernel.child_cv.wait_for(lk, std::chrono::milliseconds(20));
    }
}

static s32 sys_waitid(Thread& t, int idtype, int id, u32 infop, int options) {
    // P_ALL 0, P_PID 1, P_PGID 2. WEXITED 4, WNOHANG 1, WNOWAIT 0x01000000
    int pid = idtype == 0 ? -1 : idtype == 1 ? id : -id;
    Process& p = *t.proc;
    std::unique_lock lk(g_kernel.mtx);
    while (true) {
        bool have = false;
        for (auto it = g_kernel.procs.begin(); it != g_kernel.procs.end(); ++it) {
            auto& c = it->second;
            if (c->ppid != p.pid || !wait_match(t, *c, pid)) continue;
            have = true;
            if (c->zombie && !c->reaped) {
                int st = c->exit_status, cpid = c->pid;
                if (!(options & 0x01000000)) {
                    c->reaped = true;
                    g_kernel.procs.erase(it);
                }
                lk.unlock();
                if (infop) {
                    memset(p.mem->ptr(infop), 0, 128);
                    p.mem->wr<s32>(infop, lnx::L_SIGCHLD);
                    p.mem->wr<s32>(infop + 8, (st & 0x7f) ? 2 : 1);
                    p.mem->wr<s32>(infop + 12, cpid);
                    p.mem->wr<s32>(infop + 20, (st & 0x7f) ? (st & 0x7f) : ((st >> 8) & 0xff));
                }
                return 0;
            }
        }
        if (!have) return -lnx::ECHILD_;
        if (options & 1) {
            if (infop) memset(p.mem->ptr(infop), 0, 128);
            return 0;
        }
        lk.unlock();
        if (t.signal_pending()) return -ERESTARTSYS;
        lk.lock();
        g_kernel.child_cv.wait_for(lk, std::chrono::milliseconds(20));
    }
}

static s32 sys_kill(Thread& t, int pid, int sig) {
    if (sig < 0 || sig > 64) return -lnx::EINVAL_;
    SigInfo si;
    si.signo = sig;
    si.pid = t.proc->pid;
    si.code = 0;  // SI_USER
    std::vector<std::shared_ptr<Process>> targets;
    {
        std::lock_guard lk(g_kernel.mtx);
        for (auto& [id, p] : g_kernel.procs) {
            if (p->zombie) continue;
            if (pid > 0 ? p->pid == pid
                        : pid == 0 ? p->pgid == t.proc->pgid
                                   : pid == -1 ? (p->pid != 1 && p->pid != t.proc->pid) : p->pgid == -pid)
                targets.push_back(p);
        }
    }
    if (targets.empty()) return -lnx::ESRCH_;
    LOGC(LOG_SIGNAL, "[sig] pid %d kill(%d, %d) -> %zu targets\n", t.proc->pid, pid, sig, targets.size());
    if (sig == 0) return 0;
    for (auto& p : targets) send_signal_proc(*p, si);
    return 0;
}

static s32 sys_tgkill(Thread& t, int tgid, int tid, int sig) {
    Thread* tt;
    {
        std::lock_guard lk(g_kernel.mtx);
        tt = g_kernel.find_thread(tid);
    }
    if (!tt || (tgid > 0 && tt->proc->pid != tgid)) return -lnx::ESRCH_;
    if (sig == 0) return 0;
    SigInfo si;
    si.signo = sig;
    si.pid = t.proc->pid;
    si.code = -6;  // SI_TKILL
    send_signal_thread(*tt, si);
    return 0;
}

static s32 sys_rt_sigaction(Thread& t, int sig, u32 actp, u32 oactp) {
    if (sig < 1 || sig > 64 || sig == lnx::L_SIGKILL || sig == lnx::L_SIGSTOP) {
        if (actp || sig < 1 || sig > 64) return -lnx::EINVAL_;
    }
    AddressSpace& m = *t.proc->mem;
    std::lock_guard lk(t.proc->sig->mtx);
    SigAction& a = t.proc->sig->act[sig];
    if (oactp) {
        m.wr<u32>(oactp, a.handler);
        m.wr<u32>(oactp + 4, a.flags);
        m.wr<u32>(oactp + 8, a.restorer);
        m.wr<u64>(oactp + 12, a.mask);
    }
    if (actp) {
        a.handler = m.rd<u32>(actp);
        a.flags = m.rd<u32>(actp + 4);
        a.restorer = m.rd<u32>(actp + 8);
        a.mask = m.rd<u64>(actp + 12);
        // Setting SIG_IGN discards pending
        if (a.handler == 1) {
            std::lock_guard lk2(g_sig_mtx);
            t.proc->pending &= ~sigbit(sig);
        }
    }
    return 0;
}

static s32 sys_rt_sigprocmask(Thread& t, int how, u32 setp, u32 osetp) {
    AddressSpace& m = *t.proc->mem;
    std::lock_guard lk(g_sig_mtx);
    if (osetp) m.wr<u64>(osetp, t.sigmask);
    if (setp) {
        u64 s = m.rd<u64>(setp);
        if (how == 0) t.sigmask |= s;
        else if (how == 1) t.sigmask &= ~s;
        else if (how == 2) t.sigmask = s;
        else return -lnx::EINVAL_;
        t.sigmask &= ~(sigbit(lnx::L_SIGKILL) | sigbit(lnx::L_SIGSTOP));
    }
    return 0;
}

static s32 sys_rt_sigsuspend(Thread& t, u32 maskp) {
    u64 mask = t.proc->mem->rd<u64>(maskp);
    {
        std::lock_guard lk(g_sig_mtx);
        t.saved_sigmask = t.sigmask;
        t.restore_sigmask = true;
        t.sigmask = mask & ~(sigbit(lnx::L_SIGKILL) | sigbit(lnx::L_SIGSTOP));
    }
    while (!t.signal_pending()) {
        std::unique_lock lk(t.wmtx);
        t.interrupted = false;
        t.wcv.wait_for(lk, std::chrono::milliseconds(50), [&] { return t.interrupted.load(); });
    }
    return -lnx::EINTR_;
}

static s32 sys_rt_sigtimedwait(Thread& t, u32 setp, u32 infop, u32 tsp) {
    AddressSpace& m = *t.proc->mem;
    u64 set = m.rd<u64>(setp);
    u64 deadline = tsp ? now_ns() + (u64)m.rd<u32>(tsp) * 1000000000ull + m.rd<u32>(tsp + 4) : ~0ull;
    while (true) {
        {
            std::lock_guard lk(g_sig_mtx);
            u64 avail = (t.pending | t.proc->pending) & set;
            if (avail) {
                int s = ctz64(avail) + 1;
                SigInfo info;
                info.signo = s;
                auto take = [&](u64& pend, std::deque<SigInfo>& q) {
                    for (auto it = q.begin(); it != q.end(); ++it)
                        if (it->signo == s) { info = *it; q.erase(it); break; }
                    if (std::none_of(q.begin(), q.end(), [&](auto& x) { return x.signo == s; })) pend &= ~sigbit(s);
                };
                if (t.pending & sigbit(s)) take(t.pending, t.pending_info);
                else take(t.proc->pending, t.proc->pending_info);
                if (infop) {
                    memset(m.ptr(infop), 0, 128);
                    m.wr<s32>(infop, s);
                    m.wr<s32>(infop + 8, info.code);
                    m.wr<s32>(infop + 12, info.pid);
                    m.wr<s32>(infop + 20, info.status);
                }
                return s;
            }
        }
        if (t.signal_pending()) return -lnx::EINTR_;
        u64 n = now_ns();
        if (n >= deadline) return -lnx::EAGAIN_;
        std::unique_lock lk(t.wmtx);
        t.interrupted = false;
        t.wcv.wait_for(lk, std::chrono::nanoseconds(std::min<u64>(deadline - n, 20000000ull)), [&] { return t.interrupted.load(); });
    }
}

// ---------------------------------------------------------------- time
static void put_timespec(AddressSpace& m, u32 p, u64 ns) {
    m.wr<u32>(p, (u32)(ns / 1000000000ull));
    m.wr<u32>(p + 4, (u32)(ns % 1000000000ull));
}
static u64 clock_value(int clk) {
    switch (clk) {
    case 0: case 5: return realtime_ns();
    case 2: case 3: return now_ns() - g_kernel.boot_time_ns;
    default: return now_ns() - g_kernel.boot_time_ns + 10ull * 1000000000ull;
    }
}

static s32 sys_clock_nanosleep(Thread& t, int clk, int flags, u32 reqp, u32 remp) {
    AddressSpace& m = *t.proc->mem;
    u64 req = (u64)m.rd<u32>(reqp) * 1000000000ull + m.rd<u32>(reqp + 4);
    u64 ns = req;
    if (flags & 1) {
        u64 now = clock_value(clk);
        ns = req > now ? req - now : 0;
    }
    u64 rem = 0;
    int r = sleep_ns(t, ns, &rem);
    if (r == -lnx::EINTR_) {
        if (remp && !(flags & 1)) put_timespec(m, remp, rem);
        return -lnx::EINTR_;
    }
    return 0;
}

// itimer support: a background thread fires SIGALRM
static std::once_flag g_timer_once;
static void timer_thread() {
    plat::set_thread_name("itimer");
    while (true) {
        plat::usleep_us(2000);
        std::vector<std::shared_ptr<Process>> fire;
        u64 n = now_ns();
        {
            std::lock_guard lk(g_kernel.mtx);
            for (auto& [pid, p] : g_kernel.procs) {
                if (p->zombie || !p->alarm_deadline_ns) continue;
                if (n >= p->alarm_deadline_ns) {
                    fire.push_back(p);
                    p->alarm_deadline_ns = p->alarm_interval_ns ? n + p->alarm_interval_ns : 0;
                }
            }
        }
        for (auto& p : fire) {
            SigInfo si;
            si.signo = lnx::L_SIGALRM;
            si.code = 0x80;  // SI_KERNEL
            send_signal_proc(*p, si);
        }
    }
}

static s32 sys_setitimer(Thread& t, int which, u32 newp, u32 oldp) {
    AddressSpace& m = *t.proc->mem;
    Process& p = *t.proc;
    if (which != 0) return 0;
    std::call_once(g_timer_once, [] { std::thread(timer_thread).detach(); });
    std::lock_guard lk(g_kernel.mtx);
    u64 n = now_ns();
    if (oldp) {
        u64 rem = p.alarm_deadline_ns > n ? p.alarm_deadline_ns - n : 0;
        m.wr<u32>(oldp, (u32)(p.alarm_interval_ns / 1000000000ull));
        m.wr<u32>(oldp + 4, (u32)((p.alarm_interval_ns / 1000) % 1000000));
        m.wr<u32>(oldp + 8, (u32)(rem / 1000000000ull));
        m.wr<u32>(oldp + 12, (u32)((rem / 1000) % 1000000));
    }
    if (newp) {
        u64 interval = (u64)m.rd<u32>(newp) * 1000000000ull + (u64)m.rd<u32>(newp + 4) * 1000ull;
        u64 value = (u64)m.rd<u32>(newp + 8) * 1000000000ull + (u64)m.rd<u32>(newp + 12) * 1000ull;
        p.alarm_interval_ns = interval;
        p.alarm_deadline_ns = value ? n + value : 0;
    }
    return 0;
}

// ---------------------------------------------------------------- misc
static s32 sys_uname(Thread& t, u32 p) {
    AddressSpace& m = *t.proc->mem;
    char u[6][65] = {};
    strcpy(u[0], "Linux");
    strncpy(u[1], g_kernel.hostname.c_str(), 64);
    strcpy(u[2], "3.4.113");
    strcpy(u[3], "#1 SMP PREEMPT Mon Jul 24 2017");
    strcpy(u[4], "armv7l");
    strcpy(u[5], "(none)");
    m.write_bytes(p, u, sizeof u);
    return 0;
}

static s32 sys_sysinfo(Thread& t, u32 p) {
    AddressSpace& m = *t.proc->mem;
    u8 b[64] = {};
    u32 uptime = (u32)((now_ns() - g_kernel.boot_time_ns) / 1000000000ull) + 10;
    u32 total = 256u << 20, freem = 160u << 20;
    memcpy(b, &uptime, 4);
    memcpy(b + 16, &total, 4);
    memcpy(b + 20, &freem, 4);
    u16 procs = (u16)g_kernel.procs.size();
    memcpy(b + 40, &procs, 2);
    u32 unit = 1;
    memcpy(b + 52, &unit, 4);
    m.write_bytes(p, b, sizeof b);
    return 0;
}

static s32 sys_prctl(Thread& t, int option, u32 a2) {
    AddressSpace& m = *t.proc->mem;
    switch (option) {
    case 15: t.proc->comm = m.read_str(a2, 16); return 0;  // PR_SET_NAME
    case 16: {
        std::string c = t.proc->comm.substr(0, 15);
        m.write_bytes(a2, c.c_str(), c.size() + 1);
        return 0;
    }
    case 1: case 2: case 3: case 4: case 8: case 36: case 38: return 0;
    }
    return -lnx::EINVAL_;
}

static s32 sys_reboot(Thread& t, u32 magic1, u32 magic2, u32 cmd) {
    if (cmd != 0x4321FEDC && cmd != 0xCDEF0123 && cmd != 0x01234567) return 0;  // CAD on/off etc.
    LOGI("[kernel] reboot(cmd=%08x) requested by pid %d — powering off emulator\n", cmd, t.proc->pid);
    g_kernel.exit_code = 0;
    g_kernel.shutting_down = true;
    return 0;
}

static const char* syscall_name(u32 nr);

static std::pair<u64, u64> get_rlimit(Process& p, u32 res) {
    auto it = p.rlimits.find(res);
    if (it != p.rlimits.end()) return it->second;
    switch (res) {
    case 7: return {1024, 4096};        // NOFILE
    case 3: return {8u << 20, ~0ull};   // STACK
    case 4: return {0, ~0ull};          // CORE
    case 8: return {65536, 65536};      // MEMLOCK
    case 13: return {0, 0};             // NICE
    case 14: return {0, 0};             // RTPRIO
    }
    return {~0ull, ~0ull};
}

void do_syscall(Thread& t) {
    auto& R = t.R();
    u32 nr = R[7];
    u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
    t.sc_orig_r0 = a0;
    AddressSpace& m = *t.proc->mem;
    Process& p = *t.proc;
    s32 ret = -lnx::ENOSYS_;
    bool noret = false;
    switch (nr) {
    case 1:  // exit
        thread_exit(t, a0);
        {
            std::lock_guard lk(p.pmtx);
            int live = 0;
            for (Thread* x : p.threads)
                if (!x->is_host_call && !x->exit_req) live++;
            if (live == 0) {
                std::lock_guard lk2(g_kernel.mtx);
                if (!(p.exit_status & 0x10000)) p.exit_status = ((a0 & 0xff) << 8) | 0x10000;
            }
        }
        noret = true;
        break;
    case 248:  // exit_group
        process_exit(p, (a0 & 0xff) << 8, &t);
        noret = true;
        break;
    case 2: ret = sys_clone(t, lnx::L_SIGCHLD, 0, 0, 0, 0); break;                        // fork
    case 190: ret = sys_clone(t, lnx::L_CLONE_VM | lnx::L_CLONE_VFORK | lnx::L_SIGCHLD, 0, 0, 0, 0); break;  // vfork
    case 120: ret = sys_clone(t, a0, a1, a2, a3, a4); break;
    case 3: ret = sys_read(t, a0, a1, a2); break;
    case 4: ret = sys_write(t, a0, a1, a2); break;
    case 5: ret = sys_openat(t, lnx::L_AT_FDCWD, a0, a1, a2); break;
    case 322: ret = sys_openat(t, (s32)a0, a1, a2, a3); break;
    case 8: ret = sys_openat(t, lnx::L_AT_FDCWD, a0, lnx::L_O_CREAT | 1u | lnx::L_O_TRUNC, a1); break;
    case 6: ret = p.fds->close(a0); break;
    case 11: {  // execve
        std::string path = m.read_str(a0);
        auto argv = read_str_array(m, a1);
        auto envp = read_str_array(m, a2);
        LOGC(LOG_PROC, "[proc] pid %d execve %s\n", p.pid, path.c_str());
        ret = do_execve(t, path, argv, envp);
        if (ret == 0) noret = true;
        break;
    }
    case 12: ret = sys_chdir(t, m.read_str(a0)); break;
    case 133: {  // fchdir
        FilePtr f = p.fds->get(a0);
        ret = f ? sys_chdir(t, f->path) : -lnx::EBADF_;
        break;
    }
    case 10: { std::string h; ret = resolve_mod(t, lnx::L_AT_FDCWD, a0, h); if (!ret) ret = hfs::unlink(h); break; }
    case 328: {  // unlinkat
        std::string h;
        ret = resolve_mod(t, (s32)a0, a1, h);
        if (!ret) ret = (a2 & lnx::L_AT_REMOVEDIR) ? hfs::rmdir(h) : hfs::unlink(h);
        break;
    }
    case 14: case 324: {  // mknod / mknodat
        u32 pathp = nr == 14 ? a0 : a1, mode = nr == 14 ? a1 : a2;
        int dfd = nr == 14 ? lnx::L_AT_FDCWD : (s32)a0;
        std::string h;
        ret = resolve_mod(t, dfd, pathp, h);
        if (!ret) {
            if ((mode & lnx::L_S_IFMT) == lnx::L_S_IFIFO) ret = hfs::mkfifo(h, mode & 0777);
            else if ((mode & lnx::L_S_IFMT) == lnx::L_S_IFREG || (mode & lnx::L_S_IFMT) == 0) {
                int fd = hfs::open(h, lnx::L_O_CREAT | lnx::L_O_EXCL | 1u, mode & 0777);
                ret = fd < 0 ? fd : (hfs::close(fd), 0);
            } else ret = 0;  // device nodes: pretend
        }
        break;
    }
    case 15: case 94: case 306: ret = 0; break;  // chmod/fchmod/fchmodat (ignore)
    case 16: case 95: case 182: case 198: case 207: case 212: case 325: ret = 0; break;  // chown family
    case 19: {  // lseek
        FilePtr f = p.fds->get(a0);
        ret = f ? (s32)f->lseek((s32)a1, a2) : -lnx::EBADF_;
        break;
    }
    case 140: {  // _llseek(fd, hi, lo, result*, whence)
        FilePtr f = p.fds->get(a0);
        if (!f) { ret = -lnx::EBADF_; break; }
        s64 off = (s64)(((u64)a1 << 32) | a2);
        s64 r = f->lseek(off, a4);
        if (r < 0) ret = (s32)r;
        else { m.wr<s64>(a3, r); ret = 0; }
        break;
    }
    case 20: ret = p.pid; break;
    case 224: ret = t.tid; break;
    case 64: ret = p.ppid; break;
    case 21: case 52: case 22: ret = 0; break;  // mount / umount2
    case 23: case 46: case 213: case 214: case 70: case 71: case 203: case 204: case 164: case 170: case 208: case 210:
    case 138: case 139: case 215: case 216: case 81: case 206: ret = 0; break;  // set*id, setgroups
    case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: ret = 0; break;  // get*id
    case 165: case 171: case 209: case 211:  // getres*id
        m.wr<u32>(a0, 0); m.wr<u32>(a1, 0); m.wr<u32>(a2, 0); ret = 0; break;
    case 80: case 205: ret = 0; break;  // getgroups
    case 33: ret = sys_faccessat(t, lnx::L_AT_FDCWD, a0, a1); break;
    case 334: ret = sys_faccessat(t, (s32)a0, a1, a2); break;
    case 36: case 118: case 148: case 373: ret = 0; break;  // sync/fsync/fdatasync/syncfs
    case 37: ret = sys_kill(t, (s32)a0, (s32)a1); break;
    case 238: ret = sys_tgkill(t, -1, a0, a1); break;
    case 268: ret = sys_tgkill(t, (s32)a0, a1, a2); break;
    case 38: case 329: {  // rename / renameat
        u32 op = nr == 38 ? a0 : a1, np = nr == 38 ? a1 : a3;
        int od = nr == 38 ? lnx::L_AT_FDCWD : (s32)a0, nd = nr == 38 ? lnx::L_AT_FDCWD : (s32)a2;
        std::string ho, hn;
        ret = resolve_mod(t, od, op, ho);
        if (!ret) ret = resolve_mod(t, nd, np, hn);
        if (!ret) ret = hfs::rename(ho, hn);
        break;
    }
    case 39: case 323: {  // mkdir / mkdirat
        u32 pp = nr == 39 ? a0 : a1, mode = nr == 39 ? a1 : a2;
        int d = nr == 39 ? lnx::L_AT_FDCWD : (s32)a0;
        std::string h;
        std::string path = m.read_str(pp);
        Resolved r = vfs::resolve(p, path, false, d);
        if (r.err) ret = -r.err;
        else if (r.kind != Resolved::HOST) ret = -lnx::EEXIST_;
        else if (!r.writable) ret = hfs::exists(r.host) ? -lnx::EEXIST_ : -lnx::EROFS_;
        else ret = hfs::mkdir(r.host, mode & 0777);
        break;
    }
    case 40: { std::string h; ret = resolve_mod(t, lnx::L_AT_FDCWD, a0, h); if (!ret) ret = hfs::rmdir(h); break; }
    case 41: { FilePtr f = p.fds->get(a0); ret = f ? p.fds->install(f, false) : -lnx::EBADF_; break; }
    case 63: { FilePtr f = p.fds->get(a0); ret = !f ? -lnx::EBADF_ : (a0 == a1 ? (s32)a1 : p.fds->install_at(a1, f, false)); break; }
    case 358: ret = sys_dup3(t, a0, a1, a2); break;
    case 42: ret = sys_pipe2(t, a0, 0); break;
    case 359: ret = sys_pipe2(t, a0, a1); break;
    case 43: {  // times
        if (a0) memset(m.ptr(a0), 0, 16);
        ret = (s32)((now_ns() - g_kernel.boot_time_ns) / 10000000ull);
        break;
    }
    case 45: {  // brk
        AddressSpace& as = m;
        if (a0 == 0 || a0 < as.brk_start) { ret = as.brk_cur; break; }
        u32 oldp = (as.brk_cur + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        u32 newp = (a0 + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        if (newp > oldp) {
            if (!as.is_free(oldp, newp - oldp)) { ret = as.brk_cur; break; }
            as.map(oldp, newp - oldp, 3, "[heap]");
        } else if (newp < oldp) as.unmap(newp, oldp - newp);
        as.brk_cur = a0;
        ret = a0;
        break;
    }
    case 54: ret = sys_ioctl(t, a0, a1, a2); break;
    case 55: case 221: ret = sys_fcntl(t, a0, a1, a2); break;
    case 57: {  // setpgid
        int pid = a0 ? (s32)a0 : p.pid;
        int pg = a1 ? (s32)a1 : pid;
        std::lock_guard lk(g_kernel.mtx);
        auto tp = g_kernel.find_proc(pid);
        if (!tp) ret = -lnx::ESRCH_;
        else { tp->pgid = pg; ret = 0; }
        break;
    }
    case 132: {  // getpgid
        std::lock_guard lk(g_kernel.mtx);
        auto tp = a0 ? g_kernel.find_proc(a0) : p.shared_from_this();
        ret = tp ? tp->pgid : -lnx::ESRCH_;
        break;
    }
    case 65: ret = p.pgid; break;
    case 66: p.sid = p.pgid = p.pid; ret = p.pid; break;  // setsid
    case 147: {
        std::lock_guard lk(g_kernel.mtx);
        auto tp = a0 ? g_kernel.find_proc(a0) : p.shared_from_this();
        ret = tp ? tp->sid : -lnx::ESRCH_;
        break;
    }
    case 60: ret = p.umask; p.umask = a0 & 0777; break;
    case 61: ret = 0; break;  // chroot
    case 74: g_kernel.hostname = m.read_str(a0, a1); ret = 0; break;
    case 75: {  // setrlimit (32-bit)
        u32 c = m.rd<u32>(a1), mx = m.rd<u32>(a1 + 4);
        p.rlimits[a0] = {c == 0xffffffffu ? ~0ull : c, mx == 0xffffffffu ? ~0ull : mx};
        ret = 0;
        break;
    }
    case 76: case 191: {      // getrlimit / ugetrlimit
        auto [c, mx] = get_rlimit(p, a0);
        m.wr<u32>(a1, c > 0xffffffffull ? 0xffffffffu : (u32)c);
        m.wr<u32>(a1 + 4, mx > 0xffffffffull ? 0xffffffffu : (u32)mx);
        ret = 0;
        break;
    }
    case 369: {  // prlimit64(pid, res, new, old)
        std::shared_ptr<Process> tp = p.shared_from_this();
        if (a0) {
            std::lock_guard lk(g_kernel.mtx);
            Thread* th = g_kernel.find_thread(a0);
            tp = th ? th->proc->shared_from_this() : g_kernel.find_proc(a0);
        }
        if (!tp) { ret = -lnx::ESRCH_; break; }
        if (a3) {
            auto [c, mx] = get_rlimit(*tp, a1);
            m.wr<u64>(a3, c);
            m.wr<u64>(a3 + 8, mx);
        }
        if (a2) tp->rlimits[a1] = {m.rd<u64>(a2), m.rd<u64>(a2 + 8)};
        ret = 0;
        break;
    }
    case 77: memset(m.ptr(a1), 0, 72); ret = 0; break;  // getrusage
    case 78: {  // gettimeofday
        u64 n = realtime_ns();
        if (a0) { m.wr<u32>(a0, (u32)(n / 1000000000ull)); m.wr<u32>(a0 + 4, (u32)((n / 1000) % 1000000)); }
        if (a1) { m.wr<u32>(a1, 0); m.wr<u32>(a1 + 4, 0); }
        ret = 0;
        break;
    }
    case 79: case 262: ret = 0; break;  // settimeofday / clock_settime
    case 13: {  // time
        u32 s = (u32)(realtime_ns() / 1000000000ull);
        if (a0) m.wr<u32>(a0, s);
        ret = (s32)s;
        break;
    }
    case 83: case 331: {  // symlink / symlinkat
        std::string target = m.read_str(a0);
        std::string h;
        ret = resolve_mod(t, nr == 83 ? lnx::L_AT_FDCWD : (s32)a1, nr == 83 ? a1 : a2, h);
        if (!ret) ret = hfs::symlink(target, h);
        break;
    }
    case 9: case 330: {  // link / linkat
        std::string ho, hn;
        ret = resolve_mod(t, nr == 9 ? lnx::L_AT_FDCWD : (s32)a0, nr == 9 ? a0 : a1, ho, true);
        if (!ret) ret = resolve_mod(t, nr == 9 ? lnx::L_AT_FDCWD : (s32)a2, nr == 9 ? a1 : a3, hn);
        if (!ret) ret = hfs::link(ho, hn);
        break;
    }
    case 85: case 332: {  // readlink / readlinkat
        u32 pp = nr == 85 ? a0 : a1, buf = nr == 85 ? a1 : a2, sz = nr == 85 ? a2 : a3;
        std::string out;
        ret = vfs::readlink(t, m.read_str(pp), out, nr == 85 ? lnx::L_AT_FDCWD : (s32)a0);
        if (!ret) {
            u32 n = std::min<u32>(sz, (u32)out.size());
            m.write_bytes(buf, out.data(), n);
            ret = n;
        }
        break;
    }
    case 88: ret = sys_reboot(t, a0, a1, a2); break;
    case 91: case 192: case 125: case 163: case 144: case 219: case 220: case 150: case 151: case 152: case 153:
    case 0xf0002:
        ret = do_mem_syscall(t, nr); break;
    case 92: case 193: {  // truncate / truncate64
        std::string h;
        ret = resolve_mod(t, lnx::L_AT_FDCWD, a0, h, true);
        s64 len = nr == 92 ? (s64)(s32)a1 : (s64)(((u64)a3 << 32) | a2);
        if (!ret) ret = hfs::truncate(h, len);
        break;
    }
    case 93: case 194: {  // ftruncate / ftruncate64
        FilePtr f = p.fds->get(a0);
        s64 len = nr == 93 ? (s64)a1 : (s64)(((u64)a3 << 32) | a2);
        ret = f ? f->ftruncate(len) : -lnx::EBADF_;
        break;
    }
    case 96: ret = 20; break;     // getpriority
    case 97: case 34: ret = 0; break;  // setpriority / nice
    case 99: case 266: case 100: case 267: {  // statfs / statfs64 / fstatfs / fstatfs64
        u32 bufp = (nr == 266 || nr == 267) ? a2 : a1;
        bool tmp = true;
        if (nr == 99 || nr == 266) {
            Resolved r = vfs::resolve(p, m.read_str(a0), true);
            if (r.err) { ret = -r.err; break; }
            tmp = r.writable || r.kind == Resolved::VIRT;
        }
        write_statfs64(m, bufp, tmp);
        ret = 0;
        break;
    }
    case 103: ret = 0; break;  // syslog
    case 104: ret = sys_setitimer(t, a0, a1, a2); break;
    case 105: memset(m.ptr(a1), 0, 16); ret = 0; break;  // getitimer (approx)
    case 106: ret = do_stat(t, a0, a1, true); break;   // old stat -> treat as stat64 layout (rare)
    case 107: ret = do_stat(t, a0, a1, false); break;
    case 108: ret = sys_fstat64(t, a0, a1); break;
    case 195: ret = do_stat(t, a0, a1, true); break;
    case 196: ret = do_stat(t, a0, a1, false); break;
    case 197: ret = sys_fstat64(t, a0, a1); break;
    case 327: ret = do_stat(t, a1, a2, !(a3 & lnx::L_AT_SYMLINK_NOFOLLOW), (s32)a0, a3); break;
    case 114: ret = sys_wait4(t, (s32)a0, a1, a2, a3); break;
    case 280: ret = sys_waitid(t, a0, a1, a2, a3); break;
    case 116: ret = sys_sysinfo(t, a0); break;
    case 119: do_rt_sigreturn(t, false); noret = true; break;
    case 173: do_rt_sigreturn(t, true); noret = true; break;
    case 122: ret = sys_uname(t, a0); break;
    case 128: case 129: case 379: ret = 0; break;  // init_module / delete_module / finit_module
    case 136: ret = 0; break;  // personality
    case 141: ret = sys_getdents(t, a0, a1, a2); break;
    case 217: ret = sys_getdents64(t, a0, a1, a2); break;
    case 143: ret = 0; break;  // flock
    case 145: ret = sys_readv(t, a0, a1, a2, false); break;
    case 146: ret = sys_readv(t, a0, a1, a2, true); break;
    case 180: {  // pread64(fd, buf, count, pad, lo, hi)
        FilePtr f = p.fds->get(a0);
        ret = f ? (s32)f->pread(t, m.ptr(a1), a2, (s64)(((u64)a5 << 32) | a4)) : -lnx::EBADF_;
        break;
    }
    case 181: {
        FilePtr f = p.fds->get(a0);
        ret = f ? (s32)f->pwrite(t, m.ptr(a1), a2, (s64)(((u64)a5 << 32) | a4)) : -lnx::EBADF_;
        break;
    }
    case 154: case 155: case 156: case 157: ret = 0; break;  // sched_set/getparam/scheduler
    case 158: std::this_thread::yield(); ret = 0; break;
    case 159: ret = 99; break;
    case 160: ret = 1; break;
    case 162: {  // nanosleep
        ret = sys_clock_nanosleep(t, 1, 0, a0, a1);
        break;
    }
    case 265: ret = sys_clock_nanosleep(t, a0, a1, a2, a3); break;
    case 172: ret = sys_prctl(t, a0, a1); break;
    case 174: ret = sys_rt_sigaction(t, a0, a1, a2); break;
    case 175: ret = sys_rt_sigprocmask(t, a0, a1, a2); break;
    case 176: {
        std::lock_guard lk(g_sig_mtx);
        m.wr<u64>(a0, (t.pending | p.pending) & t.sigmask);
        ret = 0;
        break;
    }
    case 177: ret = sys_rt_sigtimedwait(t, a0, a1, a2); break;
    case 179: ret = sys_rt_sigsuspend(t, a0); break;
    case 183: ret = sys_getcwd(t, a0, a1); break;
    case 184: m.wr<u32>(a1, 0); m.wr<u32>(a1 + 4, 0xffffffff); m.wr<u32>(a1 + 8, 0xffffffff); m.wr<u32>(a1 + 12, 0); ret = 0; break;  // capget
    case 185: ret = 0; break;
    case 186: {  // sigaltstack
        if (a1) {
            m.wr<u32>(a1, t.altstack_sp);
            m.wr<u32>(a1 + 4, t.altstack_flags);
            m.wr<u32>(a1 + 8, t.altstack_size);
        }
        if (a0) {
            t.altstack_sp = m.rd<u32>(a0);
            t.altstack_flags = m.rd<u32>(a0 + 4);
            t.altstack_size = m.rd<u32>(a0 + 8);
        }
        ret = 0;
        break;
    }
    case 187: case 239: ret = sys_sendfile(t, a0, a1, a2, a3, nr == 239); break;
    case 241: ret = 0; break;  // sched_setaffinity
    case 242: {  // sched_getaffinity
        memset(m.ptr(a2), 0, a1);
        m.wr<u8>(a2, 0x0f);
        ret = std::min<u32>(a1, 4);
        break;
    }
    case 240: ret = do_futex(t); break;
    case 256: t.clear_child_tid = a0; ret = t.tid; break;  // set_tid_address
    case 338: case 339: ret = 0; break;  // robust list
    case 263: {  // clock_gettime
        put_timespec(m, a1, clock_value(a0));
        ret = 0;
        break;
    }
    case 264: if (a1) put_timespec(m, a1, 1); ret = 0; break;  // clock_getres
    case 269: case 348: case 326: ret = 0; break;  // utimes / utimensat / futimesat
    case 270: ret = 0; break;  // fadvise
    case 314: case 315: ret = 0; break;  // ioprio
    case 345: if (a0) m.wr<u32>(a0, 0); if (a1) m.wr<u32>(a1, 0); ret = 0; break;  // getcpu
    case 0xf0005: t.tls = a0; ret = 0; break;  // set_tls
    // sockets / poll / epoll / eventfd etc.
    default:
        ret = do_net_syscall(t, nr, noret);
        break;
    }
    if (g_kernel.strace && nr != 0xf0005)
        LOGI("[strace] %d/%d %s(%x, %x, %x, %x) = %d\n", p.pid, t.tid, syscall_name(nr), a0, a1, a2, a3, noret ? 0 : ret);
    if (!noret) R[0] = (u32)ret;
    if (ret == -lnx::ENOSYS_ && !noret) LOGI("[sys] pid %d (%s): unimplemented syscall %u (%s)\n", p.pid, p.comm.c_str(), nr, syscall_name(nr));
    // signal delivery at syscall exit
    if (!t.exit_req && !t.exec_pending) {
        bool pend = t.signal_pending();
        if (pend || ret == -ERESTARTSYS || ret == -ERESTARTNOHAND || t.restore_sigmask)
            setup_frame_after_syscall(t, noret ? 0 : ret);
    }
}

static const char* syscall_name(u32 nr) {
    switch (nr) {
    case 1: return "exit"; case 2: return "fork"; case 3: return "read"; case 4: return "write"; case 5: return "open";
    case 6: return "close"; case 8: return "creat"; case 10: return "unlink"; case 11: return "execve"; case 12: return "chdir";
    case 19: return "lseek"; case 20: return "getpid"; case 21: return "mount"; case 33: return "access"; case 37: return "kill";
    case 38: return "rename"; case 39: return "mkdir"; case 40: return "rmdir"; case 41: return "dup"; case 42: return "pipe";
    case 45: return "brk"; case 54: return "ioctl"; case 57: return "setpgid"; case 60: return "umask"; case 63: return "dup2";
    case 64: return "getppid"; case 66: return "setsid"; case 78: return "gettimeofday"; case 83: return "symlink";
    case 85: return "readlink"; case 88: return "reboot"; case 91: return "munmap"; case 114: return "wait4";
    case 119: return "sigreturn"; case 120: return "clone"; case 122: return "uname"; case 125: return "mprotect";
    case 128: return "init_module"; case 140: return "_llseek"; case 142: return "_newselect"; case 145: return "readv";
    case 146: return "writev"; case 162: return "nanosleep"; case 163: return "mremap"; case 168: return "poll";
    case 172: return "prctl"; case 173: return "rt_sigreturn"; case 174: return "rt_sigaction";
    case 175: return "rt_sigprocmask"; case 179: return "rt_sigsuspend"; case 180: return "pread64";
    case 183: return "getcwd"; case 190: return "vfork"; case 191: return "ugetrlimit"; case 192: return "mmap2";
    case 195: return "stat64"; case 196: return "lstat64"; case 197: return "fstat64"; case 217: return "getdents64";
    case 221: return "fcntl64"; case 224: return "gettid"; case 240: return "futex"; case 248: return "exit_group";
    case 256: return "set_tid_address"; case 263: return "clock_gettime"; case 268: return "tgkill";
    case 281: return "socket"; case 282: return "bind"; case 283: return "connect"; case 284: return "listen";
    case 285: return "accept"; case 290: return "sendto"; case 292: return "recvfrom"; case 294: return "setsockopt";
    case 296: return "sendmsg"; case 297: return "recvmsg"; case 322: return "openat"; case 327: return "fstatat64";
    case 328: return "unlinkat"; case 335: return "pselect6"; case 336: return "ppoll"; case 338: return "set_robust_list";
    case 348: return "utimensat"; case 358: return "dup3"; case 359: return "pipe2"; case 369: return "prlimit64";
    case 0xf0005: return "set_tls"; case 0xf0002: return "cacheflush";
    }
    static thread_local char buf[16];
    snprintf(buf, sizeof buf, "sys_%u", nr);
    return buf;
}
