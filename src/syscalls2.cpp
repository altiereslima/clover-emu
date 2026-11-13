// Linux syscalls part 2: memory, futex, sockets, poll/select/epoll, eventfd.
#include "syscalls.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <map>
#include <list>
#include <algorithm>
#include <cerrno>
#include <CommonCrypto/CommonDigest.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // MD5 for AF_ALG emulation

// ---------------------------------------------------------------- memory
u32 guest_mmap_anon(Process& p, u32 len, const char* name) {
    len = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    u32 a = p.mem->find_free(len);
    if (!a) return 0;
    p.mem->map(a, len, 3, name);
    return a;
}

static s32 sys_mmap2(Thread& t, u32 addr, u32 len, u32 prot, u32 flags, s32 fd, u32 pgoff) {
    Process& p = *t.proc;
    AddressSpace& as = *p.mem;
    if (!len) return -lnx::EINVAL_;
    u32 alen = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    u64 off = (u64)pgoff * 4096;
    FilePtr f;
    if (!(flags & lnx::L_MAP_ANONYMOUS)) {
        f = p.fds->get(fd);
        if (!f) return -lnx::EBADF_;
    }
    std::lock_guard lk(as.mtx);
    bool flush = false;  // JIT invalidation needed (only when executable code may change)
    // device mappings may need host-page alignment
    u32 align = GUEST_PAGE;
    if (f && !dynamic_cast<HostFile*>(f.get())) align = host_page_size();
    u32 where;
    if (flags & lnx::L_MAP_FIXED) {
        if (addr & (GUEST_PAGE - 1)) return -lnx::EINVAL_;
        where = addr;
    } else if (addr && !(addr & (align - 1)) && (u64)addr + alen <= MMAP_TOP && addr >= 0x10000 && as.is_free(addr, alen)) {
        where = addr;
    } else {
        where = as.find_free(alen, align);
        if (!where) return -lnx::ENOMEM_;
    }
    flush = (prot & lnx::L_PROT_EXEC) || as.has_exec(where, alen);
    if (f) {
        bool handled = false;
        int r = f->mmap(t, where, len, prot, flags, off, &handled);
        if (r < 0) return r;
        if (!handled) {
            as.map(where, alen, prot, f->path);
            // copy file contents
            u32 done = 0;
            while (done < len) {
                s64 n = f->pread(t, as.ptr(where + done), len - done, (s64)(off + done));
                if (n <= 0) break;
                done += (u32)n;
            }
            if ((flags & lnx::L_MAP_SHARED) && (prot & lnx::L_PROT_WRITE))
                LOGC(LOG_MEM, "[mem] pid %d: MAP_SHARED writable file mapping of %s (writes not synced)\n", p.pid, f->path.c_str());
        }
    } else {
        as.map(where, alen, prot, (flags & lnx::L_MAP_SHARED) ? "[shared-anon]" : "");
    }
    if (flush) p.invalidate_all(where, alen);
    LOGC(LOG_MEM, "[mem] pid %d mmap2(%08x, %x, %x, %x, %d, %x) = %08x\n", p.pid, addr, len, prot, flags, fd, pgoff, where);
    return (s32)where;
}

