// Signal delivery, sigreturn, process exit.
#include "kernel.h"
#include <algorithm>
#include "hle.h"

std::mutex g_sig_mtx;

static inline u64 sigbit(int s) { return 1ull << (s - 1); }

static bool default_ignored(int s) {
    return s == lnx::L_SIGCHLD || s == lnx::L_SIGCONT || s == lnx::L_SIGURG || s == lnx::L_SIGWINCH || s == lnx::L_SIGSTOP ||
           s == lnx::L_SIGTSTP || s == lnx::L_SIGTTIN || s == lnx::L_SIGTTOU;
}

void send_signal_thread(Thread& t, const SigInfo& si) {
    if (si.signo <= 0 || si.signo > 64) return;
    LOGC(LOG_SIGNAL, "[sig] -> tid %d (pid %d) signal %d\n", t.tid, t.proc->pid, si.signo);
    if (si.signo == lnx::L_SIGKILL) {
        process_exit(*t.proc, lnx::L_SIGKILL, nullptr);
        return;
    }
    {
        std::lock_guard lk(g_sig_mtx);
        if (!(t.pending & sigbit(si.signo)) || si.signo >= 32) t.pending_info.push_back(si);
        t.pending |= sigbit(si.signo);
    }
    t.kick();
}

void send_signal_proc(Process& p, const SigInfo& si) {
    if (si.signo <= 0 || si.signo > 64) return;
    if (p.zombie) return;
    LOGC(LOG_SIGNAL, "[sig] -> pid %d (%s) signal %d\n", p.pid, p.comm.c_str(), si.signo);
    if (si.signo == lnx::L_SIGKILL) {
        process_exit(p, lnx::L_SIGKILL, nullptr);
        return;
    }
    // libSDL2 (events subsystem) converts SIGINT/SIGTERM into an SDL_QUIT event.
    if ((si.signo == lnx::L_SIGTERM || si.signo == lnx::L_SIGINT) && p.hle && p.hle->events_enabled) {
        bool dfl;
        {
            std::lock_guard lk(p.sig->mtx);
            dfl = p.sig->act[si.signo].handler == 0;
        }
        if (dfl) {
            extern void hle_sdl_push_quit(Process & p);
            LOGC(LOG_SIGNAL, "[sig] pid %d: signal %d -> SDL_QUIT\n", p.pid, si.signo);
            hle_sdl_push_quit(p);
            return;
        }
    }
    // Discard ignored signals early (so they don't wake anything)
    {
        std::lock_guard lk(p.sig->mtx);
        auto& a = p.sig->act[si.signo];
        if (a.handler == 1 || (a.handler == 0 && default_ignored(si.signo))) {
            if (si.signo != lnx::L_SIGCHLD) return;
            return;
        }
    }
    {
        std::lock_guard lk(g_sig_mtx);
        if (!(p.pending & sigbit(si.signo)) || si.signo >= 32) p.pending_info.push_back(si);
        p.pending |= sigbit(si.signo);
    }
    // Kick a thread that doesn't block it; done under pmtx so the thread can't be freed meanwhile.
    std::lock_guard lk2(p.pmtx);
    for (Thread* t : p.threads) {
        if (t->is_host_call) continue;
        if (!(t->sigmask & sigbit(si.signo))) {
            t->kick();
            break;
        }
    }
}

// ARM rt_sigframe layout
constexpr u32 SIGINFO_SIZE = 128;
constexpr u32 UC_FLAGS = 0, UC_LINK = 4, UC_STACK = 8, UC_MCONTEXT = 20, UC_SIGMASK = 104, UC_REGSPACE = 232;
constexpr u32 UC_SIZE = UC_REGSPACE + 128 * 4;
constexpr u32 FRAME_SIZE = SIGINFO_SIZE + UC_SIZE;
constexpr u32 VFP_MAGIC = 0x56465001;

