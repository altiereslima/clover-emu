// /dev and /proc virtual files.
#include "kernel.h"
#include "display.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <algorithm>

namespace {
struct NullDev : File {
    int kind;  // 0 null, 1 zero, 2 full, 3 random
    explicit NullDev(int k) : kind(k) {}
    s64 read(Thread&, u8* buf, u32 n) override {
        if (kind == 0) return 0;
        if (kind == 3) arc4random_buf(buf, n);
        else memset(buf, 0, n);
        return n;
    }
    s64 write(Thread&, const u8*, u32 n) override { return kind == 2 ? -lnx::ENOSPC_ : (s64)n; }
    s64 pread(Thread& t, u8* buf, u32 n, s64) override { return read(t, buf, n); }
    s64 lseek(s64, int) override { return 0; }
    int fstat(LinuxStat64& st) override {
        static const u64 rdev[] = {0x103, 0x105, 0x107, 0x109};
        st = make_stat(lnx::L_S_IFCHR | 0666, 0, rdev[kind]);
        return 0;
    }
    int mmap(Thread& t, u32 addr, u32 len, u32 prot, u32 flags, u64 off, bool* handled) override {
        if (kind == 1) {
            t.proc->mem->map(addr, len, prot, "/dev/zero");
            *handled = true;
        }
        return 0;
    }
};

// Console: output goes to host stderr.
struct ConsoleDev : File {
    std::string name;
    explicit ConsoleDev(std::string n) : name(std::move(n)) {}
    s64 read(Thread&, u8*, u32) override { return 0; }
    s64 write(Thread& t, const u8* buf, u32 n) override {
        std::string s((const char*)buf, n);
        log_raw("%s", s.c_str());
        return n;
    }
    int ioctl(Thread& t, u32 req, u32 arg) override {
        AddressSpace& m = *t.proc->mem;
        switch (req) {
        case 0x5401: {  // TCGETS
            u8 tio[36] = {};
            u32 iflag = 0x500, oflag = 0x5, cflag = 0xbf, lflag = 0x8a3b;
            memcpy(tio, &iflag, 4); memcpy(tio + 4, &oflag, 4); memcpy(tio + 8, &cflag, 4); memcpy(tio + 12, &lflag, 4);
            m.write_bytes(arg, tio, sizeof tio);
            return 0;
        }
        case 0x5402: case 0x5403: case 0x5404: case 0x540E: case 0x5422: case 0x5410: return 0;
        case 0x540F: m.wr<s32>(arg, t.proc->pgid); return 0;  // TIOCGPGRP
        case 0x5413: { u16 ws[4] = {25, 80, 0, 0}; m.write_bytes(arg, ws, 8); return 0; }
        case 0x5414: return 0;
        case 0x4B3A: case 0x4B3B: case 0x5603: case 0x5606: return 0;  // KDSETMODE etc, VT
        }
        return -lnx::ENOTTY_;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFCHR | 0620, 0, 0x501);
        return 0;
    }
    u32 poll_mask() override { return 4; }
};

