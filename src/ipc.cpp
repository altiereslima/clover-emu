// In-emulator IPC: pipes, FIFOs, sockets (AF_UNIX, loopback AF_INET/AF_INET6, inert AF_NETLINK),
// eventfd, inotify stand-in, and the poll/select/epoll core.
//
// Every guest process lives inside this one host process, so none of this needs host kernel objects:
// all state is plain memory guarded by one mutex, and blocked threads wait on a global "I/O
// generation" condition variable that is bumped whenever readiness may have changed (io_notify) or a
// thread is kicked for signal delivery. This behaves the same on every host OS, including Windows,
// where pipes and AF_UNIX sockets cannot be poll()ed.
#include "kernel.h"
#include <deque>
#include <map>
#include <algorithm>

extern std::mutex g_sig_mtx;

namespace {

// ---------------------------------------------------------------- waiting
std::mutex g_io_mtx;
std::condition_variable g_io_cv;
u64 g_io_gen = 0;
std::recursive_mutex g_ipc;  // all pipe / socket / eventfd state
using Lock = std::lock_guard<std::recursive_mutex>;

constexpr u64 MAX_WAIT_NS = 50000000ull;  // safety net for files that change without io_notify()

u64 io_gen() {
    std::lock_guard lk(g_io_mtx);
    return g_io_gen;
}

void io_wait(u64 gen, u64 max_ns = MAX_WAIT_NS) {
    std::unique_lock lk(g_io_mtx);
    g_io_cv.wait_for(lk, std::chrono::nanoseconds(max_ns), [&] { return g_io_gen != gen; });
}

void raise_sigpipe(Thread& t) {
    SigInfo si;
    si.signo = lnx::L_SIGPIPE;
    send_signal_thread(t, si);
}

// ---------------------------------------------------------------- byte ring
struct Ring {
    std::vector<u8> buf;
    size_t head = 0, len = 0;
    explicit Ring(size_t cap) : buf(cap) {}
    size_t space() const { return buf.size() - len; }
    size_t push(const u8* p, size_t n) {
        n = std::min(n, space());
        size_t tail = (head + len) % buf.size();
        size_t first = std::min(n, buf.size() - tail);
        memcpy(buf.data() + tail, p, first);
        memcpy(buf.data(), p + first, n - first);
        len += n;
        return n;
    }
    size_t pop(u8* p, size_t n) {
        n = std::min(n, len);
        size_t first = std::min(n, buf.size() - head);
        memcpy(p, buf.data() + head, first);
        memcpy(p + first, buf.data(), n - first);
        head = (head + n) % buf.size();
        len -= n;
        return n;
    }
};

// ---------------------------------------------------------------- pipes / FIFOs
constexpr size_t PIPE_CAP = 65536, PIPE_ATOMIC = 4096;
std::atomic<u32> g_pipe_ids{1000};

struct PipeBuf {
    Ring ring{PIPE_CAP};
    int readers = 0, writers = 0;
    u32 id = g_pipe_ids++;
};
std::map<std::string, std::weak_ptr<PipeBuf>> g_fifos;

struct PipeEnd : File {
    std::shared_ptr<PipeBuf> b;
    bool rd, wr;
    PipeEnd(std::shared_ptr<PipeBuf> pb, bool r, bool w) : b(std::move(pb)), rd(r), wr(w) {
        Lock lk(g_ipc);
        if (rd) b->readers++;
        if (wr) b->writers++;
    }
    ~PipeEnd() override {
        {
            Lock lk(g_ipc);
            if (rd) b->readers--;
            if (wr) b->writers--;
        }
        io_notify();
    }
    s64 read(Thread& t, u8* buf, u32 n) override {
        if (!rd) return -lnx::EBADF_;
        if (!n) return 0;
        while (true) {
            u64 gen = io_gen();
            size_t got = 0;
            bool eof = false;
            {
                Lock lk(g_ipc);
                got = b->ring.pop(buf, n);
                eof = !got && b->writers == 0;
            }
            if (got) {
                io_notify();
                return (s64)got;
            }
            if (eof) return 0;
            if (flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
            if (t.signal_pending()) return -ERESTARTSYS;
            io_wait(gen);
        }
    }
    s64 write(Thread& t, const u8* buf, u32 n) override {
        if (!wr) return -lnx::EBADF_;
        u32 done = 0;
        while (true) {
            u64 gen = io_gen();
            bool broken = false;
            size_t put = 0;
            {
                Lock lk(g_ipc);
                if (b->readers == 0) broken = true;
                else if (n <= PIPE_ATOMIC ? b->ring.space() >= n : b->ring.space() > 0)
                    put = b->ring.push(buf + done, n - done);
            }
            if (broken) {
                raise_sigpipe(t);
                return done ? (s64)done : -lnx::EPIPE_;
            }
            if (put) {
                done += (u32)put;
                io_notify();
            }
            if (done == n) return n;
            if (flags & lnx::L_O_NONBLOCK) return done ? (s64)done : -lnx::EAGAIN_;
            if (t.signal_pending()) return done ? (s64)done : -ERESTARTSYS;
            if (!put) io_wait(gen);
        }
    }
    u32 poll_mask() override {
        Lock lk(g_ipc);
        u32 m = 0;
        if (rd) {
            if (b->ring.len) m |= 1;
            if (b->writers == 0) m |= 0x10;
        }
        if (wr) {
            if (b->readers == 0) m |= 8;
            else if (b->ring.space() >= PIPE_ATOMIC) m |= 4;
        }
        return m;
    }
    int ioctl(Thread& t, u32 req, u32 arg) override {
        if (req == 0x541B) {  // FIONREAD
            Lock lk(g_ipc);
            t.proc->mem->wr<s32>(arg, (s32)b->ring.len);
            return 0;
        }
        return -lnx::ENOTTY_;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFIFO | 0600, 0, 0, b->id);
        return 0;
    }
    std::string link_name() override { return path.empty() || path.compare(0, 5, "pipe:") == 0 ? "pipe:[" + std::to_string(b->id) + "]" : path; }
};

// ---------------------------------------------------------------- eventfd / inotify stand-in
struct EventFd : File {
    u64 count = 0;
    bool semaphore = false;
    s64 read(Thread& t, u8* buf, u32 n) override {
        if (n < 8) return -lnx::EINVAL_;
        while (true) {
            u64 gen = io_gen();
            {
                Lock lk(g_ipc);
                if (count) {
                    u64 v = semaphore ? 1 : count;
                    count -= v;
                    memcpy(buf, &v, 8);
                    io_notify();
                    return 8;
                }
            }
            if (flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
            if (t.signal_pending()) return -ERESTARTSYS;
            io_wait(gen);
        }
    }
    s64 write(Thread&, const u8* buf, u32 n) override {
        if (n < 8) return -lnx::EINVAL_;
        u64 v;
        memcpy(&v, buf, 8);
        if (v == ~0ull) return -lnx::EINVAL_;
        {
            Lock lk(g_ipc);
            count += v;
        }
        io_notify();
        return 8;
    }
    u32 poll_mask() override {
        Lock lk(g_ipc);
        return (count ? 1u : 0u) | 4u;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

struct DummyFd : File {  // inotify: never reports events
    s64 read(Thread& t, u8*, u32) override {
        if (flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
        while (!t.signal_pending()) io_wait(io_gen());
        return -ERESTARTSYS;
    }
    u32 poll_mask() override { return 0; }
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

// ---------------------------------------------------------------- epoll
struct EpollFile : File {
    struct Ent {
        u32 events;
        u64 data;
        std::weak_ptr<File> f;
    };
    std::mutex mtx;
    std::map<int, Ent> ents;
    u32 poll_mask() override {
        std::vector<std::pair<u32, FilePtr>> v;
        {
            std::lock_guard lk(mtx);
            for (auto& [fd, e] : ents)
                if (auto f = e.f.lock()) v.push_back({e.events, f});
        }
        for (auto& [ev, f] : v) {
            u32 m = f->poll_mask();
            if (((m & 1) && (ev & 0x41)) || ((m & 4) && (ev & 0x104)) || (m & 0x18)) return 1;
        }
        return 0;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(0600);
        return 0;
    }
};

// ---------------------------------------------------------------- sockets
constexpr size_t SOCK_CAP = 212992;
std::atomic<u32> g_sock_ids{5000};

struct Msg {
    std::vector<u8> data;
    size_t off = 0;
    std::vector<u8> from;  // sender's guest sockaddr (datagrams)
};
struct SockQ {
    std::deque<Msg> msgs;
    size_t bytes = 0;
    bool eof = false;          // the writer is gone / shut down: no more data will arrive
    bool reader_gone = false;  // the reading socket was closed: writes fail with EPIPE
};

enum { SK_STREAM = 1, SK_DGRAM = 2, SK_SEQPACKET = 5 };
enum { AFAM_UNIX = 1, AFAM_INET = 2, AFAM_INET6 = 10, AFAM_NETLINK = 16 };

struct Sock;
std::map<std::string, std::weak_ptr<Sock>> g_socks;  // bound address key -> socket

struct Sock : File {
    int domain = AFAM_UNIX, type = SK_STREAM;
    enum State { Fresh, Listening, Connected } st = Fresh;
    u32 id = g_sock_ids++;
    std::string key;                 // registry key while bound
    std::vector<u8> local, peer;     // guest sockaddrs
    std::string peer_key;            // connected datagram destination
    std::shared_ptr<SockQ> rx = std::make_shared<SockQ>();
    std::shared_ptr<SockQ> tx;       // the peer's rx (connected stream / seqpacket / socketpair)
    std::deque<std::shared_ptr<Sock>> backlog;
    int backlog_max = 16;
    bool shut_rd = false, shut_wr = false;

    ~Sock() override {
        {
            Lock lk(g_ipc);
            if (!key.empty()) {
                auto it = g_socks.find(key);
                if (it != g_socks.end() && it->second.expired()) g_socks.erase(it);
            }
            if (tx) tx->eof = true;
            rx->reader_gone = true;
            backlog.clear();
        }
        io_notify();
    }
    bool is_socket() const override { return true; }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFSOCK | 0777, 0, 0, id);
        return 0;
    }
    std::string link_name() override { return "socket:[" + std::to_string(id) + "]"; }
    bool stream_like() const { return type == SK_STREAM || type == SK_SEQPACKET; }
    u32 poll_mask() override {
        Lock lk(g_ipc);
        if (domain == AFAM_NETLINK) return 4;
        if (st == Listening) return backlog.empty() ? 0 : 1;
        u32 m = 0;
        if (!rx->msgs.empty() || shut_rd) m |= 1;
        if (stream_like()) {
            if (st != Connected) return 4;
            if (rx->eof) m |= 1 | 0x10 | 0x2000;
            if (!tx || tx->reader_gone || shut_wr) m |= 4 | 8;
            else if (tx->bytes < SOCK_CAP) m |= 4;
        } else {
            m |= 4;
        }
        return m;
    }
    s64 read(Thread& t, u8* buf, u32 n) override;
    s64 write(Thread& t, const u8* buf, u32 n) override;
    int ioctl(Thread& t, u32 req, u32 arg) override {
        if (req == 0x541B) {  // FIONREAD
            Lock lk(g_ipc);
            size_t n = 0;
            if (stream_like()) n = rx->bytes;
            else if (!rx->msgs.empty()) n = rx->msgs.front().data.size();
            t.proc->mem->wr<s32>(arg, (s32)n);
            return 0;
        }
        return -lnx::ENOTTY_;
    }
};

std::shared_ptr<Sock> make_sock(int domain, int type, u32 nb) {
    auto s = std::make_shared<Sock>();
    s->domain = domain;
    s->type = type;
    s->flags = 2 | (nb ? lnx::L_O_NONBLOCK : 0);
    s->path = "socket:[" + std::to_string(s->id) + "]";
    u16 fam = (u16)domain;
    s->local.assign((u8*)&fam, (u8*)&fam + 2);
    return s;
}

std::shared_ptr<Sock> lookup(const std::string& key) {
    auto it = g_socks.find(key);
    if (it == g_socks.end()) return nullptr;
    auto s = it->second.lock();
    if (!s) g_socks.erase(it);
    return s;
}

// ---- addresses
struct GAddr {
    std::string key;
    std::vector<u8> raw;
    bool loopback = true;  // inet: destination is this machine
    bool unix_path = false;
};

std::string inet_key(int fam, const u8* ip, size_t iplen, u16 port_be) {
    char b[80];
    std::string s = fam == AFAM_INET ? "4:" : "6:";
    for (size_t i = 0; i < iplen; i++) {
        snprintf(b, sizeof b, "%02x", ip[i]);
        s += b;
    }
    snprintf(b, sizeof b, ":%u", (unsigned)port_be);
    return s + b;
}

// Parse a guest sockaddr. Returns 0 or -errno.
int parse_addr(Thread& t, int domain, u32 addr, u32 len, GAddr& out, bool for_bind) {
    AddressSpace& m = *t.proc->mem;
    if (len < 2) return -lnx::EINVAL_;
    u16 fam = m.rd<u16>(addr);
    if (domain == AFAM_NETLINK) return 0;
    if (fam != domain) return -lnx::EAFNOSUPPORT_;
    if (fam == AFAM_UNIX) {
        out.raw.assign(m.ptr(addr), m.ptr(addr) + std::min<u32>(len, 110));
        if (len == 2) return -lnx::EINVAL_;  // autobind handled by caller
        const char* pth = (const char*)m.ptr(addr + 2);
        if (pth[0] == 0) {
            out.key = "@" + std::string(pth + 1, len - 3);
            return 0;
        }
        std::string gp(pth, strnlen(pth, std::min<u32>(len - 2, 108)));
        Resolved r = vfs::resolve(*t.proc, gp, true);
        if (r.err && r.err != lnx::ENOENT_) return -r.err;
        std::string canon = r.guest.empty() ? vfs::join(t.proc->cwd, gp) : r.guest;
        out.key = "u:" + canon;
        out.unix_path = true;
        if (for_bind && r.kind == Resolved::HOST && r.writable) {
            int fd = hfs::open(r.host, lnx::L_O_CREAT | 1u, 0666);
            if (fd >= 0) hfs::close(fd);
        } else if (!for_bind && (r.err == lnx::ENOENT_ || (r.kind == Resolved::HOST && !hfs::exists(r.host)))) {
            out.key += "\x01missing";  // marks "path does not exist" for error reporting
        }
        return 0;
    }
    if (fam == AFAM_INET) {
        if (len < 8) return -lnx::EINVAL_;
        out.raw.assign(16, 0);
        memcpy(out.raw.data(), m.ptr(addr), 8);
        const u8* ip = out.raw.data() + 4;
        u16 port;
        memcpy(&port, out.raw.data() + 2, 2);
        out.loopback = ip[0] == 127 || (ip[0] | ip[1] | ip[2] | ip[3]) == 0;
        out.key = inet_key(fam, ip, 4, port);
        return 0;
    }
    if (fam == AFAM_INET6) {
        if (len < 24) return -lnx::EINVAL_;
        out.raw.assign(28, 0);
        memcpy(out.raw.data(), m.ptr(addr), std::min<u32>(len, 28));
        const u8* ip = out.raw.data() + 8;
        u16 port;
        memcpy(&port, out.raw.data() + 2, 2);
        bool any = std::all_of(ip, ip + 16, [](u8 c) { return c == 0; });
        bool lo = std::all_of(ip, ip + 15, [](u8 c) { return c == 0; }) && ip[15] == 1;
        out.loopback = any || lo;
        out.key = inet_key(fam, ip, 16, port);
        return 0;
    }
    return -lnx::EAFNOSUPPORT_;
}

// Pick a free ephemeral port for an inet socket and register it. Caller holds g_ipc.
void inet_autobind(Sock& s, const std::shared_ptr<Sock>& self) {
    if (!s.key.empty()) return;
    size_t iplen = s.domain == AFAM_INET ? 4 : 16;
    size_t ipoff = s.domain == AFAM_INET ? 4 : 8;
    std::vector<u8> raw(s.domain == AFAM_INET ? 16 : 28, 0);
    u16 fam = (u16)s.domain;
    memcpy(raw.data(), &fam, 2);
    if (s.domain == AFAM_INET) raw[4] = 127, raw[7] = 1;
    else raw[ipoff + 15] = 1;
    for (u32 p = 40000; p < 65000; p++) {
        u16 be = (u16)(((p & 0xff) << 8) | (p >> 8));
        std::string k = inet_key(s.domain, raw.data() + ipoff, iplen, be);
        if (lookup(k)) continue;
        memcpy(raw.data() + 2, &be, 2);
        s.local = raw;
        s.key = k;
        g_socks[k] = self;
        return;
    }
}

// Find the socket bound to `a` (exact address, then the wildcard address on the same port).
std::shared_ptr<Sock> find_target(int domain, const GAddr& a) {
    if (auto s = lookup(a.key)) return s;
    if (domain == AFAM_INET || domain == AFAM_INET6) {
        size_t iplen = domain == AFAM_INET ? 4 : 16, ipoff = domain == AFAM_INET ? 4 : 8;
        u16 port;
        memcpy(&port, a.raw.data() + 2, 2);
        std::vector<u8> zero(iplen, 0), lo(iplen, 0);
        if (domain == AFAM_INET) lo[0] = 127, lo[3] = 1;
        else lo[15] = 1;
        if (auto s = lookup(inet_key(domain, zero.data(), iplen, port))) return s;
        if (a.loopback)
            if (auto s = lookup(inet_key(domain, lo.data(), iplen, port))) return s;
        (void)ipoff;
    }
    return nullptr;
}

std::shared_ptr<Sock> get_sock(Thread& t, int fd, s32* err) {
    FilePtr f = t.proc->fds->get(fd);
    if (!f) { *err = -lnx::EBADF_; return nullptr; }
    auto s = std::dynamic_pointer_cast<Sock>(f);
    if (!s) { *err = -lnx::ENOTSOCK_; return nullptr; }
    return s;
}

void put_addr(Thread& t, const std::vector<u8>& raw, u32 addrp, u32 lenp) {
    if (!addrp) return;
    AddressSpace& m = *t.proc->mem;
    u32 maxlen = lenp ? m.rd<u32>(lenp) : 0;
    m.write_bytes(addrp, raw.data(), std::min<size_t>(raw.size(), maxlen));
    if (lenp) m.wr<u32>(lenp, (u32)raw.size());
}

// ---- data path
// Send `data` on s (to `dest` for unconnected datagrams). Returns bytes or -errno.
s64 sock_send(Thread& t, const std::shared_ptr<Sock>& sp, const u8* data, size_t n, const GAddr* dest, u32 lflags) {
    Sock& s = *sp;
    bool nb = (s.flags & lnx::L_O_NONBLOCK) || (lflags & 0x40);
    bool nosig = lflags & 0x4000;  // MSG_NOSIGNAL
    if (s.domain == AFAM_NETLINK) return (s64)n;
    if (s.stream_like()) {
        size_t done = 0;
        while (true) {
            u64 gen = io_gen();
            s64 err = 0;
            size_t put = 0;
            {
                Lock lk(g_ipc);
                if (s.st != Sock::Connected || !s.tx) err = s.st == Sock::Fresh ? -lnx::ENOTCONN_ : -lnx::EPIPE_;
                else if (s.shut_wr || s.tx->reader_gone) err = -lnx::EPIPE_;
                else {
                    size_t space = s.tx->bytes < SOCK_CAP ? SOCK_CAP - s.tx->bytes : 0;
                    bool ok = s.type == SK_SEQPACKET ? space >= n || s.tx->bytes == 0 : space > 0;
                    if (ok) {
                        put = s.type == SK_SEQPACKET ? n : std::min(space, n - done);
                        Msg msg;
                        msg.data.assign(data + done, data + done + put);
                        s.tx->msgs.push_back(std::move(msg));
                        s.tx->bytes += put;
                    }
                }
            }
            if (err == -lnx::EPIPE_ && !nosig) raise_sigpipe(t);
            if (err) return done ? (s64)done : err;
            if (put) {
                done += put;
                io_notify();
            }
            if (done >= n) return (s64)n;
            if (nb) return done ? (s64)done : -lnx::EAGAIN_;
            if (t.signal_pending()) return done ? (s64)done : -ERESTARTSYS;
            if (!put) io_wait(gen);
        }
    }
    // datagram
    while (true) {
        u64 gen = io_gen();
        {
            Lock lk(g_ipc);
            std::shared_ptr<SockQ> q;
            if (dest) {
                auto tgt = find_target(s.domain, *dest);
                if (!tgt) {
                    if (s.domain == AFAM_UNIX) return dest->key.find('\x01') != std::string::npos ? -lnx::ENOENT_ : -lnx::ECONNREFUSED_;
                    return (s64)n;  // UDP to nobody: dropped
                }
                q = tgt->rx;
            } else if (s.tx) {
                if (s.tx->reader_gone) return -lnx::ECONNREFUSED_;
                q = s.tx;
            } else if (!s.peer_key.empty()) {
                auto tgt = lookup(s.peer_key);
                if (!tgt) return s.domain == AFAM_UNIX ? -lnx::ECONNREFUSED_ : (s64)n;
                q = tgt->rx;
            } else {
                return -lnx::EDESTADDRREQ_;
            }
            if (s.domain == AFAM_INET || s.domain == AFAM_INET6) inet_autobind(s, sp);
            if (q->bytes + n <= SOCK_CAP || q->msgs.empty()) {
                Msg msg;
                msg.data.assign(data, data + n);
                msg.from = s.local;
                q->msgs.push_back(std::move(msg));
                q->bytes += n;
                io_notify();
                return (s64)n;
            }
        }
        if (nb) return -lnx::EAGAIN_;
        if (t.signal_pending()) return -ERESTARTSYS;
        io_wait(gen);
    }
}

// Receive into buf. `from` gets the sender address for datagrams.
s64 sock_recv(Thread& t, Sock& s, u8* buf, size_t n, u32 lflags, std::vector<u8>* from) {
    bool nb = (s.flags & lnx::L_O_NONBLOCK) || (lflags & 0x40);
    bool peek = lflags & 2, waitall = (lflags & 0x100) && !peek;
    size_t done = 0;
    while (true) {
        u64 gen = io_gen();
        s64 result = -1;  // -1: keep waiting
        bool consumed = false;
        {
            Lock lk(g_ipc);
            if (s.domain == AFAM_NETLINK) {
                // never any messages
            } else if (s.st == Sock::Listening) {
                result = -lnx::ENOTCONN_;
            } else if (s.stream_like() && s.st != Sock::Connected) {
                result = -lnx::ENOTCONN_;
            } else if (!s.rx->msgs.empty()) {
                auto& q = *s.rx;
                if (s.type == SK_STREAM) {
                    size_t i = 0, skip = 0;
                    while (done < n && i < q.msgs.size()) {
                        Msg& mm = q.msgs[i];
                        size_t avail = mm.data.size() - mm.off - (peek ? skip : 0);
                        size_t c = std::min(avail, n - done);
                        memcpy(buf + done, mm.data.data() + mm.off + (peek ? skip : 0), c);
                        done += c;
                        if (peek) {
                            i++;
                            skip = 0;
                            continue;
                        }
                        mm.off += c;
                        q.bytes -= c;
                        consumed = true;
                        if (mm.off == mm.data.size()) q.msgs.pop_front();
                    }
                    if (!waitall || done >= n || q.eof) result = (s64)done;
                } else {
                    Msg& mm = q.msgs.front();
                    size_t full = mm.data.size();
                    size_t c = std::min(full, n);
                    memcpy(buf, mm.data.data(), c);
                    if (from) *from = mm.from;
                    if (!peek) {
                        q.bytes -= full;
                        q.msgs.pop_front();
                        consumed = true;
                    }
                    result = (s64)((lflags & 0x20) ? full : c);  // MSG_TRUNC: report the real length
                }
            } else if (s.shut_rd || (s.stream_like() && s.rx->eof)) {
                result = (s64)done;  // EOF
            }
        }
        if (consumed) io_notify();
        if (result >= 0 || result < -1) return result;
        if (nb) return done ? (s64)done : -lnx::EAGAIN_;
        if (t.signal_pending()) return done ? (s64)done : -ERESTARTSYS;
        io_wait(gen);
    }
}

s64 Sock::read(Thread& t, u8* buf, u32 n) { return sock_recv(t, *this, buf, n, 0, nullptr); }
s64 Sock::write(Thread& t, const u8* buf, u32 n) {
    return sock_send(t, std::static_pointer_cast<Sock>(shared_from_this()), buf, n, nullptr, 0);
}

// ---------------------------------------------------------------- poll core
struct PollItem {
    int gfd;
    u32 req;
    u32 res;
    FilePtr f;
};

u32 ready_bits(File& f, u32 req) {
    u32 m = f.poll_mask(), r = 0;
    if ((m & 1) && (req & 0x41)) r |= req & 0x41;
    if ((m & 4) && (req & 0x104)) r |= req & 0x104;
    if ((m & 2) && (req & 2)) r |= 2;
    if (m & 8) r |= 8;
    if (m & 0x10) r |= 0x10 | (req & 0x2000);
    return r;
}

// Polls items; timeout_ms < 0 = infinite. Returns number ready or -errno.
s32 do_poll(Thread& t, std::vector<PollItem>& items, s64 timeout_ms) {
    u64 deadline = timeout_ms >= 0 ? now_ns() + (u64)timeout_ms * 1000000ull : 0;
    while (true) {
        u64 gen = io_gen();
        int ready = 0;
        for (auto& it : items) {
            it.res = 0;
            if (it.gfd < 0) continue;
            if (!it.f) {
                it.res = 0x20;  // POLLNVAL
                ready++;
                continue;
            }
            it.res = ready_bits(*it.f, it.req);
            if (it.res) ready++;
        }
        if (ready) return ready;
        u64 wait = MAX_WAIT_NS;
        if (timeout_ms >= 0) {
            u64 n = now_ns();
            if (n >= deadline) return 0;
            wait = std::min(wait, deadline - n);
        }
        if (t.signal_pending()) return -ERESTARTNOHAND;
        io_wait(gen, wait);
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

s64 timespec_ms(AddressSpace& m, u32 tsp) {
    if (!tsp) return -1;
    return (s64)m.rd<u32>(tsp) * 1000 + m.rd<u32>(tsp + 4) / 1000000;
}

}  // namespace

// ---------------------------------------------------------------- public entry points
void io_notify() {
    {
        std::lock_guard lk(g_io_mtx);
        g_io_gen++;
    }
    g_io_cv.notify_all();
}

void ipc_make_pipe(FilePtr& rd, FilePtr& wr, bool nonblock) {
    auto b = std::make_shared<PipeBuf>();
    auto r = std::make_shared<PipeEnd>(b, true, false);
    auto w = std::make_shared<PipeEnd>(b, false, true);
    r->flags = 0 | (nonblock ? lnx::L_O_NONBLOCK : 0);
    w->flags = 1 | (nonblock ? lnx::L_O_NONBLOCK : 0);
    r->path = w->path = "pipe:[" + std::to_string(b->id) + "]";
    rd = r;
    wr = w;
}

FilePtr ipc_open_fifo(Thread& t, const std::string& key, u32 flags, int* err) {
    u32 acc = flags & 3;
    bool nb = flags & lnx::L_O_NONBLOCK;
    std::shared_ptr<PipeEnd> end;
    {
        Lock lk(g_ipc);
        std::shared_ptr<PipeBuf> b = g_fifos[key].lock();
        if (!b) {
            b = std::make_shared<PipeBuf>();
            g_fifos[key] = b;
        }
        if (acc == 1 && nb && b->readers == 0) {
            *err = lnx::ENXIO_;
            return nullptr;
        }
        end = std::make_shared<PipeEnd>(b, acc != 1, acc != 0);
        end->flags = flags & (lnx::L_O_ACCMODE | lnx::L_O_NONBLOCK);
    }
    io_notify();
    // POSIX: a blocking open of one end waits for the other end.
    if (!nb && acc != 2) {
        while (true) {
            u64 gen = io_gen();
            {
                Lock lk(g_ipc);
                if (acc == 0 ? end->b->writers > 0 : end->b->readers > 0) break;
            }
            if (t.signal_pending()) {
                *err = lnx::EINTR_;
                return nullptr;
            }
            io_wait(gen);
        }
    }
    return end;
}

s32 ipc_syscall(Thread& t, u32 nr) {
    auto& R = t.R();
    Process& p = *t.proc;
    AddressSpace& m = *p.mem;
    u32 a0 = R[0], a1 = R[1], a2 = R[2], a3 = R[3], a4 = R[4], a5 = R[5];
    s32 err = 0;
    switch (nr) {
    case 168: return sys_poll(t, a0, a1, (s32)a2);
    case 336: {  // ppoll(fds, n, tsp, sigmask, size)
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
    case 335: return sys_select(t, a0, a1, a2, a3, timespec_ms(m, a4));  // pselect6
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
    case 252: case 346: return sys_epoll_wait(t, a0, a1, a2, (s32)a3);
    case 351: case 356: {  // eventfd / eventfd2
        auto e = std::make_shared<EventFd>();
        e->count = a0;
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
        if (a0 != AFAM_UNIX && a0 != AFAM_INET && a0 != AFAM_INET6 && a0 != AFAM_NETLINK) return -lnx::EAFNOSUPPORT_;
        if (type != SK_STREAM && type != SK_DGRAM && type != SK_SEQPACKET && !(type == 3 && a0 == AFAM_NETLINK))
            return -lnx::EPROTONOSUPPORT_;
        if (a0 == AFAM_NETLINK) type = SK_DGRAM;
        auto s = make_sock(a0, type, a1 & lnx::L_O_NONBLOCK);
        return p.fds->install(s, a1 & lnx::L_O_CLOEXEC);
    }
    case 288: {  // socketpair(domain, type, proto, sv*)
        if (a0 != AFAM_UNIX) return -lnx::EOPNOTSUPP_;
        int type = a1 & 0xf;
        if (type != SK_STREAM && type != SK_DGRAM && type != SK_SEQPACKET) return -lnx::EPROTONOSUPPORT_;
        auto x = make_sock(AFAM_UNIX, type, a1 & lnx::L_O_NONBLOCK);
        auto y = make_sock(AFAM_UNIX, type, a1 & lnx::L_O_NONBLOCK);
        x->tx = y->rx;
        y->tx = x->rx;
        x->st = y->st = Sock::Connected;
        x->peer = y->local;
        y->peer = x->local;
        int fx = p.fds->install(x, a1 & lnx::L_O_CLOEXEC);
        if (fx < 0) return fx;
        int fy = p.fds->install(y, a1 & lnx::L_O_CLOEXEC);
        if (fy < 0) {
            p.fds->close(fx);
            return fy;
        }
        m.wr<s32>(a3, fx);
        m.wr<s32>(a3 + 4, fy);
        return 0;
    }
    case 282: {  // bind
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (s->domain == AFAM_NETLINK) return 0;
        GAddr ga;
        if (s->domain == AFAM_UNIX && a2 == 2) {  // autobind: random abstract name
            char nm[8];
            snprintf(nm, sizeof nm, "%05x", s->id & 0xfffff);
            ga.key = std::string("@") + nm;
            ga.raw = {1, 0, 0};
            ga.raw.insert(ga.raw.end(), nm, nm + 5);
        } else if (int e = parse_addr(t, s->domain, a1, a2, ga, true)) {
            return e;
        }
        Lock lk(g_ipc);
        if (!s->key.empty()) return -lnx::EINVAL_;
        if (s->domain != AFAM_UNIX) {
            u16 port;
            memcpy(&port, ga.raw.data() + 2, 2);
            if (port == 0) {
                inet_autobind(*s, s);
                return 0;
            }
            if (lookup(ga.key)) return -lnx::EADDRINUSE_;
        } else if (!ga.unix_path && lookup(ga.key)) {
            return -lnx::EADDRINUSE_;
        }
        // Unix path sockets: like the original host-backed version, a new bind replaces a stale one.
        s->key = ga.key;
        s->local = ga.raw;
        g_socks[ga.key] = s;
        return 0;
    }
    case 283: {  // connect
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (s->domain == AFAM_NETLINK) return 0;
        GAddr ga;
        if (int e = parse_addr(t, s->domain, a1, a2, ga, false)) return e;
        bool missing = ga.key.find('\x01') != std::string::npos;
        if (missing) ga.key.resize(ga.key.find('\x01'));
        if ((s->domain == AFAM_INET || s->domain == AFAM_INET6) && !ga.loopback) {
            Lock lk(g_ipc);
            if (!find_target(s->domain, ga)) return -lnx::ENETUNREACH_;  // no networking beyond this machine
        }
        if (!s->stream_like()) {
            Lock lk(g_ipc);
            auto tgt = find_target(s->domain, ga);
            if (!tgt && s->domain == AFAM_UNIX) return missing ? -lnx::ENOENT_ : -lnx::ECONNREFUSED_;
            s->peer_key = tgt ? tgt->key : ga.key;
            s->peer = ga.raw;
            s->st = Sock::Connected;
            if (s->domain != AFAM_UNIX) inet_autobind(*s, s);
            return 0;
        }
        while (true) {
            u64 gen = io_gen();
            {
                Lock lk(g_ipc);
                if (s->st == Sock::Connected) return -lnx::EISCONN_;
                if (s->st == Sock::Listening) return -lnx::EINVAL_;
                auto lst = find_target(s->domain, ga);
                if (!lst || lst->st != Sock::Listening || lst->type != s->type)
                    return (s->domain == AFAM_UNIX && missing) ? -lnx::ENOENT_ : -lnx::ECONNREFUSED_;
                if ((int)lst->backlog.size() <= std::max(lst->backlog_max, 1)) {
                    if (s->domain != AFAM_UNIX) inet_autobind(*s, s);
                    auto srv = make_sock(s->domain, s->type, 0);
                    srv->st = Sock::Connected;
                    srv->local = lst->local;
                    srv->peer = s->local;
                    s->tx = srv->rx;
                    srv->tx = s->rx;
                    s->peer = lst->local;
                    s->st = Sock::Connected;
                    lst->backlog.push_back(srv);
                    io_notify();
                    return 0;
                }
            }
            if (s->flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
            if (t.signal_pending()) return -lnx::EINTR_;
            io_wait(gen);
        }
    }
    case 284: {  // listen
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (!s->stream_like()) return -lnx::EOPNOTSUPP_;
        Lock lk(g_ipc);
        if (s->st == Sock::Connected) return -lnx::EINVAL_;
        if (s->key.empty()) {
            if (s->domain == AFAM_UNIX) return -lnx::EINVAL_;
            inet_autobind(*s, s);
        }
        s->st = Sock::Listening;
        s->backlog_max = std::clamp((int)(s32)a1, 1, 4096);
        io_notify();
        return 0;
    }
    case 285: case 366: {  // accept / accept4
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        u32 fl = nr == 366 ? a3 : 0;
        while (true) {
            u64 gen = io_gen();
            std::shared_ptr<Sock> c;
            {
                Lock lk(g_ipc);
                if (s->st != Sock::Listening) return -lnx::EINVAL_;
                if (!s->backlog.empty()) {
                    c = s->backlog.front();
                    s->backlog.pop_front();
                }
            }
            if (c) {
                c->flags = 2 | (fl & lnx::L_O_NONBLOCK);
                if (a1) put_addr(t, c->peer, a1, a2);
                io_notify();
                return p.fds->install(c, fl & lnx::L_O_CLOEXEC);
            }
            if (s->flags & lnx::L_O_NONBLOCK) return -lnx::EAGAIN_;
            if (t.signal_pending()) return -ERESTARTSYS;
            io_wait(gen);
        }
    }
    case 286: case 287: {  // getsockname / getpeername
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        Lock lk(g_ipc);
        if (nr == 287) {
            if (s->st != Sock::Connected) return -lnx::ENOTCONN_;
            put_addr(t, s->peer, a1, a2);
        } else {
            put_addr(t, s->local, a1, a2);
        }
        return 0;
    }
    case 289: case 290: {  // send / sendto(fd, buf, len, flags, addr, alen)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        GAddr ga;
        bool has_dest = nr == 290 && a4 && !s->stream_like();
        if (has_dest)
            if (int e = parse_addr(t, s->domain, a4, a5, ga, false)) return e;
        return (s32)sock_send(t, s, m.ptr(a1), a2, has_dest ? &ga : nullptr, a3);
    }
    case 291: case 292: {  // recv / recvfrom(fd, buf, len, flags, addr, alen*)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        std::vector<u8> from;
        s64 r = sock_recv(t, *s, m.ptr(a1), a2, a3, &from);
        if (r >= 0 && nr == 292 && a4) {
            if (!from.empty()) put_addr(t, from, a4, a5);
            else if (s->stream_like()) put_addr(t, s->peer, a4, a5);
            else if (a5) m.wr<u32>(a5, 0);
        }
        return (s32)r;
    }
    case 293: {  // shutdown
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        {
            Lock lk(g_ipc);
            if (s->stream_like() && s->st != Sock::Connected) return -lnx::ENOTCONN_;
            if (a1 == 0 || a1 == 2) s->shut_rd = true;
            if (a1 == 1 || a1 == 2) {
                s->shut_wr = true;
                if (s->tx) s->tx->eof = true;
            }
        }
        io_notify();
        return 0;
    }
    case 294: {  // setsockopt: accepted and ignored
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        return 0;
    }
    case 295: {  // getsockopt(fd, level, name, val, len*)
        auto s = get_sock(t, a0, &err);
        if (!s) return err;
        if (a1 == 1 && a2 == 4) {  // SO_ERROR
            m.wr<s32>(a3, 0);
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
            m.wr<s32>(a3, s->type);
            m.wr<u32>(a4, 4);
            return 0;
        }
        if (a1 == 1 && a2 == 30) {  // SO_ACCEPTCONN
            m.wr<s32>(a3, s->st == Sock::Listening ? 1 : 0);
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
        size_t total = 0;
        for (u32 i = 0; i < iovlen; i++) total += m.rd<u32>(iov + i * 8 + 4);
        std::vector<u8> tmp(total);
        if (nr == 296) {
            size_t off = 0;
            for (u32 i = 0; i < iovlen; i++) {
                u32 b = m.rd<u32>(iov + i * 8), l = m.rd<u32>(iov + i * 8 + 4);
                memcpy(tmp.data() + off, m.ptr(b), l);
                off += l;
            }
            GAddr ga;
            bool has_dest = name && !s->stream_like();
            if (has_dest)
                if (int e = parse_addr(t, s->domain, name, namelen, ga, false)) return e;
            return (s32)sock_send(t, s, tmp.data(), total, has_dest ? &ga : nullptr, a2);
        }
        std::vector<u8> from;
        s64 r = sock_recv(t, *s, tmp.data(), total, a2, &from);
        if (r < 0) return (s32)r;
        size_t off = 0;
        for (u32 i = 0; i < iovlen && off < (size_t)r; i++) {
            u32 b = m.rd<u32>(iov + i * 8), l = m.rd<u32>(iov + i * 8 + 4);
            u32 c = (u32)std::min<size_t>(l, (size_t)r - off);
            memcpy(m.ptr(b), tmp.data() + off, c);
            off += c;
        }
        if (name) {
            const std::vector<u8>& src = !from.empty() ? from : s->peer;
            m.write_bytes(name, src.data(), std::min<size_t>(src.size(), namelen));
            m.wr<u32>(a1 + 4, (u32)src.size());
        }
        m.wr<u32>(a1 + 20, 0);  // controllen
        m.wr<u32>(a1 + 24, 0);  // msg_flags
        return (s32)r;
    }
    }
    return -lnx::ENOSYS_;
}
