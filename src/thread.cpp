// Threads, CPU glue (dynarmic callbacks), fd tables, interruptible waits.
#include "kernel.h"
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#include <algorithm>

Kernel g_kernel;

u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
u64 realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

std::shared_ptr<Process> Kernel::find_proc(int pid) {
    auto it = procs.find(pid);
    return it == procs.end() ? nullptr : it->second;
}
Thread* Kernel::find_thread(int tid) {
    auto it = threads.find(tid);
    return it == threads.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- fd table
FilePtr FdTable::get(int fd) {
    std::lock_guard lk(mtx);
    if (fd < 0 || fd >= (int)fds.size()) return nullptr;
    return fds[fd].file;
}
int FdTable::install(FilePtr f, bool cloexec, int min_fd) {
    std::lock_guard lk(mtx);
    for (int i = min_fd; i < 4096; i++) {
        if (i >= (int)fds.size()) fds.resize(i + 1);
        if (!fds[i].file) {
            fds[i].file = std::move(f);
            fds[i].cloexec = cloexec;
            return i;
        }
    }
    return -lnx::EMFILE_;
}
int FdTable::install_at(int fd, FilePtr f, bool cloexec) {
    FilePtr old;
    {
        std::lock_guard lk(mtx);
        if (fd < 0 || fd >= 4096) return -lnx::EBADF_;
        if (fd >= (int)fds.size()) fds.resize(fd + 1);
        old = std::move(fds[fd].file);
        fds[fd].file = std::move(f);
        fds[fd].cloexec = cloexec;
    }
    if (old && old.use_count() == 1) old->on_close();
    return fd;
}
int FdTable::close(int fd) {
    FilePtr old;
    {
        std::lock_guard lk(mtx);
        if (fd < 0 || fd >= (int)fds.size() || !fds[fd].file) return -lnx::EBADF_;
        old = std::move(fds[fd].file);
        fds[fd].cloexec = false;
    }
    if (old.use_count() == 1) old->on_close();
    return 0;
}
std::shared_ptr<FdTable> FdTable::clone() {
    auto n = std::make_shared<FdTable>();
    std::lock_guard lk(mtx);
    n->fds = fds;
    return n;
}
void FdTable::close_on_exec() {
    std::vector<FilePtr> olds;
    {
        std::lock_guard lk(mtx);
        for (auto& e : fds)
            if (e.file && e.cloexec) {
                olds.push_back(std::move(e.file));
                e.cloexec = false;
            }
    }
    for (auto& f : olds)
        if (f.use_count() == 1) f->on_close();
}
void FdTable::close_all() {
    std::vector<FilePtr> olds;
    {
        std::lock_guard lk(mtx);
        for (auto& e : fds)
            if (e.file) olds.push_back(std::move(e.file));
        fds.clear();
    }
    for (auto& f : olds)
        if (f.use_count() == 1) f->on_close();
}

// ---------------------------------------------------------------- process helpers
int Process::alloc_cpu_slot() {
    std::lock_guard lk(pmtx);
    for (int i = 0; i < MAX_CPUS; i++)
        if (!cpu_used[i]) {
            cpu_used[i] = true;
            return i;
        }
    fatal("too many guest threads in process %d", pid);
}
void Process::free_cpu_slot(int s) {
    std::lock_guard lk(pmtx);
    cpu_used[s] = false;
    if (monitor) monitor->ClearProcessor(s);
}
void Process::invalidate_all(u32 addr, u32 len) {
    std::lock_guard lk(pmtx);
    for (Thread* t : threads)
        if (t->jit) t->jit->InvalidateCacheRange(addr, len);
}
void Process::suspend_others(Thread* self) {
    suspend_req = true;
    std::vector<Thread*> others;
    {
        std::lock_guard lk(pmtx);
        for (Thread* t : threads)
            if (t != self && !t->is_host_call) {
                others.push_back(t);
                if (t->jit) t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            }
    }
    // wait until they are out of guest code
    for (Thread* t : others) {
        for (int i = 0; i < 2000; i++) {
            auto s = t->state.load();
            if (s != ThreadState::Running) break;
            usleep(500);
        }
    }
}
void Process::resume_others() {
    {
        std::lock_guard lk(suspend_mtx);
        suspend_req = false;
    }
    suspend_cv.notify_all();
}

// ---------------------------------------------------------------- CP15
Cp15::CallbackOrAccessOneWord Cp15::CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    using CR = Dynarmic::A32::CoprocReg;
    if (!two && opc1 == 0 && CRn == CR::C13 && CRm == CR::C0 && opc2 == 3) return &t_->tls;  // TPIDRURO
    if (!two && opc1 == 0 && CRn == CR::C13 && CRm == CR::C0 && opc2 == 2) return &t_->tls;  // TPIDRURW (approx)
    static u32 zero = 0;
    LOGI("[cpu] unhandled MRC p15 opc1=%u CRn=%d CRm=%d opc2=%u\n", opc1, (int)CRn, (int)CRm, opc2);
    return &zero;
}
Cp15::CallbackOrAccessOneWord Cp15::CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
    using CR = Dynarmic::A32::CoprocReg;
    if (!two && opc1 == 0 && CRn == CR::C7 && CRm == CR::C10 && (opc2 == 5 || opc2 == 4)) {  // legacy barriers
        static u32 dummy;
        return &dummy;
    }
    if (!two && opc1 == 0 && CRn == CR::C7 && CRm == CR::C5 && opc2 == 4) {  // CP15ISB
        static u32 dummy;
        return &dummy;
    }
    static u32 sink;
    LOGI("[cpu] unhandled MCR p15 opc1=%u CRn=%d CRm=%d opc2=%u\n", opc1, (int)CRn, (int)CRm, opc2);
    return &sink;
}
Cp15::CallbackOrAccessTwoWords Cp15::CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) {
    // CNTVCT etc. not used on this firmware.
    return std::monostate{};
}