// /dev/fb0
struct FbDev : File {
    int ioctl(Thread& t, u32 req, u32 arg) override {
        AddressSpace& m = *t.proc->mem;
        auto& fb = display::fb();
        switch (req) {
        case 0x4600: {  // FBIOGET_VSCREENINFO
            u32 v[40] = {};
            v[0] = fb.xres; v[1] = fb.yres; v[2] = fb.xres; v[3] = fb.yres_virtual;
            v[4] = 0; v[5] = fb.yoffset; v[6] = 32; v[7] = 0;
            v[8] = 16; v[9] = 8; v[10] = 0;   // red
            v[11] = 8; v[12] = 8; v[13] = 0;  // green
            v[14] = 0; v[15] = 8; v[16] = 0;  // blue
            v[17] = 24; v[18] = 8; v[19] = 0; // transp
            v[22] = 0xffffffff; v[23] = 0xffffffff;  // height/width mm unknown
            v[25] = 13468;  // pixclock (ps) ~74.25MHz
            v[26] = 220; v[27] = 110; v[28] = 20; v[29] = 5; v[30] = 40; v[31] = 5;
            m.write_bytes(arg, v, 160);
            return 0;
        }
        case 0x4601: {  // FBIOPUT_VSCREENINFO
            u32 yoff = m.rd<u32>(arg + 20);
            if (yoff + fb.yres <= fb.yres_virtual) fb.yoffset = yoff;
            display::note_fb_flip();
            return 0;
        }
        case 0x4602: {  // FBIOGET_FSCREENINFO
            u8 f[68] = {};
            strcpy((char*)f, "sunxi_fb0");
            u32 smem_start = 0x5c000000, smem_len = fb.size, type = 0, visual = 2;
            memcpy(f + 16, &smem_start, 4);
            memcpy(f + 20, &smem_len, 4);
            memcpy(f + 24, &type, 4);
            memcpy(f + 32, &visual, 4);
            u16 ypan = 1;
            memcpy(f + 38, &ypan, 2);
            u32 ll = fb.xres * 4;
            memcpy(f + 44, &ll, 4);
            m.write_bytes(arg, f, sizeof f);
            return 0;
        }
        case 0x4606: {  // FBIOPAN_DISPLAY
            u32 yoff = m.rd<u32>(arg + 20);
            if (yoff + fb.yres <= fb.yres_virtual) fb.yoffset = yoff;
            display::note_fb_flip();
            return 0;
        }
        case 0x4611:  // FBIOBLANK
            fb.blank = arg != 0;
            display::note_fb_activity();
            return 0;
        case 0x40044620:  // FBIO_WAITFORVSYNC
            display::wait_vsync_tick();
            return 0;
        case 0x4604: case 0x4605: return 0;  // cmap
        }
        LOGC(LOG_DEV, "[fb0] unhandled ioctl %08x\n", req);
        return 0;
    }
    s64 pread(Thread&, u8* buf, u32 n, s64 off) override {
        auto& fb = display::fb();
        if (off >= fb.size) return 0;
        u32 c = std::min<u32>(n, fb.size - (u32)off);
        memcpy(buf, fb.mem + off, c);
        return c;
    }
    s64 pos = 0;
    s64 read(Thread& t, u8* buf, u32 n) override {
        s64 r = pread(t, buf, n, pos);
        if (r > 0) pos += r;
        return r;
    }
    s64 write(Thread&, const u8* buf, u32 n) override {
        auto& fb = display::fb();
        if (pos >= fb.size) return -lnx::ENOSPC_;
        u32 c = std::min<u32>(n, fb.size - (u32)pos);
        memcpy(fb.mem + pos, buf, c);
        pos += c;
        display::note_fb_activity();
        return c;
    }
    s64 lseek(s64 off, int whence) override {
        pos = whence == 0 ? off : whence == 1 ? pos + off : display::fb().size + off;
        return pos;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFCHR | 0660, 0, 0x1d00);
        return 0;
    }
    int mmap(Thread& t, u32 addr, u32 len, u32 prot, u32 flags, u64 off, bool* handled) override {
        auto& fb = display::fb();
        if (!t.proc->mem->map_shared(addr, std::min<u32>(len, fb.size), fb.shm_fd, off, "/dev/fb0")) {
            LOGI("[fb0] map_shared failed at %08x\n", addr);
            return -lnx::ENOMEM_;
        }
        *handled = true;
        display::note_fb_activity();
        return 0;
    }
};

// Allwinner /dev/disp (sunxi display engine). Commands take u32 args[4] pointer.
struct DispDev : File {
    int ioctl(Thread& t, u32 req, u32 arg) override {
        AddressSpace& m = *t.proc->mem;
        u32 a[4] = {};
        if (arg) m.read_bytes(arg, a, 16);
        LOGC(LOG_DEV, "[disp] cmd %#x args %x %x %x %x\n", req, a[0], a[1], a[2], a[3]);
        display::note_fb_activity();
        switch (req) {
        case 0x07: return (s32)display::fb().xres;  // DISP_GET_SCN_WIDTH
        case 0x08: return (s32)display::fb().yres;  // DISP_GET_SCN_HEIGHT
        case 0x09: return 4;                         // DISP_GET_OUTPUT_TYPE -> HDMI
        case 0x0a: return 0;
        case 0xc1: return 5;                         // DISP_HDMI_GET_MODE (720p60)
        case 0xc4: return 1;                         // DISP_HDMI_SUPPORT_MODE
        }
        return 0;
    }
    int fstat(LinuxStat64& st) override {
        st = make_stat(lnx::L_S_IFCHR | 0660, 0, 0xfe00);
        return 0;
    }
};

