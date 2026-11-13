// ELF loading and execve.
#include "kernel.h"
#include <cstring>
#include <algorithm>
#include <unistd.h>

namespace {
struct Elf32_Ehdr {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32_Phdr {
    u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_PHDR = 6, ET_EXEC = 2, ET_DYN = 3, EM_ARM = 40 };

struct LoadedImage {
    u32 base = 0;       // load bias
    u32 entry = 0;
    u32 phdr = 0;
    u32 phnum = 0;
    u32 end = 0;        // highest mapped address
    std::string interp;
};

bool check_elf(const std::vector<u8>& d) {
    if (d.size() < sizeof(Elf32_Ehdr)) return false;
    if (memcmp(d.data(), "\x7f" "ELF", 4) != 0) return false;
    auto* eh = (const Elf32_Ehdr*)d.data();
    if (eh->e_ident[4] != 1 || eh->e_machine != EM_ARM) return false;
    return eh->e_type == ET_EXEC || eh->e_type == ET_DYN;
}

bool load_elf(AddressSpace& as, const std::vector<u8>& d, LoadedImage& img, const std::string& name) {
    auto* eh = (const Elf32_Ehdr*)d.data();
    if ((u64)eh->e_phoff + (u64)eh->e_phnum * sizeof(Elf32_Phdr) > d.size()) return false;
    auto* ph = (const Elf32_Phdr*)(d.data() + eh->e_phoff);
    u32 lo = 0xffffffff, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            lo = std::min(lo, ph[i].p_vaddr & ~(GUEST_PAGE - 1));
            hi = std::max(hi, ph[i].p_vaddr + ph[i].p_memsz);
        }
        if (ph[i].p_type == PT_INTERP) {
            img.interp.assign((const char*)d.data() + ph[i].p_offset, strnlen((const char*)d.data() + ph[i].p_offset, ph[i].p_filesz));
        }
    }
    if (lo > hi) return false;
    u32 bias = 0;
    if (eh->e_type == ET_DYN) {
        u32 span = ((hi - lo) + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        u32 at = as.find_free(span, 0x10000);
        if (!at) return false;
        bias = at - lo;
    }
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        u32 va = ph[i].p_vaddr + bias;
        u32 start = va & ~(GUEST_PAGE - 1);
        u32 end = (va + ph[i].p_memsz + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        u32 prot = 0;
        if (ph[i].p_flags & 4) prot |= 1;
        if (ph[i].p_flags & 2) prot |= 2;
        if (ph[i].p_flags & 1) prot |= 4;
        // Segments may share a page with the previous one; map() of overlapping page is fine since we copy afterwards.
        if (as.is_free(start, end - start)) {
            as.map(start, end - start, prot, name);
        } else if (!as.is_mapped(start, end - start)) {
            // map only unmapped parts
            for (u32 a = start; a < end; a += GUEST_PAGE)
                if (!as.is_mapped(a, GUEST_PAGE)) as.map(a, GUEST_PAGE, prot, name);
        }
        as.protect(start, end - start, prot);
        if (ph[i].p_filesz) {
            if ((u64)ph[i].p_offset + ph[i].p_filesz > d.size()) return false;
            as.write_bytes(va, d.data() + ph[i].p_offset, ph[i].p_filesz);
        }
        if (ph[i].p_memsz > ph[i].p_filesz) memset(as.ptr(va + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
        img.end = std::max(img.end, end);
    }
    img.base = bias;
    img.entry = eh->e_entry + bias;
    img.phnum = eh->e_phnum;
    img.phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_PHDR) img.phdr = ph[i].p_vaddr + bias;
    if (!img.phdr) {
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff && eh->e_phoff < ph[i].p_offset + ph[i].p_filesz)
                img.phdr = ph[i].p_vaddr + bias + (eh->e_phoff - ph[i].p_offset);
    }
    return true;
}
}  // namespace