s32 do_mem_syscall(Thread& t, u32 nr) {
    auto& R = t.R();
    Process& p = *t.proc;
    AddressSpace& as = *p.mem;
    u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
    switch (nr) {
    case 192: return sys_mmap2(t, a0, a1, a2, a3, (s32)a4, a5);
    case 91: {  // munmap
        if (a0 & (GUEST_PAGE - 1)) return -lnx::EINVAL_;
        bool flush = as.has_exec(a0, a1);
        as.unmap(a0, a1);
        if (flush) p.invalidate_all(a0, a1);
        return 0;
    }
    case 125:  // mprotect
        // No JIT flush here: on ARM Linux, code writers must cacheflush() the bytes they change (LuaJIT
        // does), and flushing whole RW<->RX toggled ranges made ReedPlayer recompile every trace.
        as.protect(a0, a1, a2);
        return 0;
    case 163: {  // mremap(old, oldlen, newlen, flags, newaddr)
        u32 oldlen = (a1 + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        u32 newlen = (a2 + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        std::lock_guard lk(as.mtx);
        if (newlen <= oldlen) {
            if (newlen < oldlen) as.unmap(a0 + newlen, oldlen - newlen);
            return (s32)a0;
        }
        u32 prot = 3;
        std::string name;
        {
            auto it = as.regions.upper_bound(a0);
            if (it != as.regions.begin()) {
                --it;
                prot = it->second.prot;
                name = it->second.name;
            }
        }
        if (!(a3 & 2) && as.is_free(a0 + oldlen, newlen - oldlen)) {
            as.map(a0 + oldlen, newlen - oldlen, prot, name);
            return (s32)a0;
        }
        if (!(a3 & 1)) return -lnx::ENOMEM_;
        u32 dst = (a3 & 2) ? a4 : as.find_free(newlen);
        if (!dst) return -lnx::ENOMEM_;
        as.map(dst, newlen, prot, name);
        memcpy(as.ptr(dst), as.ptr(a0), oldlen);
        bool flush = as.has_exec(a0, oldlen);
        as.unmap(a0, oldlen);
        if (flush) p.invalidate_all(a0, oldlen);
        return (s32)dst;
    }
    case 144: case 150: case 151: case 152: case 153: return 0;  // msync, mlock...
    case 219: {  // mincore
        u32 pages = (a1 + GUEST_PAGE - 1) / GUEST_PAGE;
        memset(as.ptr(a2), 1, pages);
        return 0;
    }
    case 220: return 0;  // madvise
    case 0xf0002:        // cacheflush(start, end, flags)
        p.invalidate_all(a0, a1 - a0);
        return 0;
    }
    return -lnx::ENOSYS_;
}

// ---------------------------------------------------------------- futex
namespace {
struct FWaiter {
    Thread* t;
    AddressSpace* as;
    u32 addr;
    u32 bitset;
    bool woken = false;
};
std::mutex g_fmtx;
std::map<std::pair<AddressSpace*, u32>, std::list<FWaiter*>> g_fq;

int wake_locked(AddressSpace* as, u32 addr, int n, u32 bitset) {
    auto it = g_fq.find({as, addr});
    if (it == g_fq.end()) return 0;
    int cnt = 0;
    auto& l = it->second;
    for (auto w = l.begin(); w != l.end() && cnt < n;) {
        if ((*w)->bitset & bitset) {
            FWaiter* fw = *w;
            w = l.erase(w);
            // Notify while holding the waiter's lock: once it is released the waiter may return and
            // its stack-allocated FWaiter (and possibly its Thread) can disappear.
            Thread* wt = fw->t;
            std::lock_guard lk(wt->wmtx);
            fw->woken = true;
            wt->wcv.notify_all();
            cnt++;
        } else ++w;
    }
    if (l.empty()) g_fq.erase(it);
    return cnt;
}

s32 futex_wait(Thread& t, u32 addr, u32 val, u64 deadline_ns, bool abs_realtime, u32 bitset) {
    AddressSpace* as = t.proc->mem.get();
    FWaiter w{&t, as, addr, bitset};
    {
        std::lock_guard fl(g_fmtx);
        u32 cur = __atomic_load_n((u32*)as->ptr(addr), __ATOMIC_SEQ_CST);
        if (cur != val) return -lnx::EAGAIN_;
        g_fq[{as, addr}].push_back(&w);
    }
    s32 result = 0;
    {
        std::unique_lock lk(t.wmtx);
        while (!w.woken) {
            lk.unlock();
            bool sp = t.signal_pending();
            lk.lock();
            if (w.woken) break;
            if (sp) { result = -lnx::EINTR_; break; }
            u64 now = abs_realtime ? realtime_ns() : now_ns();
            if (deadline_ns && now >= deadline_ns) { result = -lnx::ETIMEDOUT_; break; }
            u64 wait = 20000000ull;
            if (deadline_ns) wait = std::min<u64>(wait, deadline_ns - now);
            t.interrupted = false;
            t.wcv.wait_for(lk, std::chrono::nanoseconds(wait), [&] { return w.woken || t.interrupted.load(); });
        }
    }
    if (result != 0) {
        std::lock_guard fl(g_fmtx);
        if (w.woken) return 0;
        auto it = g_fq.find({as, w.addr});
        if (it != g_fq.end()) {
            it->second.remove(&w);
            if (it->second.empty()) g_fq.erase(it);
        }
    }
    return result;
}
}  // namespace

int futex_wake(Process& p, u32 addr, int n, u32 bitset) {
    std::lock_guard fl(g_fmtx);
    return wake_locked(p.mem.get(), addr, n, bitset);
}

s32 do_futex(Thread& t) {
    auto& R = t.R();
    u32 uaddr = R[0], op = R[1], val = R[2], a3 = R[3], uaddr2 = R[4], val3 = R[5];
    AddressSpace& m = *t.proc->mem;
    int cmd = op & 127;
    bool realtime = op & 256;
    switch (cmd) {
    case 0: {  // WAIT
        u64 dl = 0;
        if (a3) dl = now_ns() + (u64)m.rd<u32>(a3) * 1000000000ull + m.rd<u32>(a3 + 4);
        return futex_wait(t, uaddr, val, dl, false, 0xffffffff);
    }
    case 9: {  // WAIT_BITSET (absolute timeout)
        u64 dl = 0;
        if (a3) dl = (u64)m.rd<u32>(a3) * 1000000000ull + m.rd<u32>(a3 + 4);
        if (dl && !realtime) dl = dl - 10ull * 1000000000ull + g_kernel.boot_time_ns;  // guest monotonic -> host
        return futex_wait(t, uaddr, val, dl, realtime, val3 ? val3 : 0xffffffff);
    }
    case 1: return futex_wake(*t.proc, uaddr, (int)val, 0xffffffff);
    case 10: return futex_wake(*t.proc, uaddr, (int)val, val3);
    case 3: case 4: {  // REQUEUE / CMP_REQUEUE
        std::lock_guard fl(g_fmtx);
        if (cmd == 4 && __atomic_load_n((u32*)m.ptr(uaddr), __ATOMIC_SEQ_CST) != val3) return -lnx::EAGAIN_;
        AddressSpace* as = t.proc->mem.get();
        int woke = wake_locked(as, uaddr, (int)val, 0xffffffff);
        int moved = 0;
        auto it = g_fq.find({as, uaddr});
        if (it != g_fq.end()) {
            auto& src = it->second;
            auto& dst = g_fq[{as, uaddr2}];
            while (!src.empty() && moved < (int)a3) {
                FWaiter* w = src.front();
                src.pop_front();
                w->addr = uaddr2;
                dst.push_back(w);
                moved++;
            }
            if (src.empty()) g_fq.erase({as, uaddr});
            if (dst.empty()) g_fq.erase({as, uaddr2});
        }
        return woke + moved;
    }
    case 5: {  // WAKE_OP
        std::lock_guard fl(g_fmtx);
        AddressSpace* as = t.proc->mem.get();
        u32 opn = (val3 >> 28) & 0xf, cmp = (val3 >> 24) & 0xf;
        s32 oparg = ((s32)(val3 << 8)) >> 20;
        s32 cmparg = ((s32)(val3 << 20)) >> 20;
        if (opn & 8) oparg = 1 << (oparg & 31);
        opn &= 7;
        u32* p2 = (u32*)m.ptr(uaddr2);
        u32 old = __atomic_load_n(p2, __ATOMIC_SEQ_CST), nv;
        do {
            switch (opn) {
            case 0: nv = oparg; break;
            case 1: nv = old + oparg; break;
            case 2: nv = old | oparg; break;
            case 3: nv = old & ~oparg; break;
            case 4: nv = old ^ oparg; break;
            default: return -lnx::ENOSYS_;
            }
        } while (!__atomic_compare_exchange_n(p2, &old, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        int woke = wake_locked(as, uaddr, (int)val, 0xffffffff);
        bool c;
        s32 o = (s32)old;
        switch (cmp) {
        case 0: c = o == cmparg; break;
        case 1: c = o != cmparg; break;
        case 2: c = o < cmparg; break;
        case 3: c = o <= cmparg; break;
        case 4: c = o > cmparg; break;
        case 5: c = o >= cmparg; break;
        default: c = false;
        }
        if (c) woke += wake_locked(as, uaddr2, (int)a3, 0xffffffff);
        return woke;
    }
    case 6: case 8: {  // LOCK_PI / TRYLOCK_PI
        u32* pp = (u32*)m.ptr(uaddr);
        while (true) {
            u32 cur = 0;
            if (__atomic_compare_exchange_n(pp, &cur, (u32)t.tid, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
            if ((cur & 0x3fffffff) == (u32)t.tid) return -lnx::EDEADLK_;
            if (cmd == 8) return -lnx::EAGAIN_;
            u32 nv = cur | 0x80000000u;
            if (!__atomic_compare_exchange_n(pp, &cur, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            s32 r = futex_wait(t, uaddr, nv, 0, false, 0xffffffff);
            if (r == -lnx::EINTR_) return r;
        }
    }
    case 7: {  // UNLOCK_PI
        u32* pp = (u32*)m.ptr(uaddr);
        __atomic_store_n(pp, 0u, __ATOMIC_SEQ_CST);
        futex_wake(*t.proc, uaddr, 1, 0xffffffff);
        return 0;
    }
    }
    LOGI("[futex] unsupported op %u\n", op);
    return -lnx::ENOSYS_;
}

// ---------------------------------------------------------------- sockets
namespace {
std::string g_sockdir;

struct SockFile : HostFile {
    int domain = 1;
    bool is_socket() const override { return true; }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFSOCK | 0777);
        return 0;
    }
};

// eventfd
struct EventFd : File {
    std::mutex mtx;
    u64 count = 0;
    bool semaphore = false;
    int pr = -1, pw = -1;  // host pipe signalling readability
    EventFd() {
        int p[2];
        if (pipe(p) == 0) {
            pr = p[0];
            pw = p[1];
            fcntl(pr, F_SETFL, O_NONBLOCK);
            fcntl(pw, F_SETFL, O_NONBLOCK);
            fcntl(pr, F_SETFD, FD_CLOEXEC);
            fcntl(pw, F_SETFD, FD_CLOEXEC);
        }
    }
    ~EventFd() override {
        if (pr >= 0) ::close(pr);
        if (pw >= 0) ::close(pw);
    }
    void update_pipe() {
        char buf[64];
        while (::read(pr, buf, sizeof buf) > 0) {}
        if (count) {
            char c = 1;
            (void)::write(pw, &c, 1);
        }
    }
    s64 read(Thread& t, u8* buf, u32 n) override {
        if (n < 8) return -lnx::EINVAL_;
        while (true) {
            {
                std::lock_guard lk(mtx);
                if (count) {
                    u64 v = semaphore ? 1 : count;
                    count -= v;
                    memcpy(buf, &v, 8);
                    update_pipe();
                    return 8;
                }
            }
            if (flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
            int w = wait_fd(t, pr, POLLIN);
            if (w == -lnx::EINTR_) return -ERESTARTSYS;
        }
    }
    s64 write(Thread&, const u8* buf, u32 n) override {
        if (n < 8) return -lnx::EINVAL_;
        u64 v;
        memcpy(&v, buf, 8);
        std::lock_guard lk(mtx);
        count += v;
        update_pipe();
        return 8;
    }
    int host_fd() override { return pr; }
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

// inotify stand-in: never reports events
struct DummyFd : File {
    int pr = -1, pw = -1;
    DummyFd() {
        int p[2];
        if (pipe(p) == 0) {
            pr = p[0];
            pw = p[1];
            fcntl(pr, F_SETFL, O_NONBLOCK);
        }
    }
    ~DummyFd() override {
        if (pr >= 0) ::close(pr);
        if (pw >= 0) ::close(pw);
    }
    s64 read(Thread& t, u8*, u32) override {
        if (flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
        int w = wait_fd(t, pr, POLLIN);
        return w == -lnx::EINTR_ ? -ERESTARTSYS : -lnx::EAGAIN_;
    }
    int host_fd() override { return pr; }
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

// epoll
struct EpollFile : File {
    struct Ent {
        u32 events;
        u64 data;
        std::weak_ptr<File> f;
    };
    std::mutex mtx;
    std::map<int, Ent> ents;
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

// AF_ALG (Linux kernel crypto API) — hash algorithms only.
struct AlgOp : File {
    std::string alg;
    CC_SHA1_CTX sha1;
    CC_SHA256_CTX sha256;
    CC_MD5_CTX md5;
    bool started = false;
    std::vector<u8> digest;  // pending output after finalization
    size_t digest_pos = 0;
    void reset() {
        if (alg == "sha1") CC_SHA1_Init(&sha1);
        else if (alg == "sha256") CC_SHA256_Init(&sha256);
        else CC_MD5_Init(&md5);
        started = true;
    }
    void update(const u8* p, u32 n) {
        if (!started || !digest.empty()) {
            digest.clear();
            digest_pos = 0;
            reset();
        }
        if (alg == "sha1") CC_SHA1_Update(&sha1, p, n);
        else if (alg == "sha256") CC_SHA256_Update(&sha256, p, n);
        else CC_MD5_Update(&md5, p, n);
    }
    void final() {
        if (!started) reset();
        if (alg == "sha1") { digest.resize(20); CC_SHA1_Final(digest.data(), &sha1); }
        else if (alg == "sha256") { digest.resize(32); CC_SHA256_Final(digest.data(), &sha256); }
        else { digest.resize(16); CC_MD5_Final(digest.data(), &md5); }
        digest_pos = 0;
        started = false;
    }
    s64 send(const u8* p, u32 n, bool more) {
        update(p, n);
        if (!more) final();
        return n;
    }
    s64 write(Thread&, const u8* p, u32 n) override { return send(p, n, false); }
    s64 read(Thread&, u8* buf, u32 n) override {
        if (digest.empty()) final();
        u32 c = (u32)std::min<size_t>(n, digest.size() - digest_pos);
        memcpy(buf, digest.data() + digest_pos, c);
        digest_pos += c;
        if (digest_pos >= digest.size()) { digest.clear(); digest_pos = 0; }
        return c;
    }
    bool is_socket() const override { return true; }
    int fstat(LinuxStat64& st) override { st = make_stat(lnx::L_S_IFSOCK | 0777); return 0; }
};
struct AlgSock : File {
    std::string type, name;
    bool is_socket() const override { return true; }
    int fstat(LinuxStat64& st) override { st = make_stat(lnx::L_S_IFSOCK | 0777); return 0; }
};

std::string hexhash(const std::string& s) {
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)std::hash<std::string>{}(s));
    return b;
}

// Convert guest sockaddr to host. Returns host length or -errno.
int to_host_addr(Thread& t, int domain, u32 addr, u32 len, sockaddr_storage& out, std::string* guest_path, bool for_bind) {
    AddressSpace& m = *t.proc->mem;
    memset(&out, 0, sizeof out);
    u16 fam = m.rd<u16>(addr);
    if (fam == 1) {
        auto* su = (sockaddr_un*)&out;
        su->sun_family = AF_UNIX;
        std::string host;
        if (len <= 2) return -lnx::EINVAL_;
        const char* pth = (const char*)m.ptr(addr + 2);
        if (pth[0] == 0) {
            std::string name(pth + 1, len - 3);
            host = g_sockdir + "/@" + hexhash(name);
            if (guest_path) *guest_path = "@" + name;
        } else {
            std::string gp(pth, strnlen(pth, std::min<u32>(len - 2, 108)));
            Resolved r = vfs::resolve(*t.proc, gp, true);
            if (r.err && r.err != lnx::ENOENT_) return -r.err;
            std::string canon = r.guest.empty() ? vfs::join(t.proc->cwd, gp) : r.guest;
            host = g_sockdir + "/" + hexhash(canon);
            if (guest_path) *guest_path = canon;
            if (for_bind && r.kind == Resolved::HOST && r.writable) {
                int fd = ::open(r.host.c_str(), O_CREAT | O_WRONLY, 0666);
                if (fd >= 0) ::close(fd);
            }
        }
        strncpy(su->sun_path, host.c_str(), sizeof su->sun_path - 1);
        su->sun_len = sizeof(sockaddr_un);
        return sizeof(sockaddr_un);
    }
    if (fam == 2) {
        auto* si = (sockaddr_in*)&out;
        si->sin_len = sizeof(sockaddr_in);
        si->sin_family = AF_INET;
        memcpy(&si->sin_port, m.ptr(addr + 2), 2);
        memcpy(&si->sin_addr, m.ptr(addr + 4), 4);
        return sizeof(sockaddr_in);
    }
    if (fam == 10) {
        auto* s6 = (sockaddr_in6*)&out;
        s6->sin6_len = sizeof(sockaddr_in6);
        s6->sin6_family = AF_INET6;
        memcpy(&s6->sin6_port, m.ptr(addr + 2), 2);
        memcpy(&s6->sin6_flowinfo, m.ptr(addr + 4), 4);
        memcpy(&s6->sin6_addr, m.ptr(addr + 8), 16);
        memcpy(&s6->sin6_scope_id, m.ptr(addr + 24), 4);
        return sizeof(sockaddr_in6);
    }
    return -lnx::EAFNOSUPPORT_;
}

void from_host_addr(Thread& t, const sockaddr* sa, u32 addrp, u32 lenp) {
    if (!addrp) return;
    AddressSpace& m = *t.proc->mem;
    u32 maxlen = lenp ? m.rd<u32>(lenp) : 0;
    u8 b[128] = {};
    u32 n = 2;
    if (sa->sa_family == AF_UNIX) {
        u16 f = 1;
        memcpy(b, &f, 2);
        n = 2;
    } else if (sa->sa_family == AF_INET) {
        auto* si = (const sockaddr_in*)sa;
        u16 f = 2;
        memcpy(b, &f, 2);
        memcpy(b + 2, &si->sin_port, 2);
        memcpy(b + 4, &si->sin_addr, 4);
        n = 16;
    } else if (sa->sa_family == AF_INET6) {
        auto* s6 = (const sockaddr_in6*)sa;
        u16 f = 10;
        memcpy(b, &f, 2);
        memcpy(b + 2, &s6->sin6_port, 2);
        memcpy(b + 8, &s6->sin6_addr, 16);
        n = 28;
    }
    m.write_bytes(addrp, b, std::min(n, maxlen));
    if (lenp) m.wr<u32>(lenp, n);
}

int msg_flags(u32 lf) {
    int h = 0;
    if (lf & 1) h |= MSG_OOB;
    if (lf & 2) h |= MSG_PEEK;
    if (lf & 0x40) h |= MSG_DONTWAIT;
    if (lf & 0x100) h |= MSG_WAITALL;
    return h;
}

std::shared_ptr<SockFile> get_sock(Thread& t, int fd, s32* err) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) { *err = -lnx::EBADF_; return nullptr; }
    auto s = std::dynamic_pointer_cast<SockFile>(f);
    if (!s) { *err = -lnx::ENOTSOCK_; return nullptr; }
    return s;
}

s32 sock_wait(Thread& t, SockFile& s, short ev) {
    if (s.flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
    int w = wait_fd(t, s.fd, ev);
    if (w == -lnx::EINTR_) return -ERESTARTSYS;
    return w;
}

// ---------------------------------------------------------------- poll core
// Translate guest poll events <-> host
short ev_to_host(u32 e) {
    short h = 0;
    if (e & (0x001 | 0x040 | 0x080)) h |= POLLIN;
    if (e & 0x002) h |= POLLPRI;
    if (e & (0x004 | 0x100 | 0x200)) h |= POLLOUT;
    return h;
}
u32 ev_from_host(short h, u32 req) {
    u32 e = 0;
    if (h & POLLIN) e |= req & (0x001 | 0x040 | 0x080);
    if (h & POLLPRI) e |= 0x002;
    if (h & POLLOUT) e |= req & (0x004 | 0x100 | 0x200);
    if (h & POLLERR) e |= 0x008;
    if (h & POLLHUP) e |= 0x010 | (req & 0x2000);
    if (h & POLLNVAL) e |= 0x020;
    return e;
}

struct PollItem {
    int gfd;
    u32 req;
    u32 res;
    FilePtr f;
};

// Polls items; timeout_ms < 0 = infinite. Returns number ready or -errno.
s32 do_poll(Thread& t, std::vector<PollItem>& items, s64 timeout_ms) {
    u64 deadline = timeout_ms >= 0 ? now_ns() + (u64)timeout_ms * 1000000ull : 0;
    char buf[64];
    while (true) {
        std::vector<pollfd> pf;
        std::vector<int> idx;
        int ready = 0;
        for (size_t i = 0; i < items.size(); i++) {
            auto& it = items[i];
            it.res = 0;
            if (it.gfd < 0) continue;
            if (!it.f) {
                it.res = 0x20;  // POLLNVAL
                ready++;
                continue;
            }
            int hf = it.f->host_fd();
            if (hf >= 0) {
                pf.push_back({hf, ev_to_host(it.req), 0});
                idx.push_back((int)i);
            } else {
                u32 m = it.f->poll_mask();
                u32 r = 0;
                if ((m & 1) && (it.req & 0x41)) r |= it.req & 0x41;
                if ((m & 4) && (it.req & 0x104)) r |= it.req & 0x104;
                if (m & 0x10) r |= 0x10;
                it.res = r;
                if (r) ready++;
            }
        }
        while (::read(t.wake_pipe[0], buf, sizeof buf) > 0) {}
        pf.push_back({t.wake_pipe[0], POLLIN, 0});
        int to;
        if (ready) to = 0;
        else if (timeout_ms < 0) to = 50;
        else {
            u64 n = now_ns();
            to = n >= deadline ? 0 : (int)std::min<u64>((deadline - n) / 1000000ull + 1, 50);
        }
        if (!ready && t.signal_pending()) return -ERESTARTNOHAND;
        int r = ::poll(pf.data(), (nfds_t)pf.size(), to);
        if (r < 0 && errno != EINTR) return -host_errno_to_linux(errno);
        for (size_t k = 0; k < idx.size(); k++) {
            if (pf[k].revents) {
                auto& it = items[idx[k]];
                it.res = ev_from_host(pf[k].revents, it.req | 0x18);
                if (it.res) ready++;
            }
        }
        if (ready) return ready;
        if (timeout_ms >= 0 && now_ns() >= deadline) return 0;
        if (t.signal_pending()) return -ERESTARTNOHAND;
    }
}

s32 sys_poll(Thread& t, u32 fdsp, u32 n, s64 timeout_ms) {
    AddressSpace& m = *t.proc->mem;
    std::vector<PollItem> items(n);
    for (u32 i = 0; i < n; i++) {
        items[i].gfd = m.rd<s32>(fdsp + i * 8);
        items[i].req = m.rd<u16>(fdsp + i * 8 + 4);
        if (items[i].gfd >= 0) items[i].f = t.proc->fds->get(items[i].gfd);
    }
    s32 r = do_poll(t, items, timeout_ms);
    if (r < 0) return r;
    for (u32 i = 0; i < n; i++) m.wr<u16>(fdsp + i * 8 + 6, (u16)items[i].res);
    return r;
}

s32 sys_select(Thread& t, int nfds, u32 rp, u32 wp, u32 ep, s64 timeout_ms) {
    AddressSpace& m = *t.proc->mem;
    std::vector<PollItem> items;
    auto isset = [&](u32 set, int fd) { return set && (m.rd<u32>(set + (fd / 32) * 4) >> (fd % 32)) & 1; };
    for (int fd = 0; fd < nfds; fd++) {
        u32 req = 0;
        if (isset(rp, fd)) req |= 1;
        if (isset(wp, fd)) req |= 4;
        if (isset(ep, fd)) req |= 2;
        if (!req) continue;
        PollItem it{fd, req, 0, t.proc->fds->get(fd)};
        if (!it.f) return -lnx::EBADF_;
        items.push_back(it);
    }
    s32 r = do_poll(t, items, timeout_ms);
    if (r < 0) return r;
    u32 words = (nfds + 31) / 32;
    for (u32 w = 0; w < words; w++) {
        if (rp) m.wr<u32>(rp + w * 4, 0);
        if (wp) m.wr<u32>(wp + w * 4, 0);
        if (ep) m.wr<u32>(ep + w * 4, 0);
    }
    int cnt = 0;
    auto set = [&](u32 s, int fd) { if (s) m.wr<u32>(s + (fd / 32) * 4, m.rd<u32>(s + (fd / 32) * 4) | (1u << (fd % 32))); };
    for (auto& it : items) {
        if ((it.res & (1 | 0x18)) && (it.req & 1)) { set(rp, it.gfd); cnt++; }
        if ((it.res & (4 | 0x8)) && (it.req & 4)) { set(wp, it.gfd); cnt++; }
        if ((it.res & 2) && (it.req & 2)) { set(ep, it.gfd); cnt++; }
    }
    return cnt;
}

s32 sys_epoll_wait(Thread& t, int epfd, u32 evp, int maxev, s64 timeout_ms) {
    FilePtr f = t.proc->fds->get(epfd);
    auto ep = std::dynamic_pointer_cast<EpollFile>(f);
    if (!ep) return -lnx::EINVAL_;
    std::vector<PollItem> items;
    std::vector<u64> datas;
    {
        std::lock_guard lk(ep->mtx);
        for (auto& [fd, e] : ep->ents) {
            if (!(e.events & ~0x60000000u)) continue;  // disabled oneshot
            PollItem it{fd, e.events & 0x3fff, 0, e.f.lock()};
            if (!it.f) continue;
            items.push_back(it);
            datas.push_back(e.data);
        }
    }
    s32 r = do_poll(t, items, timeout_ms);
    if (r <= 0) return r;
    AddressSpace& m = *t.proc->mem;
    int n = 0;
    std::lock_guard lk(ep->mtx);
    for (size_t i = 0; i < items.size() && n < maxev; i++) {
        if (!items[i].res) continue;
        m.wr<u32>(evp + n * 16, items[i].res);
        m.wr<u64>(evp + n * 16 + 8, datas[i]);
        auto it = ep->ents.find(items[i].gfd);
        if (it != ep->ents.end() && (it->second.events & 0x40000000)) it->second.events &= 0x60000000;  // ONESHOT
        n++;
    }
    return n;
}
}  // namespace

static s64 timespec_ms(AddressSpace& m, u32 tsp) {
    if (!tsp) return -1;
    return (s64)m.rd<u32>(tsp) * 1000 + m.rd<u32>(tsp + 4) / 1000000;
}

s32 do_net_syscall(Thread& t, u32 nr, bool& noret) {
    auto& R = t.R();
    Process& p = *t.proc;
    AddressSpace& m = *p.mem;
    u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
    if (g_sockdir.empty()) {
        // per emulator instance, so two consoles can run side by side (short path: sun_path is 104 bytes)
        g_sockdir = "/tmp/clover-sock-" + std::to_string(getuid()) + "-" + std::to_string(getpid());
        mkdir(g_sockdir.c_str(), 0700);
    }
    s32 err = 0;
    // AF_ALG handling
    if (nr == 281 && a0 == 38) {
        auto as = std::make_shared<AlgSock>();
        as->flags = 2;
        as->path = "socket:[alg]";
        return p.fds->install(as, a1 & lnx::L_O_CLOEXEC);
    }
    if (nr == 282 || nr == 285 || nr == 366 || nr == 289 || nr == 290 || nr == 291 || nr == 292 || nr == 296 || nr == 297) {
        FilePtr f = p.fds->get(a0);
        if (auto as = std::dynamic_pointer_cast<AlgSock>(f)) {
            if (nr == 282) {
                as->type = m.read_str(a1 + 2, 14);
                as->name = m.read_str(a1 + 24, 64);
                if (as->type != "hash" || (as->name != "sha1" && as->name != "sha256" && as->name != "md5")) {
                    LOGI("[alg] unsupported algorithm %s/%s\n", as->type.c_str(), as->name.c_str());
                    return -lnx::ENOENT_;
                }
                return 0;
            }
            if (nr == 285 || nr == 366) {
                auto op = std::make_shared<AlgOp>();
                op->alg = as->name;
                op->flags = 2;
                op->path = "socket:[alg-op]";
                op->reset();
                return p.fds->install(op, nr == 366 && (a3 & lnx::L_O_CLOEXEC));
            }
            return -lnx::EOPNOTSUPP_;
        }
        if (auto op = std::dynamic_pointer_cast<AlgOp>(f)) {
            switch (nr) {
            case 289: case 290: return (s32)op->send(m.ptr(a1), a2, a3 & 0x8000);
            case 291: case 292: return (s32)op->read(t, m.ptr(a1), a2);
            case 296: case 297: {
                u32 iov = m.rd<u32>(a1 + 8), iovlen = m.rd<u32>(a1 + 12);
                s64 total = 0;
                for (u32 i = 0; i < iovlen; i++) {
                    u32 b = m.rd<u32>(iov + i * 8), l = m.rd<u32>(iov + i * 8 + 4);
                    if (nr == 296) { op->update(m.ptr(b), l); total += l; }
                    else { s64 r = op->read(t, m.ptr(b), l); total += r; if ((u32)r < l) break; }
                }
                if (nr == 296 && !(a2 & 0x8000)) op->final();
                return (s32)total;
            }
            }
            return -lnx::EOPNOTSUPP_;
        }
    }
    switch (nr) {
    case 168: return sys_poll(t, a0, a1, (s32)a2);
    case 336: {  // ppoll(fds, n, tsp, sigmask, size)
        extern std::mutex g_sig_mtx;
        if (a3) {
            std::lock_guard lk(g_sig_mtx);
            t.saved_sigmask = t.sigmask;
            t.restore_sigmask = true;
            t.sigmask = m.rd<u64>(a3);
        }
        s32 r = sys_poll(t, a0, a1, timespec_ms(m, a2));
        if (r == -ERESTARTNOHAND) r = -lnx::EINTR_;
        return r;
    }
    case 142: {  // _newselect(n, r, w, e, timeval*)
        s64 to = a4 ? (s64)m.rd<u32>(a4) * 1000 + m.rd<u32>(a4 + 4) / 1000 : -1;
        return sys_select(t, a0, a1, a2, a3, to);
    }
    case 335: {  // pselect6(n, r, w, e, timespec*, sigmask)
        return sys_select(t, a0, a1, a2, a3, timespec_ms(m, a4));
    }
    case 250: case 357: {  // epoll_create / epoll_create1
        auto e = std::make_shared<EpollFile>();
        e->path = "anon_inode:[eventpoll]";
        return p.fds->install(e, nr == 357 && (a0 & lnx::L_O_CLOEXEC));
    }
    case 251: {  // epoll_ctl(epfd, op, fd, event*)
        auto ep = std::dynamic_pointer_cast<EpollFile>(p.fds->get(a0));
        if (!ep) return -lnx::EINVAL_;
        FilePtr f = p.fds->get(a2);
        if (!f) return -lnx::EBADF_;
        std::lock_guard lk(ep->mtx);
        if (a1 == 1) {  // ADD
            if (ep->ents.count(a2)) return -lnx::EEXIST_;
            ep->ents[a2] = {m.rd<u32>(a3), m.rd<u64>(a3 + 8), f};
        } else if (a1 == 2) {  // DEL
            ep->ents.erase(a2);
        } else if (a1 == 3) {  // MOD
            auto it = ep->ents.find(a2);
            if (it == ep->ents.end()) return -lnx::ENOENT_;
            it->second.events = m.rd<u32>(a3);
            it->second.data = m.rd<u64>(a3 + 8);
        }
        return 0;
    }
    case 252: return sys_epoll_wait(t, a0, a1, a2, (s32)a3);
    case 346: return sys_epoll_wait(t, a0, a1, a2, (s32)a3);
    case 351: case 356: {  // eventfd / eventfd2
        auto e = std::make_shared<EventFd>();
        e->count = a0;
        e->update_pipe();
        u32 fl = nr == 356 ? a1 : 0;
        e->semaphore = fl & 1;
        e->flags = 2 | (fl & lnx::L_O_NONBLOCK);
        e->path = "anon_inode:[eventfd]";
        return p.fds->install(e, fl & lnx::L_O_CLOEXEC);
    }
    case 316: case 360: {  // inotify_init(1)
        auto d = std::make_shared<DummyFd>();
        d->flags = (nr == 360 ? a0 : 0) & lnx::L_O_NONBLOCK;
        d->path = "anon_inode:inotify";
        return p.fds->install(d, nr == 360 && (a0 & lnx::L_O_CLOEXEC));
    }
    case 317: return 1;  // inotify_add_watch
    case 318: return 0;
    case 349: case 355: case 350: case 353: case 354: return -lnx::ENOSYS_;  // signalfd/timerfd
    case 281: {  // socket(domain, type, proto)
        int type = a1 & 0xf;
        bool nb = a1 & lnx::L_O_NONBLOCK, ce = a1 & lnx::L_O_CLOEXEC;
        int hfd;
        if (a0 == 16) {  // AF_NETLINK -> inert socket
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return -host_errno_to_linux(errno);
            ::close(sv[1]);
            hfd = sv[0];
        } else if (a0 == 1) {
            int ht = type == 1 ? SOCK_STREAM : SOCK_DGRAM;
            hfd = ::socket(AF_UNIX, ht, 0);
        } else if (a0 == 2 || a0 == 10) {
            int ht = type == 1 ? SOCK_STREAM : type == 2 ? SOCK_DGRAM : SOCK_RAW;
            hfd = ::socket(a0 == 2 ? AF_INET : AF_INET6, ht, type == 3 ? (int)a2 : 0);
        } else return -lnx::EAFNOSUPPORT_;
        if (hfd < 0) return -host_errno_to_linux(errno);
        fcntl(hfd, F_SETFL, O_NONBLOCK);
        fcntl(hfd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(hfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        auto s = std::make_shared<SockFile>();
        s->fd = hfd;
        s->domain = a0;
        s->flags = 2 | (nb ? lnx::L_O_NONBLOCK : 0);
        s->path = "socket:[" + std::to_string(hfd) + "]";
        return p.fds->install(s, ce);
    }
    case 288: {  // socketpair(domain, type, proto, sv*)
        int sv[2];
        int type = (a1 & 0xf) == 1 ? SOCK_STREAM : SOCK_DGRAM;
        if (::socketpair(AF_UNIX, type, 0, sv) != 0) return -host_errno_to_linux(errno);
        for (int i = 0; i < 2; i++) {
            fcntl(sv[i], F_SETFL, O_NONBLOCK);
            fcntl(sv[i], F_SETFD, FD_CLOEXEC);
            auto s = std::make_shared<SockFile>();
            s->fd = sv[i];
            s->flags = 2 | ((a1 & lnx::L_O_NONBLOCK) ? lnx::L_O_NONBLOCK : 0);
            s->path = "socket:[" + std::to_string(sv[i]) + "]";
            m.wr<s32>(a3 + i * 4, p.fds->install(s, a1 & lnx::L_O_CLOEXEC));
        }
        return 0;
    }
    case 282: case 283: {  // bind / connect
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        sockaddr_storage ss;
        std::string gp;
        int hl = to_host_addr(t, s->domain, a1, a2, ss, &gp, nr == 282);
        if (hl < 0) return hl;
        if (s->domain == 16) return 0;  // netlink bind
        if (nr == 282) {
            if (ss.ss_family == AF_UNIX) ::unlink(((sockaddr_un*)&ss)->sun_path);
            if (::bind(s->fd, (sockaddr*)&ss, hl) != 0) return -host_errno_to_linux(errno);
            return 0;
        }
        int r = ::connect(s->fd, (sockaddr*)&ss, hl);
        if (r == 0) return 0;
        if (errno == EINPROGRESS || errno == EAGAIN) {
            if (s->flags & lnx::L_O_NONBLOCK) return -lnx::EINPROGRESS_;
            int w = wait_fd(t, s->fd, POLLOUT);
            if (w == -lnx::EINTR_) return -lnx::EINTR_;
            int e = 0;
            socklen_t el = sizeof e;
            getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &el);
            return e ? -host_errno_to_linux(e) : 0;
        }
        if (errno == ENOENT && ss.ss_family == AF_UNIX) return -lnx::ENOENT_;
        return -host_errno_to_linux(errno);
    }
    case 284: {  // listen
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        return ::listen(s->fd, (int)a1) ? -host_errno_to_linux(errno) : 0;
    }
    case 285: case 366: {  // accept / accept4
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        while (true) {
            sockaddr_storage ss;
            socklen_t sl = sizeof ss;
            int c = ::accept(s->fd, (sockaddr*)&ss, &sl);
            if (c >= 0) {
                fcntl(c, F_SETFL, O_NONBLOCK);
                fcntl(c, F_SETFD, FD_CLOEXEC);
                int one = 1;
                setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
                auto ns = std::make_shared<SockFile>();
                ns->fd = c;
                ns->domain = s->domain;
                u32 fl = nr == 366 ? a3 : 0;
                ns->flags = 2 | (fl & lnx::L_O_NONBLOCK);
                ns->path = "socket:[" + std::to_string(c) + "]";
                if (a1) from_host_addr(t, (sockaddr*)&ss, a1, a2);
                return p.fds->install(ns, fl & lnx::L_O_CLOEXEC);
            }
            if (errno == EAGAIN) {
                s32 w = sock_wait(t, *s, POLLIN);
                if (w < 0) return w;
                continue;
            }
            return -host_errno_to_linux(errno);
        }
    }
    case 286: case 287: {  // getsockname / getpeername
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int r = nr == 286 ? getsockname(s->fd, (sockaddr*)&ss, &sl) : getpeername(s->fd, (sockaddr*)&ss, &sl);
        if (r) return -host_errno_to_linux(errno);
        from_host_addr(t, (sockaddr*)&ss, a1, a2);
        return 0;
    }
    case 289: case 290: {  // send / sendto(fd, buf, len, flags, addr, alen)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        sockaddr_storage ss;
        int hl = 0;
        if (nr == 290 && a4) {
            hl = to_host_addr(t, s->domain, a4, a5, ss, nullptr, false);
            if (hl < 0) return hl;
        }
        while (true) {
            ssize_t r = hl ? ::sendto(s->fd, m.ptr(a1), a2, msg_flags(a3), (sockaddr*)&ss, hl)
                           : ::send(s->fd, m.ptr(a1), a2, msg_flags(a3));
            if (r >= 0) return (s32)r;
            if (errno == EAGAIN && !(a3 & 0x40)) {
                s32 w = sock_wait(t, *s, POLLOUT);
                if (w < 0) return w;
                continue;
            }
            if (errno == ENOBUFS) { usleep(1000); continue; }
            return -host_errno_to_linux(errno);
        }
    }
    case 291: case 292: {  // recv / recvfrom(fd, buf, len, flags, addr, alen*)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        while (true) {
            sockaddr_storage ss;
            socklen_t sl = sizeof ss;
            ssize_t r = ::recvfrom(s->fd, m.ptr(a1), a2, msg_flags(a3), (sockaddr*)&ss, &sl);
            if (r >= 0) {
                if (nr == 292 && a4) {
                    if (sl > 0) from_host_addr(t, (sockaddr*)&ss, a4, a5);
                    else if (a5) m.wr<u32>(a5, 0);
                }
                return (s32)r;
            }
            if (errno == EAGAIN && !(a3 & 0x40)) {
                s32 w = sock_wait(t, *s, POLLIN);
                if (w < 0) return w;
                continue;
            }
            return -host_errno_to_linux(errno);
        }
    }
    case 293: {
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        ::shutdown(s->fd, a1 == 0 ? SHUT_RD : a1 == 1 ? SHUT_WR : SHUT_RDWR);
        return 0;
    }
    case 294: {  // setsockopt(fd, level, name, val, len)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (a1 == 1) {  // SOL_SOCKET
            int hn = 0;
            switch (a2) {
            case 2: hn = SO_REUSEADDR; break;
            case 7: hn = SO_SNDBUF; break;
            case 8: hn = SO_RCVBUF; break;
            case 9: hn = SO_KEEPALIVE; break;
            case 6: hn = SO_BROADCAST; break;
            default: return 0;
            }
            int v = m.rd<s32>(a3);
            setsockopt(s->fd, SOL_SOCKET, hn, &v, sizeof v);
            return 0;
        }
        if (a1 == 6 && a2 == 1) {
            int v = m.rd<s32>(a3);
            setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
        }
        return 0;
    }
    case 295: {  // getsockopt(fd, level, name, val, len*)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (a1 == 1 && a2 == 4) {  // SO_ERROR
            int e = 0;
            socklen_t el = sizeof e;
            getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &el);
            m.wr<s32>(a3, e ? host_errno_to_linux(e) : 0);
            m.wr<u32>(a4, 4);
            return 0;
        }
        if (a1 == 1 && a2 == 17) {  // SO_PEERCRED
            m.wr<s32>(a3, p.pid);
            m.wr<u32>(a3 + 4, 0);
            m.wr<u32>(a3 + 8, 0);
            m.wr<u32>(a4, 12);
            return 0;
        }
        if (a1 == 1 && a2 == 3) {  // SO_TYPE
            int ty = 0;
            socklen_t tl = sizeof ty;
            getsockopt(s->fd, SOL_SOCKET, SO_TYPE, &ty, &tl);
            m.wr<s32>(a3, ty == SOCK_STREAM ? 1 : 2);
            m.wr<u32>(a4, 4);
            return 0;
        }
        m.wr<s32>(a3, 65536);
        m.wr<u32>(a4, 4);
        return 0;
    }
    case 296: case 297: {  // sendmsg / recvmsg(fd, msghdr*, flags)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        u32 name = m.rd<u32>(a1), namelen = m.rd<u32>(a1 + 4), iov = m.rd<u32>(a1 + 8), iovlen = m.rd<u32>(a1 + 12);
        std::vector<iovec> hv(iovlen);
        for (u32 i = 0; i < iovlen; i++) {
            hv[i].iov_base = m.ptr(m.rd<u32>(iov + i * 8));
            hv[i].iov_len = m.rd<u32>(iov + i * 8 + 4);
        }
        msghdr mh{};
        sockaddr_storage ss;
        mh.msg_iov = hv.data();
        mh.msg_iovlen = (int)iovlen;
        if (nr == 296 && name) {
            int hl = to_host_addr(t, s->domain, name, namelen, ss, nullptr, false);
            if (hl < 0) return hl;
            mh.msg_name = &ss;
            mh.msg_namelen = hl;
        } else if (nr == 297) {
            mh.msg_name = &ss;
            mh.msg_namelen = sizeof ss;
        }
        while (true) {
            ssize_t r = nr == 296 ? ::sendmsg(s->fd, &mh, msg_flags(a2)) : ::recvmsg(s->fd, &mh, msg_flags(a2));
            if (r >= 0) {
                if (nr == 297) {
                    if (name && mh.msg_namelen) from_host_addr(t, (sockaddr*)&ss, name, a1 + 4);
                    m.wr<u32>(a1 + 20, 0);  // controllen
                    m.wr<u32>(a1 + 24, 0);
                }
                return (s32)r;
            }
            if (errno == EAGAIN && !(a2 & 0x40)) {
                s32 w = sock_wait(t, *s, nr == 296 ? POLLOUT : POLLIN);
                if (w < 0) return w;
                continue;
            }
            return -host_errno_to_linux(errno);
        }
    }
    }
    return -lnx::ENOSYS_;
}