struct DevEntry {
    const char* path;
    u32 mode;
    u64 rdev;
};
const DevEntry g_devs[] = {
    {"/dev/null", lnx::L_S_IFCHR | 0666, 0x103},    {"/dev/zero", lnx::L_S_IFCHR | 0666, 0x105},
    {"/dev/full", lnx::L_S_IFCHR | 0666, 0x107},    {"/dev/random", lnx::L_S_IFCHR | 0666, 0x108},
    {"/dev/urandom", lnx::L_S_IFCHR | 0666, 0x109}, {"/dev/console", lnx::L_S_IFCHR | 0600, 0x501},
    {"/dev/tty", lnx::L_S_IFCHR | 0666, 0x500},     {"/dev/tty0", lnx::L_S_IFCHR | 0620, 0x400},
    {"/dev/tty1", lnx::L_S_IFCHR | 0620, 0x401},    {"/dev/ttyS0", lnx::L_S_IFCHR | 0660, 0x440},
    {"/dev/ttyS2", lnx::L_S_IFCHR | 0660, 0x442},   {"/dev/kmsg", lnx::L_S_IFCHR | 0644, 0x10b},
    {"/dev/fb0", lnx::L_S_IFCHR | 0660, 0x1d00},    {"/dev/disp", lnx::L_S_IFCHR | 0660, 0xfe00},
    {"/dev/ptmx", lnx::L_S_IFCHR | 0666, 0x502},
};
const char* g_dev_dirs[] = {"/dev", "/dev/pts", "/dev/input", "/dev/snd", "/dev/shm"};
}  // namespace

bool device_exists(const std::string& g, LinuxStat64* st) {
    for (auto& d : g_devs)
        if (g == d.path) {
            if (st) *st = make_stat(d.mode, 0, d.rdev, std::hash<std::string>{}(g));
            return true;
        }
    for (auto* d : g_dev_dirs)
        if (g == d) {
            if (st) *st = make_stat(lnx::L_S_IFDIR | 0755, 0, 0, std::hash<std::string>{}(g));
            return true;
        }
    for (const char* l : {"/dev/stdin", "/dev/stdout", "/dev/stderr", "/dev/fd"})
        if (g == l) {
            if (st) *st = make_stat(lnx::L_S_IFLNK | 0777, 15);
            return true;
        }
    return false;
}

std::vector<std::pair<std::string, u8>> device_list(const std::string& dir) {
    std::vector<std::pair<std::string, u8>> out = {{".", 4}, {"..", 4}};
    std::string pre = dir + "/";
    for (auto& d : g_devs) {
        std::string p = d.path;
        if (p.compare(0, pre.size(), pre) == 0 && p.find('/', pre.size()) == std::string::npos)
            out.push_back({p.substr(pre.size()), 2});
    }
    for (auto* d : g_dev_dirs) {
        std::string p = d;
        if (p.compare(0, pre.size(), pre) == 0 && p.find('/', pre.size()) == std::string::npos)
            out.push_back({p.substr(pre.size()), 4});
    }
    if (dir == "/dev")
        for (const char* l : {"stdin", "stdout", "stderr", "fd"}) out.push_back({l, 10});
    return out;
}

FilePtr open_device(Thread& t, const std::string& g, u32 flags, int* err) {
    FilePtr f;
    if (g == "/dev/null") f = std::make_shared<NullDev>(0);
    else if (g == "/dev/zero") f = std::make_shared<NullDev>(1);
    else if (g == "/dev/full") f = std::make_shared<NullDev>(2);
    else if (g == "/dev/random" || g == "/dev/urandom") f = std::make_shared<NullDev>(3);
    else if (g == "/dev/console" || g == "/dev/tty" || g == "/dev/tty0" || g == "/dev/tty1" || g == "/dev/ttyS0" ||
             g == "/dev/ttyS2" || g == "/dev/kmsg")
        f = std::make_shared<ConsoleDev>(g);
    else if (g == "/dev/fb0") f = std::make_shared<FbDev>();
    else if (g == "/dev/disp") f = std::make_shared<DispDev>();
    else {
        for (auto* d : g_dev_dirs)
            if (g == d) {
                auto vd = std::make_shared<VirtDir>();
                vd->entries = device_list(g);
                f = vd;
            }
    }
    if (!f) {
        LOGC(LOG_DEV, "[dev] open of missing device %s\n", g.c_str());
        *err = lnx::ENOENT_;
        return nullptr;
    }
    f->path = g;
    f->flags = flags & (lnx::L_O_ACCMODE | lnx::L_O_NONBLOCK);
    return f;
}