void write_kuser_page(AddressSpace& as) {
    as.map(KUSER_PAGE, 0x10000, 5, "[vectors]");
    static const u32 cmpxchg64[] = {0xe92d00f0, 0xe1c040d0, 0xe1c160d0, 0xe1b20f9f, 0xe0303004, 0x00313005,
                                    0x01a23f96, 0x03330001, 0x0afffff9, 0xe2730000, 0xe8bd00f0, 0xe12fff1e};
    static const u32 membar[] = {0xf57ff05b, 0xe12fff1e};
    static const u32 cmpxchg[] = {0xe1923f9f, 0xe0533000, 0x01823f91, 0x03330001, 0x0afffffa, 0xe2730000, 0xe12fff1e};
    static const u32 gettls[] = {0xee1d0f70, 0xe12fff1e};
    static const u32 sigret[] = {0xe3a070ad, 0xef000000};
    static const u32 hostret[] = {0xefffffff, 0xefffffff};
    as.write_bytes(0xffff0f60, cmpxchg64, sizeof cmpxchg64);
    as.write_bytes(0xffff0fa0, membar, sizeof membar);
    as.write_bytes(0xffff0fc0, cmpxchg, sizeof cmpxchg);
    as.write_bytes(0xffff0fe0, gettls, sizeof gettls);
    as.wr<u32>(0xffff0ffc, 5);
    as.write_bytes(KUSER_SIGRETURN, sigret, sizeof sigret);
    as.write_bytes(KUSER_HOSTRET, hostret, sizeof hostret);
}

// Pending exec image stored per-thread between do_execve (syscall) and finish_exec (after Run returns).
struct ExecRequest {
    std::string path;  // canonical guest path of the binary
    std::vector<u8> image;
    std::vector<std::string> argv, envp;
};
static std::mutex g_exec_mtx;
static std::map<Thread*, ExecRequest> g_exec_reqs;

static int prepare_exec(Process& p, std::string path, std::vector<std::string> argv, ExecRequest& req) {
    for (int depth = 0; depth < 5; depth++) {
        std::vector<u8> data;
        std::string canon;
        Resolved r = vfs::resolve(p, path, true);
        if (r.err) return -r.err;
        if (r.kind != Resolved::HOST) return -lnx::EACCES_;
        struct stat st;
        if (::stat(r.host.c_str(), &st) != 0) return -lnx::ENOENT_;
        if (S_ISDIR(st.st_mode)) return -lnx::EACCES_;
        if (!(st.st_mode & 0111)) return -lnx::EACCES_;
        if (!vfs::read_file(p, path, data, &canon)) return -lnx::ENOENT_;
        if (data.size() >= 2 && data[0] == '#' && data[1] == '!') {
            std::string line;
            for (size_t i = 2; i < data.size() && data[i] != '\n' && i < 256; i++) line += (char)data[i];
            // trim
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) return -lnx::ENOEXEC_;
            line = line.substr(a);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
            std::string interp, arg;
            size_t sp = line.find_first_of(" \t");
            if (sp == std::string::npos) interp = line;
            else {
                interp = line.substr(0, sp);
                arg = line.substr(line.find_first_not_of(" \t", sp));
            }
            std::vector<std::string> nargv;
            nargv.push_back(interp);
            if (!arg.empty()) nargv.push_back(arg);
            nargv.push_back(path);
            for (size_t i = 1; i < argv.size(); i++) nargv.push_back(argv[i]);
            argv = nargv;
            path = interp;
            continue;
        }
        if (!check_elf(data)) return -lnx::ENOEXEC_;
        req.path = canon;
        req.image = std::move(data);
        req.argv = argv;
        return 0;
    }
    return -lnx::ELOOP_;
}

static u32 push_str(AddressSpace& as, u32& sp, const std::string& s) {
    sp -= (u32)s.size() + 1;
    as.write_bytes(sp, s.c_str(), s.size() + 1);
    return sp;
}