static bool pick_signal(Thread& t, int& signo, SigInfo& info) {
    std::lock_guard lk(g_sig_mtx);
    u64 avail = (t.pending | t.proc->pending) & ~t.sigmask;
    // SIGKILL/SIGSTOP can't be blocked; handled elsewhere
    if (!avail) return false;
    int s = __builtin_ctzll(avail) + 1;
    signo = s;
    info = SigInfo{};
    info.signo = s;
    auto take = [&](u64& pend, std::deque<SigInfo>& q) {
        for (auto it = q.begin(); it != q.end(); ++it)
            if (it->signo == s) {
                info = *it;
                q.erase(it);
                break;
            }
        bool more = std::any_of(q.begin(), q.end(), [&](const SigInfo& x) { return x.signo == s; });
        if (!more) pend &= ~sigbit(s);
    };
    if (t.pending & sigbit(s)) take(t.pending, t.pending_info);
    else take(t.proc->pending, t.proc->pending_info);
    return true;
}

static void build_frame(Thread& t, int signo, const SigInfo& info, const SigAction& act, u64 oldmask) {
    auto& R = t.R();
    AddressSpace& m = *t.proc->mem;
    u32 sp = R[13];
    if ((act.flags & lnx::L_SA_ONSTACK) && t.altstack_flags == 0 && t.altstack_size &&
        !(sp > t.altstack_sp && sp <= t.altstack_sp + t.altstack_size)) {
        sp = t.altstack_sp + t.altstack_size;
    }
    bool rt = act.flags & lnx::L_SA_SIGINFO;
    u32 fsize = rt ? FRAME_SIZE : UC_SIZE + 8;
    sp = (sp - fsize) & ~7u;
    u32 frame = rt ? sp : sp - SIGINFO_SIZE;  // non-rt: ucontext at sp
    // siginfo
    memset(m.ptr(sp), 0, fsize);
    if (rt) {
        m.wr<s32>(frame + 0, signo);
        m.wr<s32>(frame + 4, 0);
        m.wr<s32>(frame + 8, info.code);
        if (signo == lnx::L_SIGCHLD) {
            m.wr<s32>(frame + 12, info.pid);
            m.wr<u32>(frame + 16, info.uid);
            m.wr<s32>(frame + 20, info.status);
        } else if (signo == lnx::L_SIGSEGV || signo == lnx::L_SIGBUS || signo == lnx::L_SIGILL || signo == lnx::L_SIGFPE) {
            m.wr<u32>(frame + 12, info.addr);
        } else {
            m.wr<s32>(frame + 12, info.pid);
            m.wr<u32>(frame + 16, info.uid);
        }
    }
    u32 uc = frame + SIGINFO_SIZE;
    m.wr<u32>(uc + UC_FLAGS, 0);
    m.wr<u32>(uc + UC_LINK, 0);
    m.wr<u32>(uc + UC_STACK + 0, t.altstack_sp);
    m.wr<u32>(uc + UC_STACK + 4, t.altstack_flags);
    m.wr<u32>(uc + UC_STACK + 8, t.altstack_size);
    u32 mc = uc + UC_MCONTEXT;
    m.wr<u32>(mc + 0, 0);  // trap_no
    m.wr<u32>(mc + 4, 0);  // error_code
    m.wr<u32>(mc + 8, (u32)oldmask);
    for (int i = 0; i < 16; i++) m.wr<u32>(mc + 12 + i * 4, R[i]);
    m.wr<u32>(mc + 76, t.jit->Cpsr());
    m.wr<u32>(mc + 80, info.addr);
    m.wr<u64>(uc + UC_SIGMASK, oldmask);
    // VFP
    u32 rs = uc + UC_REGSPACE;
    m.wr<u32>(rs + 0, VFP_MAGIC);
    m.wr<u32>(rs + 4, 8 + 32 * 8 + 8 + 16);
    auto& X = t.jit->ExtRegs();
    for (int i = 0; i < 64; i++) m.wr<u32>(rs + 8 + i * 4, X[i]);
    m.wr<u32>(rs + 8 + 256, t.jit->Fpscr());

    R[0] = signo;
    R[1] = rt ? frame : 0;
    R[2] = rt ? uc : 0;
    R[13] = sp;
    R[14] = (act.flags & lnx::L_SA_RESTORER) ? act.restorer : KUSER_SIGRETURN;
    u32 h = act.handler;
    u32 cpsr = t.jit->Cpsr();
    if (h & 1) cpsr |= 0x20; else cpsr &= ~0x20u;
    cpsr &= ~(0x0600fc00u);  // clear IT bits
    t.jit->SetCpsr(cpsr);
    R[15] = h & ~1u;
}

