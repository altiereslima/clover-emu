// Linux syscalls part 2: memory, futex, AF_ALG. Sockets / poll / epoll / eventfd live in ipc.cpp.
#include "syscalls.h"
#include "hash.h"
#include <map>
#include <list>
#include <algorithm>

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
    if (f && !(flags & lnx::L_MAP_FIXED))
        if (u32 fixed = f->mmap_fixed(off)) where = fixed;
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
        u32 cur = atomic_load_raw((u32*)as->ptr(addr));
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
        if (cmd == 4 && atomic_load_raw((u32*)m.ptr(uaddr)) != val3) return -lnx::EAGAIN_;
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
        u32 old = atomic_load_raw(p2), nv;
        do {
            switch (opn) {
            case 0: nv = oparg; break;
            case 1: nv = old + oparg; break;
            case 2: nv = old | oparg; break;
            case 3: nv = old & ~oparg; break;
            case 4: nv = old ^ oparg; break;
            default: return -lnx::ENOSYS_;
            }
        } while (!atomic_cas_raw(p2, &old, nv));
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
            if (atomic_cas_raw(pp, &cur, (u32)t.tid)) return 0;
            if ((cur & 0x3fffffff) == (u32)t.tid) return -lnx::EDEADLK_;
            if (cmd == 8) return -lnx::EAGAIN_;
            u32 nv = cur | 0x80000000u;
            if (!atomic_cas_raw(pp, &cur, nv)) continue;
            s32 r = futex_wait(t, uaddr, nv, 0, false, 0xffffffff);
            if (r == -lnx::EINTR_) return r;
        }
    }
    case 7: {  // UNLOCK_PI
        u32* pp = (u32*)m.ptr(uaddr);
        atomic_store_raw(pp, 0u);
        futex_wake(*t.proc, uaddr, 1, 0xffffffff);
        return 0;
    }
    }
    LOGI("[futex] unsupported op %u\n", op);
    return -lnx::ENOSYS_;
}

// ---------------------------------------------------------------- AF_ALG
namespace {
// AF_ALG (Linux kernel crypto API) — hash algorithms only.
struct AlgOp : File {
    std::string alg;
    Sha1 sha1;
    Sha256 sha256;
    Md5 md5;
    bool started = false;
    std::vector<u8> digest;  // pending output after finalization
    size_t digest_pos = 0;
    void reset() {
        if (alg == "sha1") sha1.init();
        else if (alg == "sha256") sha256.init();
        else md5.init();
        started = true;
    }
    void update(const u8* p, u32 n) {
        if (!started || !digest.empty()) {
            digest.clear();
            digest_pos = 0;
            reset();
        }
        if (alg == "sha1") sha1.update(p, n);
        else if (alg == "sha256") sha256.update(p, n);
        else md5.update(p, n);
    }
    void final() {
        if (!started) reset();
        if (alg == "sha1") { digest.resize(20); sha1.final(digest.data()); }
        else if (alg == "sha256") { digest.resize(32); sha256.final(digest.data()); }
        else { digest.resize(16); md5.final(digest.data()); }
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

}  // namespace

s32 do_net_syscall(Thread& t, u32 nr, bool& noret) {
    auto& R = t.R();
    Process& p = *t.proc;
    AddressSpace& m = *p.mem;
    u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
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
    return ipc_syscall(t, nr);
}