// Load image into p's (fresh) address space and set up initial registers on thread t.
static int load_image(Thread& t, ExecRequest& req) {
    Process& p = *t.proc;
    AddressSpace& as = *p.mem;
    LoadedImage exe, interp;
    if (!load_elf(as, req.image, exe, req.path)) return -lnx::ENOEXEC_;
    u32 entry = exe.entry;
    if (!exe.interp.empty()) {
        std::vector<u8> idata;
        std::string icanon;
        if (!vfs::read_file(p, exe.interp, idata, &icanon) || !check_elf(idata)) {
            LOGI("[exec] cannot load interpreter %s\n", exe.interp.c_str());
            return -lnx::ENOENT_;
        }
        if (!load_elf(as, idata, interp, icanon)) return -lnx::ENOEXEC_;
        entry = interp.entry;
    }
    as.brk_start = as.brk_cur = (exe.end + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    write_kuser_page(as);

    // Stack
    as.map(STACK_TOP - STACK_SIZE, STACK_SIZE, 3, "[stack]");
    u32 sp = STACK_TOP;
    u32 execfn = push_str(as, sp, req.path);
    std::vector<u32> envp_ptrs, argv_ptrs;
    for (auto it = req.envp.rbegin(); it != req.envp.rend(); ++it) envp_ptrs.push_back(push_str(as, sp, *it));
    std::reverse(envp_ptrs.begin(), envp_ptrs.end());
    for (auto it = req.argv.rbegin(); it != req.argv.rend(); ++it) argv_ptrs.push_back(push_str(as, sp, *it));
    std::reverse(argv_ptrs.begin(), argv_ptrs.end());
    u32 platform = push_str(as, sp, "v7l");
    sp &= ~15u;
    sp -= 16;
    u32 random = sp;
    for (int i = 0; i < 16; i++) as.wr<u8>(random + i, (u8)arc4random());
    std::vector<std::pair<u32, u32>> auxv = {
        {3, exe.phdr}, {4, 32}, {5, exe.phnum}, {6, GUEST_PAGE}, {7, interp.base}, {8, 0}, {9, exe.entry},
        {11, 0}, {12, 0}, {13, 0}, {14, 0}, {15, platform},
        {16, 0x0037b0d7},  // swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae
        {17, 100}, {23, 0}, {25, random}, {26, 0}, {31, execfn}, {0, 0},
    };
    u32 words = 1 + (u32)argv_ptrs.size() + 1 + (u32)envp_ptrs.size() + 1 + (u32)auxv.size() * 2;
    sp -= words * 4;
    sp &= ~7u;
    u32 w = sp;
    as.wr<u32>(w, (u32)argv_ptrs.size()); w += 4;
    for (u32 a : argv_ptrs) { as.wr<u32>(w, a); w += 4; }
    as.wr<u32>(w, 0); w += 4;
    for (u32 a : envp_ptrs) { as.wr<u32>(w, a); w += 4; }
    as.wr<u32>(w, 0); w += 4;
    for (auto& [k, v] : auxv) { as.wr<u32>(w, k); as.wr<u32>(w + 4, v); w += 8; }

    std::array<u32, 16> regs{};
    regs[13] = sp;
    regs[15] = entry & ~1u;
    t.jit->Regs() = regs;
    t.jit->SetCpsr((entry & 1) ? 0x30 : 0x10);
    std::array<u32, 64> ext{};
    t.jit->ExtRegs() = ext;
    t.jit->SetFpscr(0);
    t.tls = 0;
    p.exe = req.path;
    p.argv = req.argv;
    p.envp = req.envp;
    std::string base = req.path.substr(req.path.rfind('/') + 1);
    if (!req.argv.empty()) {
        std::string a0 = req.argv[0];
        base = a0.substr(a0.rfind('/') + 1);
    }
    p.comm = base.substr(0, 15);
    LOGC(LOG_PROC, "[exec] pid %d: %s (entry %08x interp %s)\n", p.pid, req.path.c_str(), entry, exe.interp.c_str());
    return 0;
}

int do_execve(Thread& t, const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp) {
    ExecRequest req;
    int r = prepare_exec(*t.proc, path, argv, req);
    if (r) return r;
    req.envp = envp;
    {
        std::lock_guard lk(g_exec_mtx);
        g_exec_reqs[&t] = std::move(req);
    }
    t.exec_pending = true;
    t.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    return 0;
}

// Runs on the thread's host thread after leaving the JIT.
int finish_exec(Thread& t) {
    ExecRequest req;
    {
        std::lock_guard lk(g_exec_mtx);
        req = std::move(g_exec_reqs[&t]);
        g_exec_reqs.erase(&t);
    }
    Process& p = *t.proc;
    // Kill other threads
    {
        {
            std::lock_guard lk(p.pmtx);
            for (Thread* o : p.threads)
                if (o != &t) {
                    o->exit_req = true;
                    o->kick();
                }
        }
        for (int i = 0; i < 5000; i++) {
            bool alone;
            {
                std::lock_guard lk(p.pmtx);
                alone = std::all_of(p.threads.begin(), p.threads.end(), [&](Thread* x) { return x == &t || x->is_host_call; });
            }
            if (alone) break;
            usleep(1000);
        }
    }
    hle_process_exec(p);
    // fresh address space
    p.mem = std::make_shared<AddressSpace>();
    p.monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(MAX_CPUS);
    t.clear_child_tid = 0;
    // new JIT bound to new memory
    t.jit.reset();
    t.create_jit();
    // signal handlers: reset caught ones to default; unshare
    {
        auto nh = std::make_shared<SigHandlers>();
        std::lock_guard lk(p.sig->mtx);
        for (int i = 1; i <= 64; i++)
            if (p.sig->act[i].handler == 1) nh->act[i].handler = 1;
        p.sig = nh;
    }
    t.altstack_flags = 2;
    t.altstack_sp = t.altstack_size = 0;
    p.fds->close_on_exec();
    int r = load_image(t, req);
    if (p.vfork_done) {
        *p.vfork_done = true;
        p.vfork_done.reset();
        g_kernel.child_cv.notify_all();
    }
    if (r) {
        LOGI("[exec] pid %d: failed to load %s (%d)\n", p.pid, req.path.c_str(), r);
        process_exit(p, lnx::L_SIGKILL, &t);
        return r;
    }
    return 0;
}

std::shared_ptr<Process> spawn_init(const std::string& path, const std::vector<std::string>& argv,
                                    const std::vector<std::string>& envp) {
    auto p = std::make_shared<Process>();
    {
        std::lock_guard lk(g_kernel.mtx);
        p->pid = g_kernel.alloc_pid();
        g_kernel.procs[p->pid] = p;
    }
    p->ppid = 0;
    p->pgid = p->sid = p->pid;
    p->mem = std::make_shared<AddressSpace>();
    p->monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(MAX_CPUS);
    p->fds = std::make_shared<FdTable>();
    p->sig = std::make_shared<SigHandlers>();
    p->start_time_ns = now_ns();
    std::array<u32, 16> regs{};
    std::array<u32, 64> ext{};
    Thread* t = create_thread(p, regs, 0x10, ext, 0, 0);
    t->tid = p->pid;
    {
        std::lock_guard lk(g_kernel.mtx);
        g_kernel.threads[t->tid] = t;
    }
    // stdio -> /dev/console
    int err = 0;
    for (int fd = 0; fd < 3; fd++) {
        FilePtr f = open_device(*t, "/dev/console", fd == 0 ? 0 : 1, &err);
        if (f) p->fds->install_at(fd, f, false);
    }
    ExecRequest req;
    int r = prepare_exec(*p, path, argv, req);
    if (r) fatal("cannot exec %s: errno %d", path.c_str(), -r);
    req.envp = envp;
    r = load_image(*t, req);
    if (r) fatal("cannot load %s: %d", path.c_str(), r);
    start_thread(t);
    return p;
}