// ---------------------------------------------------------------- callbacks
void CpuCallbacks::InterpreterFallback(u32 pc, size_t num) {
    u32 insn = MemoryRead32(pc);
    LOGI("[cpu] pid %d tid %d: interpreter fallback at %08x insn %08x (unsupported)\n", t_->proc->pid, t_->tid, pc, insn);
    SigInfo si;
    si.signo = lnx::L_SIGILL;
    si.addr = pc;
    t_->R()[15] = pc;
    send_signal_thread(*t_, si);
    t_->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void CpuCallbacks::ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::Yield:
    case E::SendEvent:
    case E::SendEventLocal:
    case E::WaitForEvent:
    case E::WaitForInterrupt:
    case E::PreloadData:
    case E::PreloadDataWithIntentToWrite:
    case E::PreloadInstruction:
        t_->R()[15] = pc + 4;
        return;
    default:
        break;
    }
    u32 insn = MemoryRead32(pc);
    LOGI("[cpu] pid %d (%s) tid %d: exception %d at %08x insn %08x lr=%08x\n", t_->proc->pid, t_->proc->comm.c_str(), t_->tid,
         (int)e, pc, insn, t_->R()[14]);
    SigInfo si;
    si.signo = (e == E::Breakpoint) ? lnx::L_SIGTRAP : lnx::L_SIGILL;
    si.addr = pc;
    t_->R()[15] = pc;
    send_signal_thread(*t_, si);
    t_->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void CpuCallbacks::CallSVC(std::uint32_t swi) {
    Thread& t = *t_;
    if (swi == 0) {
        t.state = ThreadState::InSyscall;
        do_syscall(t);
        if (t.state != ThreadState::Exited) t.state = ThreadState::Running;
        return;
    }
    if (swi == SVC_HOST_RETURN) {
        t.host_call_done = true;
        t.jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        return;
    }
    if (swi >= SVC_HLE_BASE && swi < SVC_HLE_BASE + 0x10000) {
        t.state = ThreadState::InSyscall;
        // stubs are "svc #n; bx lr": execution continues at the bx and returns to the caller
        if (!hle_dispatch(t, swi - SVC_HLE_BASE)) LOGI("[hle] unknown HLE index %u\n", swi - SVC_HLE_BASE);
        if (t.state != ThreadState::Exited) t.state = ThreadState::Running;
        return;
    }
    LOGI("[cpu] unknown svc #%x at %08x\n", swi, t.R()[15] - 4);
}