void do_rt_sigreturn(Thread& t, bool rt) {
    auto& R = t.R();
    AddressSpace& m = *t.proc->mem;
    u32 frame = R[13];
    u32 uc = rt ? frame + SIGINFO_SIZE : frame;
    u32 mc = uc + UC_MCONTEXT;
    for (int i = 0; i < 16; i++) R[i] = m.rd<u32>(mc + 12 + i * 4);
    u32 cpsr = m.rd<u32>(mc + 76);
    t.jit->SetCpsr((cpsr & 0xf80f0220u) | 0x10);
    if (cpsr & 0x20) R[15] &= ~1u;
    u32 rs = uc + UC_REGSPACE;
    if (m.rd<u32>(rs) == VFP_MAGIC) {
        auto& X = t.jit->ExtRegs();
        for (int i = 0; i < 64; i++) X[i] = m.rd<u32>(rs + 8 + i * 4);
        t.jit->SetFpscr(m.rd<u32>(rs + 8 + 256));
    }
    u64 mask = m.rd<u64>(uc + UC_SIGMASK);
    mask &= ~(sigbit(lnx::L_SIGKILL) | sigbit(lnx::L_SIGSTOP));
    {
        std::lock_guard lk(g_sig_mtx);
        t.sigmask = mask;
    }
}

// Deliver pending signals. syscall_ret: if non-null, we are at a syscall boundary with that return value
// (may be a restart code). Returns false if the thread must exit.
static bool deliver_impl(Thread& t, s32* syscall_ret) {
    if (t.is_host_call) return true;
    if (t.exit_req) return false;
    int signo;
    SigInfo info;
    while (pick_signal(t, signo, info)) {
        SigAction act;
        {
            std::lock_guard lk(t.proc->sig->mtx);
            act = t.proc->sig->act[signo];
            if (act.handler > 1 && (act.flags & lnx::L_SA_RESETHAND)) t.proc->sig->act[signo] = SigAction{};
        }
        if (act.handler == 1 || (act.handler == 0 && default_ignored(signo))) continue;
        if (act.handler == 0) {
            LOGC(LOG_SIGNAL | LOG_PROC, "[sig] pid %d (%s) killed by signal %d\n", t.proc->pid, t.proc->comm.c_str(), signo);
            if (signo == lnx::L_SIGSEGV || signo == lnx::L_SIGILL || signo == lnx::L_SIGBUS || signo == lnx::L_SIGABRT ||
                signo == lnx::L_SIGFPE)
                LOGI("[sig] pid %d (%s) fatal signal %d at pc=%08x lr=%08x\n", t.proc->pid, t.proc->comm.c_str(), signo,
                     t.R()[15], t.R()[14]);
            process_exit(*t.proc, signo, &t);
            return false;
        }
        // handler
        if (syscall_ret) {
            s32 r = *syscall_ret;
            if (r == -ERESTARTSYS) {
                if (act.flags & lnx::L_SA_RESTART) {
                    t.R()[0] = t.sc_orig_r0;
                    t.R()[15] -= 4;
                } else t.R()[0] = (u32)-lnx::EINTR_;
            } else if (r == -ERESTARTNOHAND) {
                t.R()[0] = (u32)-lnx::EINTR_;
            }
            syscall_ret = nullptr;
        }
        u64 oldmask;
        {
            std::lock_guard lk(g_sig_mtx);
            oldmask = t.restore_sigmask ? t.saved_sigmask : t.sigmask;
            t.restore_sigmask = false;
            t.sigmask |= act.mask;
            if (!(act.flags & lnx::L_SA_NODEFER)) t.sigmask |= sigbit(signo);
            t.sigmask &= ~(sigbit(lnx::L_SIGKILL) | sigbit(lnx::L_SIGSTOP));
        }
        LOGC(LOG_SIGNAL, "[sig] tid %d deliver %d handler %08x\n", t.tid, signo, act.handler);
        build_frame(t, signo, info, act, oldmask);
        return true;  // deliver one at a time
    }
    // no handler ran
    if (syscall_ret) {
        s32 r = *syscall_ret;
        if (r == -ERESTARTSYS || r == -ERESTARTNOHAND) {
            t.R()[0] = t.sc_orig_r0;
            t.R()[15] -= 4;
        }
    }
    if (t.restore_sigmask) {
        std::lock_guard lk(g_sig_mtx);
        t.sigmask = t.saved_sigmask;
        t.restore_sigmask = false;
    }
    return !t.exit_req;
}