// ---------------------------------------------------------------- procfs
namespace {
std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> v;
    size_t i = 1;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        if (j > i) v.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return v;
}
bool is_num(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); }

std::string cpuinfo() {
    std::string s;
    for (int i = 0; i < 4; i++) {
        s += "processor\t: " + std::to_string(i) +
             "\nmodel name\t: ARMv7 Processor rev 5 (v7l)\nBogoMIPS\t: 48.00\n"
             "Features\t: swp half thumb fastmult vfp edsp thumbee neon vfpv3 tls vfpv4 idiva idivt \n"
             "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc07\nCPU revision\t: 5\n\n";
    }
    s += "Hardware\t: sun8i\nRevision\t: 0000\nSerial\t\t: 1234567890abcdef0123\n";
    return s;
}

std::string proc_file(Thread& t, Process* tp, const std::string& name, bool* ok) {
    *ok = true;
    char b[512];
    if (!tp) {
        if (name == "cpuinfo") return cpuinfo();
        if (name == "meminfo")
            return "MemTotal:         250000 kB\nMemFree:          160000 kB\nMemAvailable:     190000 kB\nBuffers:            1000 kB\n"
                   "Cached:            20000 kB\nSwapCached:            0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n";
        if (name == "uptime") {
            double up = (now_ns() - g_kernel.boot_time_ns) / 1e9 + 10;
            snprintf(b, sizeof b, "%.2f %.2f\n", up, up * 3);
            return b;
        }
        if (name == "loadavg") return "0.10 0.10 0.05 1/50 100\n";
        if (name == "version") return "Linux version 3.4.113 (clover@build) (gcc version 4.9) #1 SMP PREEMPT\n";
        if (name == "cmdline") return "console=ttyS0,115200 root=/dev/ram0 hsqs=1\n";
        if (name == "filesystems") return "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n\tsquashfs\n\text4\nnodev\tdevpts\n";
        if (name == "mounts")
            return "rootfs / rootfs rw 0 0\n/dev/root / squashfs ro,relatime 0 0\nproc /proc proc rw 0 0\n"
                   "tmpfs /tmp tmpfs rw 0 0\ntmpfs /var tmpfs rw 0 0\n/dev/nandd /var/lib ext4 rw,nosuid,nodev,noatime 0 0\n"
                   "sysfs /sys sysfs rw 0 0\ndevpts /dev/pts devpts rw 0 0\ntmpfs /dev/shm tmpfs rw 0 0\ntmpfs /run tmpfs rw 0 0\n";
        if (name == "stat") return "cpu  100 0 100 10000 0 0 0 0 0 0\ncpu0 25 0 25 2500 0 0 0 0 0 0\nbtime 1500000000\nprocesses 100\n";
        if (name == "modules") return "mali 200000 0 - Live 0x00000000\nclovercon 10000 0 - Live 0x00000000\n";
        if (name == "devices") return "Character devices:\n  1 mem\n  4 tty\n  5 /dev/tty\n 29 fb\n 13 input\n";
        if (name == "partitions") return "major minor  #blocks  name\n\n  93        0     524288 nand\n";
        if (name == "swaps") return "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n";
        if (name == "interrupts") return "           CPU0\n";
        if (name == "sys/kernel/hotplug") return "\n";
        if (name == "sys/kernel/osrelease") return "3.4.113\n";
        if (name == "sys/kernel/pid_max") return "32768\n";
        if (name == "sys/kernel/random/boot_id" || name == "sys/kernel/random/uuid") return "d1f0c2a0-1111-4aaa-9bbb-123456789abc\n";
        if (name == "sys/vm/overcommit_memory") return "0\n";
        if (name == "sys/kernel/printk") return "4\t4\t1\t7\n";
        *ok = false;
        return "";
    }
    Process& p = *tp;
    if (name == "cmdline") {
        std::string s;
        for (auto& a : p.argv) s += a + std::string(1, '\0');
        return s;
    }
    if (name == "environ") {
        std::string s;
        for (auto& a : p.envp) s += a + std::string(1, '\0');
        return s;
    }
    if (name == "comm") return p.comm + "\n";
    if (name == "stat") {
        snprintf(b, sizeof b, "%d (%s) %c %d %d %d 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 %zu 0 %llu 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                 p.pid, p.comm.c_str(), p.zombie ? 'Z' : 'S', p.ppid, p.pgid, p.sid, p.threads.size(),
                 (unsigned long long)((p.start_time_ns - g_kernel.boot_time_ns) / 10000000ull));
        return b;
    }
    if (name == "status") {
        snprintf(b, sizeof b,
                 "Name:\t%s\nState:\t%s\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t%zu\n"
                 "VmRSS:\t   10000 kB\n",
                 p.comm.c_str(), p.zombie ? "Z (zombie)" : "S (sleeping)", p.pid, p.pid, p.ppid, p.threads.size());
        return b;
    }
    if (name == "statm") return "5000 2500 500 100 0 2000 0\n";
    if (name == "maps") return p.mem ? p.mem->maps_text() : "";
    if (name == "oom_score_adj" || name == "oom_adj") return "0\n";
    if (name == "mounts") return proc_file(t, nullptr, "mounts", ok);
    *ok = false;
    return "";
}
}  // namespace