// ---------------------------------------------------------------- threads
void Thread::create_jit() {
    cb = std::make_unique<CpuCallbacks>(this);
    cb->mem = proc->mem->base();
    cp15 = std::make_shared<Cp15>(this);
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = cb.get();
    cfg.processor_id = cpu_slot;
    cfg.global_monitor = proc->monitor.get();
    cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
    cfg.fastmem_pointer = (uintptr_t)proc->mem->base();
    cfg.recompile_on_fastmem_failure = true;
    cfg.coprocessors[15] = cp15;
    cfg.enable_cycle_counting = false;
    cfg.code_cache_size = 64u << 20;
    cfg.define_unpredictable_behaviour = true;
    // Float shortcuts that don't affect integer/atomic semantics (NaN payloads, reciprocal estimates).
    cfg.unsafe_optimizations = true;
    cfg.optimizations = Dynarmic::all_safe_optimizations | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                        Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                        Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

void Thread::kick() {
    {
        std::lock_guard lk(wmtx);
        interrupted = true;
        wcv.notify_all();
    }
    if (wake_pipe[1] >= 0) {
        char c = 1;
        (void)::write(wake_pipe[1], &c, 1);
    }
    if (jit) jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

void Thread::invalidate(u32 addr, u32 len) {
    if (jit) jit->InvalidateCacheRange(addr, len);
}

Thread* create_thread(std::shared_ptr<Process> p, const std::array<u32, 16>& regs, u32 cpsr,
                      const std::array<u32, 64>& ext, u32 fpscr, u32 tls) {
    Thread* t = new Thread();
    t->proc = p.get();
    t->proc_ref = p;
    t->tls = tls;
    t->cpu_slot = p->alloc_cpu_slot();
    if (pipe(t->wake_pipe) == 0) {
        fcntl(t->wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(t->wake_pipe[1], F_SETFL, O_NONBLOCK);
        fcntl(t->wake_pipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(t->wake_pipe[1], F_SETFD, FD_CLOEXEC);
    }
    t->create_jit();
    t->jit->Regs() = regs;
    t->jit->SetCpsr(cpsr);
    t->jit->ExtRegs() = ext;
    t->jit->SetFpscr(fpscr);
    {
        std::lock_guard lk(p->pmtx);
        p->threads.push_back(t);
    }
    return t;
}

void start_thread(Thread* t) {
    t->host = std::thread([t] { t->run_loop(); });
    t->host.detach();
}

static void finalize_thread(Thread* t);

void Thread::run_loop() {
    char name[64];
    snprintf(name, sizeof name, "guest %d/%d", proc->pid, tid);
    pthread_setname_np(name);
    // Highest QoS: minimal timer leeway and P-core scheduling (the guest is a real-time console).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    while (!exit_req) {
        if (proc->suspend_req) {
            state = ThreadState::Suspended;
            std::unique_lock lk(proc->suspend_mtx);
            proc->suspend_cv.wait(lk, [&] { return !proc->suspend_req.load() || exit_req.load(); });
        }
        if (exit_req) break;
        state = ThreadState::Running;
        jit->Run();
        if (exec_pending) {
            exec_pending = false;
            finish_exec(*this);
            continue;
        }
        if (exit_req) break;
        if (!deliver_signals(*this)) break;
    }
    finalize_thread(this);
}

// Called on the thread's own host thread after it leaves guest code for good.
static void finalize_thread(Thread* t) {
    Process* p = t->proc;
    std::shared_ptr<Process> pref = t->proc_ref;
    t->state = ThreadState::Exited;
    // CLONE_CHILD_CLEARTID
    if (t->clear_child_tid && p->mem) {
        p->mem->wr<u32>(t->clear_child_tid, 0);
        extern int futex_wake(Process& p, u32 addr, int n, u32 bitset);
        futex_wake(*p, t->clear_child_tid, 1, 0xffffffff);
    }
    bool last = false;
    {
        std::lock_guard lk(p->pmtx);
        auto& v = p->threads;
        v.erase(std::remove(v.begin(), v.end(), t), v.end());
        last = std::none_of(v.begin(), v.end(), [](Thread* x) { return !x->is_host_call; });
        p->cpu_used[t->cpu_slot] = false;
    }
    if (p->monitor) p->monitor->ClearProcessor(t->cpu_slot);
    {
        std::lock_guard lk(g_kernel.mtx);
        g_kernel.threads.erase(t->tid);
    }
    if (t->wake_pipe[0] >= 0) ::close(t->wake_pipe[0]);
    if (t->wake_pipe[1] >= 0) ::close(t->wake_pipe[1]);
    t->jit.reset();
    delete t;
    if (last) {
        extern void process_finalize(std::shared_ptr<Process> p);
        process_finalize(pref);
    }
}

bool Thread::signal_pending() {
    extern std::mutex g_sig_mtx;
    std::lock_guard lk(g_sig_mtx);
    u64 pend = pending | proc->pending;
    return (pend & ~sigmask) != 0 || exit_req;
}

int wait_fd(Thread& t, int fd, short events, s64 timeout_ms) {
    // drain wake pipe
    char buf[64];
    while (::read(t.wake_pipe[0], buf, sizeof buf) > 0) {}
    if (t.signal_pending()) return -lnx::EINTR_;
    pollfd pf[2] = {{fd, events, 0}, {t.wake_pipe[0], POLLIN, 0}};
    u64 deadline = timeout_ms >= 0 ? now_ns() + (u64)timeout_ms * 1000000ull : 0;
    while (true) {
        int to = -1;
        if (timeout_ms >= 0) {
            u64 n = now_ns();
            if (n >= deadline) return -lnx::ETIMEDOUT_;
            to = (int)((deadline - n) / 1000000ull) + 1;
        }
        int r = ::poll(pf, 2, to);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -host_errno_to_linux(errno);
        }
        if (pf[0].revents) return 0;
        if (pf[1].revents) {
            while (::read(t.wake_pipe[0], buf, sizeof buf) > 0) {}
            if (t.signal_pending()) return -lnx::EINTR_;
        }
        if (r == 0 && timeout_ms >= 0) return -lnx::ETIMEDOUT_;
    }
}

int sleep_ns(Thread& t, u64 ns, u64* remaining) {
    // Interruptible coarse wait on the thread's condition variable, then a precise nanosleep for the
    // last ~2 ms: macOS timer leeway would otherwise add 2-4 ms to every sleep, which wrecks guest
    // frame pacing (the firmware paces its frames with nanosleep).
    u64 deadline = now_ns() + ns;
    while (true) {
        if (t.signal_pending()) {
            u64 n = now_ns();
            if (remaining) *remaining = deadline > n ? deadline - n : 0;
            return -lnx::EINTR_;
        }
        u64 n = now_ns();
        if (n >= deadline) return 0;
        u64 rem = deadline - n;
        if (rem > 2500000ull) {
            std::unique_lock lk(t.wmtx);
            t.interrupted = false;
            t.wcv.wait_for(lk, std::chrono::nanoseconds(std::min<u64>(rem - 2000000ull, 50000000ull)),
                           [&] { return t.interrupted.load(); });
        } else {
            timespec ts{0, (long)rem};
            nanosleep(&ts, nullptr);
        }
    }
}

// ---------------------------------------------------------------- host -> guest calls
// Runs a guest function synchronously on the calling host thread, using a temporary CPU context.
u32 guest_call(std::shared_ptr<Process> p, u32 fn, std::initializer_list<u32> args, u32 tls) {
    thread_local std::map<Process*, std::pair<Thread*, u32>> ctx_cache;  // per host-thread context + stack
    Thread* t;
    u32 stack;
    auto it = ctx_cache.find(p.get());
    if (it == ctx_cache.end() || !it->second.first) {
        std::array<u32, 16> regs{};
        std::array<u32, 64> ext{};
        t = create_thread(p, regs, 0x10, ext, 0x03000000, tls);
        t->is_host_call = true;
        {
            std::lock_guard lk(g_kernel.mtx);
            t->tid = g_kernel.alloc_pid();
            g_kernel.threads[t->tid] = t;
        }
        u32 sz = 256 * 1024;
        stack = p->mem->find_free(sz);
        p->mem->map(stack, sz, 3, "[hostcall-stack]");
        stack += sz;
        ctx_cache[p.get()] = {t, stack};
    } else {
        t = it->second.first;
        stack = it->second.second;
    }
    t->tls = tls;
    auto& R = t->jit->Regs();
    int i = 0;
    for (u32 a : args) {
        if (i < 4) R[i] = a;
        i++;
    }
    u32 sp = stack - 64;
    i = 0;
    for (u32 a : args) {
        if (i >= 4) p->mem->wr<u32>(sp + (i - 4) * 4, a);
        i++;
    }
    R[13] = sp;
    R[14] = KUSER_HOSTRET;
    R[15] = fn & ~1u;
    t->jit->SetCpsr((fn & 1) ? 0x30 : 0x10);
    t->host_call_done = false;
    while (!t->host_call_done && !t->exit_req && !p->zombie) {
        t->state = ThreadState::Running;
        t->jit->Run();
    }
    t->state = ThreadState::InSyscall;
    return t->jit->Regs()[0];
}