bool deliver_signals(Thread& t) { return deliver_impl(t, nullptr); }

void setup_frame_after_syscall(Thread& t, s32 ret) {
    if (!deliver_impl(t, &ret)) {
        t.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
}

// ---------------------------------------------------------------- exit
void process_exit(Process& p, int status, Thread* self) {
    {
        // first exit wins; bit 16 marks "status recorded"
        std::lock_guard lk(g_kernel.mtx);
        if (!(p.exit_status & 0x10000)) p.exit_status = status | 0x10000;
    }
    {
        std::lock_guard lk(p.pmtx);
        for (Thread* t : p.threads) {
            t->exit_req = true;
            if (t != self) t->kick();
            else if (t->jit) t->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }
    p.resume_others();
}

void thread_exit(Thread& t, int code) {
    t.exit_req = true;
    t.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    (void)code;
}

void process_finalize(std::shared_ptr<Process> p) {
    // Kill leftover host-call contexts
    {
        std::lock_guard lk(p->pmtx);
        for (Thread* t : p->threads) t->exit_req = true;
    }
    hle_process_exit(*p);
    if (p->fds) p->fds->close_all();
    int status = p->exit_status & 0xffff;
    std::shared_ptr<Process> parent;
    bool autoreap = false;
    {
        std::lock_guard lk(g_kernel.mtx);
        p->exit_status = status;
        p->zombie = true;
        // reparent children to init
        for (auto& [pid, c] : g_kernel.procs) {
            if (c->ppid == p->pid) {
                c->ppid = 1;
            }
        }
        parent = g_kernel.find_proc(p->ppid);
        if (parent) {
            std::lock_guard lk2(parent->sig->mtx);
            auto& a = parent->sig->act[lnx::L_SIGCHLD];
            autoreap = (a.handler == 1) || (a.flags & lnx::L_SA_NOCLDWAIT);
        }
        if (autoreap || !parent) {
            p->reaped = true;
            g_kernel.procs.erase(p->pid);
        }
    }
    LOGC(LOG_PROC, "[proc] pid %d (%s) exited status 0x%x\n", p->pid, p->comm.c_str(), status);
    if (p->vfork_done) {
        *p->vfork_done = true;
    }
    // release address space now (keep pointer if shared via vfork)
    p->mem.reset();
    g_kernel.child_cv.notify_all();
    if (parent && !autoreap) {
        SigInfo si;
        si.signo = lnx::L_SIGCHLD;
        si.pid = p->pid;
        si.code = (status & 0x7f) ? 2 /*CLD_KILLED*/ : 1 /*CLD_EXITED*/;
        si.status = (status & 0x7f) ? (status & 0x7f) : ((status >> 8) & 0xff);
        send_signal_proc(*parent, si);
    }
    if (p->pid == 1) {
        LOGI("[kernel] init exited (status 0x%x); shutting down\n", status);
        g_kernel.shutting_down = true;
    }
    // Wake parent threads waiting in wait4
    if (parent) {
        std::lock_guard lk(parent->pmtx);
        for (Thread* t : parent->threads) t->kick();
    }
}