int readlink_proc_p(Process& self, const std::string& g, std::string& out) {
    auto v = split_path(g);  // proc, pid, ...
    if (v.size() < 3 || v[0] != "proc") return -lnx::EINVAL_;
    std::shared_ptr<Process> tp;
    if (v[1] == "self") tp = self.shared_from_this();
    else if (is_num(v[1])) {
        std::lock_guard lk(g_kernel.mtx);
        tp = g_kernel.find_proc(std::stoi(v[1]));
        if (!tp) {
            Thread* th = g_kernel.find_thread(std::stoi(v[1]));
            if (th) tp = th->proc->shared_from_this();
        }
    }
    if (!tp) return -lnx::ENOENT_;
    if (v.size() == 3 && v[2] == "exe") { out = tp->exe; return 0; }
    if (v.size() == 3 && v[2] == "cwd") { out = tp->cwd; return 0; }
    if (v.size() == 3 && v[2] == "root") { out = "/"; return 0; }
    if (v.size() == 4 && v[2] == "fd" && is_num(v[3])) {
        FilePtr f = tp->fds ? tp->fds->get(std::stoi(v[3])) : nullptr;
        if (!f) return -lnx::ENOENT_;
        out = f->link_name();
        return 0;
    }
    return -lnx::EINVAL_;
}

int readlink_proc(Thread& t, const std::string& g, std::string& out) { return readlink_proc_p(*t.proc, g, out); }

static std::shared_ptr<Process> proc_target(Thread& t, const std::string& s) {
    if (s == "self") return t.proc->shared_from_this();
    if (!is_num(s)) return nullptr;
    std::lock_guard lk(g_kernel.mtx);
    auto p = g_kernel.find_proc(std::stoi(s));
    if (!p) {
        Thread* th = g_kernel.find_thread(std::stoi(s));
        if (th) p = th->proc->shared_from_this();
    }
    return p;
}

static bool proc_is_dir(Thread& t, const std::vector<std::string>& v) {
    if (v.size() == 1) return true;  // /proc
    if (v.size() >= 2 && (is_num(v[1]) || v[1] == "self")) {
        if (v.size() == 2) return true;
        if (v.size() == 3 && (v[2] == "fd" || v[2] == "task" || v[2] == "fdinfo")) return true;
        if (v.size() == 4 && v[2] == "task") return true;
        return false;
    }
    if (v[1] == "sys" || v[1] == "bus" || v[1] == "driver" || v[1] == "net") {
        if (v.size() <= 3) return v.size() < 3 || (v[2] == "kernel" || v[2] == "vm" || v[2] == "net" || v[2] == "fs");
        if (v.size() == 4 && v[3] == "random") return true;
    }
    return false;
}

