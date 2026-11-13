// Emulated Linux kernel: processes, threads, fds, signals.
#pragma once
#include "common.h"
#include "memory.h"
#include "vfs.h"
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <array>
#include <deque>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

struct Process;
struct Thread;
struct HleProcState;

constexpr int MAX_CPUS = 64;  // exclusive monitor slots per process

// ---------------------------------------------------------------- fd table
struct FdEntry {
    FilePtr file;
    bool cloexec = false;
};
struct FdTable {
    std::mutex mtx;
    std::vector<FdEntry> fds;
    FilePtr get(int fd);
    int install(FilePtr f, bool cloexec, int min_fd = 0);
    int install_at(int fd, FilePtr f, bool cloexec);
    int close(int fd);
    std::shared_ptr<FdTable> clone();
    void close_on_exec();
    void close_all();
};

// ---------------------------------------------------------------- signals
struct SigAction {
    u32 handler = 0;  // 0 = SIG_DFL, 1 = SIG_IGN
    u32 flags = 0;
    u32 restorer = 0;
    u64 mask = 0;
};
struct SigHandlers {
    std::mutex mtx;
    std::array<SigAction, 65> act{};
};
struct SigInfo {
    int signo = 0;
    int code = 0;
    int pid = 0;
    int uid = 0;
    int status = 0;
    u32 addr = 0;
};
constexpr int ERESTARTSYS = 512;
constexpr int ERESTARTNOHAND = 514;

// ---------------------------------------------------------------- dynarmic glue
class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(Thread* t) : t_(t) {}
    using CoprocReg = Dynarmic::A32::CoprocReg;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override;
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg CRm) override;
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
private:
    Thread* t_;
};

class CpuCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit CpuCallbacks(Thread* t) : t_(t) {}
    u8* mem = nullptr;
    std::uint8_t MemoryRead8(u32 a) override { return mem[a]; }
    std::uint16_t MemoryRead16(u32 a) override { u16 v; memcpy(&v, mem + a, 2); return v; }
    std::uint32_t MemoryRead32(u32 a) override { u32 v; memcpy(&v, mem + a, 4); return v; }
    std::uint64_t MemoryRead64(u32 a) override { u64 v; memcpy(&v, mem + a, 8); return v; }
    void MemoryWrite8(u32 a, std::uint8_t v) override { mem[a] = v; }
    void MemoryWrite16(u32 a, std::uint16_t v) override { memcpy(mem + a, &v, 2); }
    void MemoryWrite32(u32 a, std::uint32_t v) override { memcpy(mem + a, &v, 4); }
    void MemoryWrite64(u32 a, std::uint64_t v) override { memcpy(mem + a, &v, 8); }
    bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t e) override { return __atomic_compare_exchange_n((u8*)(mem + a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t e) override { return __atomic_compare_exchange_n((u16*)(mem + a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t e) override { return __atomic_compare_exchange_n((u32*)(mem + a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t e) override { return __atomic_compare_exchange_n((u64*)(mem + a), &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    void InterpreterFallback(u32 pc, size_t num) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 40; }
private:
    Thread* t_;
};

// ---------------------------------------------------------------- thread
enum class ThreadState { Running, InSyscall, Suspended, Exited };

struct Thread {
    int tid = 0;
    Process* proc = nullptr;
    std::shared_ptr<Process> proc_ref;  // keep process alive
    std::unique_ptr<CpuCallbacks> cb;
    std::shared_ptr<Cp15> cp15;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    int cpu_slot = 0;
    u32 tls = 0;
    u32 clear_child_tid = 0;
    u64 sigmask = 0;
    u64 pending = 0;  // thread-directed pending signals
    std::deque<SigInfo> pending_info;
    u32 altstack_sp = 0, altstack_size = 0, altstack_flags = 2 /*SS_DISABLE*/;
    bool is_host_call = false;  // temporary context used for host->guest calls
    std::atomic<ThreadState> state{ThreadState::Running};
    std::atomic<bool> exit_req{false};
    std::atomic<bool> host_call_done{false};

    // interruptible wait support
    std::mutex wmtx;
    std::condition_variable wcv;
    int wake_pipe[2] = {-1, -1};
    std::atomic<bool> interrupted{false};

    // pending actions set inside syscalls, handled after Run() returns
    bool exec_pending = false;
    std::vector<u32> invalidations;  // pairs addr,len
    std::mutex inval_mtx;

    // syscall restart bookkeeping
    u32 sc_orig_r0 = 0;
    bool in_sigsuspend = false;
    u64 saved_sigmask = 0;
    bool restore_sigmask = false;

    std::thread host;

    void create_jit();
    void run_loop();
    std::array<u32, 16>& R() { return jit->Regs(); }
    // wake this thread from any interruptible wait
    void kick();
    bool signal_pending();  // unblocked pending signal exists
    void invalidate(u32 addr, u32 len);
};

// ---------------------------------------------------------------- process
struct Process : std::enable_shared_from_this<Process> {
    int pid = 0, ppid = 0, pgid = 0, sid = 0;
    std::shared_ptr<AddressSpace> mem;
    std::shared_ptr<FdTable> fds;
    std::shared_ptr<SigHandlers> sig;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor;
    std::string cwd = "/";
    u32 umask = 022;
    std::string exe;              // guest path of executable
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    u64 pending = 0;              // process-directed pending signals
    std::deque<SigInfo> pending_info;
    bool zombie = false;
    bool reaped = false;
    int exit_status = 0;          // wait status encoding
    std::vector<Thread*> threads;
    std::array<bool, MAX_CPUS> cpu_used{};
    std::mutex pmtx;              // threads/cpu slots
    // vfork
    std::shared_ptr<std::atomic<bool>> vfork_done;
    // suspension for fork
    std::atomic<bool> suspend_req{false};
    std::condition_variable suspend_cv;
    std::mutex suspend_mtx;
    // itimer (ITIMER_REAL)
    u64 alarm_deadline_ns = 0, alarm_interval_ns = 0;
    // HLE per-process state
    std::shared_ptr<HleProcState> hle;
    u64 start_time_ns = 0;
    std::string comm;
    std::map<int, std::pair<u64, u64>> rlimits;  // resource -> (cur, max); inherited on fork

    int alloc_cpu_slot();
    void free_cpu_slot(int s);
    void invalidate_all(u32 addr, u32 len);
    void suspend_others(Thread* self);
    void resume_others();
};

// ---------------------------------------------------------------- kernel global
struct Kernel {
    std::mutex mtx;  // process table
    std::condition_variable child_cv;
    std::map<int, std::shared_ptr<Process>> procs;
    std::map<int, Thread*> threads;
    int next_pid = 1;
    std::string rootfs, datadir, rundir;
    u64 boot_time_ns = 0;
    std::atomic<bool> shutting_down{false};
    std::atomic<int> exit_code{0};
    bool strace = false;
    std::string hostname = "clover";

    int alloc_pid() { return next_pid++; }
    std::shared_ptr<Process> find_proc(int pid);
    Thread* find_thread(int tid);
};
extern Kernel g_kernel;

u64 now_ns();       // monotonic
u64 realtime_ns();

// syscalls.cpp
void do_syscall(Thread& t);
// signals.cpp
void send_signal_proc(Process& p, const SigInfo& si);
void send_signal_thread(Thread& t, const SigInfo& si);
bool deliver_signals(Thread& t);  // returns false if thread should exit
void setup_frame_after_syscall(Thread& t, s32 ret);
void do_rt_sigreturn(Thread& t, bool rt);
void process_exit(Process& p, int status, Thread* self);  // terminate whole process
void thread_exit(Thread& t, int code);
// loader.cpp
int do_execve(Thread& t, const std::string& path, const std::vector<std::string>& argv,
              const std::vector<std::string>& envp);
int finish_exec(Thread& t);
std::shared_ptr<Process> spawn_init(const std::string& path, const std::vector<std::string>& argv,
                                    const std::vector<std::string>& envp);
void write_kuser_page(AddressSpace& as);
// threads
Thread* create_thread(std::shared_ptr<Process> p, const std::array<u32, 16>& regs, u32 cpsr,
                      const std::array<u32, 64>& ext, u32 fpscr, u32 tls);
void start_thread(Thread* t);
// interruptible waits
// Wait on a host fd for events (POLLIN/POLLOUT). Returns 0 when ready, -EINTR if signalled, -ETIMEDOUT on timeout.
int wait_fd(Thread& t, int fd, short events, s64 timeout_ms = -1);
// sleep interruptibly; returns 0 or -EINTR
int sleep_ns(Thread& t, u64 ns, u64* remaining = nullptr);

// host->guest call: run guest function `fn` with args on the current host thread.
u32 guest_call(std::shared_ptr<Process> p, u32 fn, std::initializer_list<u32> args, u32 tls);

// HLE (hle.cpp)
bool hle_dispatch(Thread& t, u32 idx);
std::vector<u8> hle_build_stub(const std::string& soname, bool* found);
void hle_process_exit(Process& p);
void hle_process_fork(Process& parent, Process& child);
void hle_process_exec(Process& p);

constexpr u32 SVC_HLE_BASE = 0x100000;
constexpr u32 SVC_HOST_RETURN = 0xffffff;
constexpr u32 KUSER_SIGRETURN = 0xffff0500;
constexpr u32 KUSER_HOSTRET = 0xffff0508;