int stat_proc(Thread& t, const std::string& g, LinuxStat64& st) {
    auto v = split_path(g);
    if (proc_is_dir(t, v)) {
        if (v.size() >= 2 && (is_num(v[1]) || v[1] == "self") && !proc_target(t, v[1])) return -lnx::ENOENT_;
        st = make_stat(lnx::L_S_IFDIR | 0555, 0, 0, std::hash<std::string>{}(g));
        return 0;
    }
    std::string link;
    if (readlink_proc(t, g, link) == 0) {
        st = make_stat(lnx::L_S_IFLNK | 0777, link.size());
        return 0;
    }
    bool ok;
    std::shared_ptr<Process> tp;
    std::string rest;
    if (v.size() >= 3 && (is_num(v[1]) || v[1] == "self")) {
        tp = proc_target(t, v[1]);
        if (!tp) return -lnx::ENOENT_;
        for (size_t i = 2; i < v.size(); i++) rest += (i > 2 ? "/" : "") + v[i];
    } else
        for (size_t i = 1; i < v.size(); i++) rest += (i > 1 ? "/" : "") + v[i];
    proc_file(t, tp.get(), rest, &ok);
    if (!ok) return -lnx::ENOENT_;
    st = make_stat(lnx::L_S_IFREG | 0444, 0, 0, std::hash<std::string>{}(g));
    return 0;
}

FilePtr open_proc(Thread& t, const std::string& g, u32 flags, int* err) {
    auto v = split_path(g);
    if (proc_is_dir(t, v)) {
        auto d = std::make_shared<VirtDir>();
        d->entries = {{".", 4}, {"..", 4}};
        if (v.size() == 1) {
            std::lock_guard lk(g_kernel.mtx);
            for (auto& [pid, p] : g_kernel.procs)
                if (!p->zombie) d->entries.push_back({std::to_string(pid), 4});
            for (const char* n : {"cpuinfo", "meminfo", "uptime", "loadavg", "version", "cmdline", "filesystems", "mounts",
                                  "stat", "modules", "devices", "partitions"})
                d->entries.push_back({n, 8});
            d->entries.push_back({"self", 10});
            d->entries.push_back({"sys", 4});
        } else if (v.size() == 2) {
            if (!proc_target(t, v[1])) { *err = lnx::ENOENT_; return nullptr; }
            for (const char* n : {"cmdline", "environ", "comm", "stat", "status", "statm", "maps", "mounts", "oom_score_adj"})
                d->entries.push_back({n, 8});
            for (const char* n : {"exe", "cwd", "root"}) d->entries.push_back({n, 10});
            d->entries.push_back({"fd", 4});
            d->entries.push_back({"task", 4});
        } else if (v.size() == 3 && v[2] == "fd") {
            auto tp = proc_target(t, v[1]);
            if (!tp) { *err = lnx::ENOENT_; return nullptr; }
            std::lock_guard lk(tp->fds->mtx);
            for (size_t i = 0; i < tp->fds->fds.size(); i++)
                if (tp->fds->fds[i].file) d->entries.push_back({std::to_string(i), 10});
        } else if (v.size() == 3 && v[2] == "task") {
            auto tp = proc_target(t, v[1]);
            if (!tp) { *err = lnx::ENOENT_; return nullptr; }
            std::lock_guard lk(tp->pmtx);
            for (Thread* th : tp->threads)
                if (!th->is_host_call) d->entries.push_back({std::to_string(th->tid), 4});
        }
        return d;
    }
    bool ok;
    std::shared_ptr<Process> tp;
    std::string rest;
    if (v.size() >= 3 && (is_num(v[1]) || v[1] == "self")) {
        tp = proc_target(t, v[1]);
        if (!tp) { *err = lnx::ENOENT_; return nullptr; }
        for (size_t i = 2; i < v.size(); i++) rest += (i > 2 ? "/" : "") + v[i];
    } else
        for (size_t i = 1; i < v.size(); i++) rest += (i > 1 ? "/" : "") + v[i];
    auto f = std::make_shared<MemFile>();
    f->data = proc_file(t, tp.get(), rest, &ok);
    if (!ok) {
        *err = lnx::ENOENT_;
        return nullptr;
    }
    f->mode = lnx::L_S_IFREG | 0644;
    f->on_write = [](const std::string&) {};
    return f;
}
