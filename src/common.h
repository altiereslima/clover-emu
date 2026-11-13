// Common definitions for the Clover (NES Classic) user-mode emulator.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <atomic>
#include <cstring>
#include "platform.h"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// ---------------------------------------------------------------- logging
enum LogCat : u32 {
    LOG_SYSCALL = 1u << 0,
    LOG_PROC = 1u << 1,
    LOG_VFS = 1u << 2,
    LOG_HLE = 1u << 3,
    LOG_GL = 1u << 4,
    LOG_SIGNAL = 1u << 5,
    LOG_MEM = 1u << 6,
    LOG_DEV = 1u << 7,
};
extern u32 g_log_mask;
void log_raw(const char* fmt, ...) CLOVER_PRINTF(1, 2);
#define LOGC(cat, ...) do { if (g_log_mask & (cat)) log_raw(__VA_ARGS__); } while (0)
#define LOGI(...) log_raw(__VA_ARGS__)
[[noreturn]] void fatal(const char* fmt, ...) CLOVER_PRINTF(1, 2);
extern void (*g_fatal_hook)(const char* msg);  // e.g. show an error dialog before exiting
// Immediate process exit without library teardown (guest threads may still be blocked; on Windows
// ExitProcess would run DLL detach code, e.g. of the GL driver, that can hang on them).
[[noreturn]] void hard_exit(int code);

// ---------------------------------------------------------------- Linux ABI constants (ARM EABI)
namespace lnx {
// errno
enum {
    EPERM_ = 1, ENOENT_ = 2, ESRCH_ = 3, EINTR_ = 4, EIO_ = 5, ENXIO_ = 6, E2BIG_ = 7, ENOEXEC_ = 8, EBADF_ = 9,
    ECHILD_ = 10, EAGAIN_ = 11, ENOMEM_ = 12, EACCES_ = 13, EFAULT_ = 14, EBUSY_ = 16, EEXIST_ = 17, EXDEV_ = 18,
    ENODEV_ = 19, ENOTDIR_ = 20, EISDIR_ = 21, EINVAL_ = 22, ENFILE_ = 23, EMFILE_ = 24, ENOTTY_ = 25,
    EFBIG_ = 27, ENOSPC_ = 28, ESPIPE_ = 29, EROFS_ = 30, EMLINK_ = 31, EPIPE_ = 32, ERANGE_ = 34,
    EDEADLK_ = 35, ENAMETOOLONG_ = 36, ENOSYS_ = 38, ENOTEMPTY_ = 39, ELOOP_ = 40, ENODATA_ = 61,
    ETIME_ = 62, EOVERFLOW_ = 75, ENOTSOCK_ = 88, EDESTADDRREQ_ = 89, EMSGSIZE_ = 90, EPROTOTYPE_ = 91,
    ENOPROTOOPT_ = 92, EPROTONOSUPPORT_ = 93, EOPNOTSUPP_ = 95, EAFNOSUPPORT_ = 97, EADDRINUSE_ = 98,
    EADDRNOTAVAIL_ = 99, ENETDOWN_ = 100, ENETUNREACH_ = 101, ECONNABORTED_ = 103, ECONNRESET_ = 104,
    ENOBUFS_ = 105, EISCONN_ = 106, ENOTCONN_ = 107, ETIMEDOUT_ = 110, ECONNREFUSED_ = 111,
    EALREADY_ = 114, EINPROGRESS_ = 115,
};
// open flags
enum : u32 {
    L_O_ACCMODE = 3, L_O_CREAT = 0100, L_O_EXCL = 0200, L_O_NOCTTY = 0400, L_O_TRUNC = 01000, L_O_APPEND = 02000,
    L_O_NONBLOCK = 04000, L_O_DSYNC = 010000, L_O_DIRECTORY = 040000, L_O_NOFOLLOW = 0100000, L_O_LARGEFILE = 0400000,
    L_O_CLOEXEC = 02000000, L_O_PATH = 010000000,
};
// signals
enum {
    L_SIGHUP = 1, L_SIGINT = 2, L_SIGQUIT = 3, L_SIGILL = 4, L_SIGTRAP = 5, L_SIGABRT = 6, L_SIGBUS = 7, L_SIGFPE = 8,
    L_SIGKILL = 9, L_SIGUSR1 = 10, L_SIGSEGV = 11, L_SIGUSR2 = 12, L_SIGPIPE = 13, L_SIGALRM = 14, L_SIGTERM = 15,
    L_SIGSTKFLT = 16, L_SIGCHLD = 17, L_SIGCONT = 18, L_SIGSTOP = 19, L_SIGTSTP = 20, L_SIGTTIN = 21, L_SIGTTOU = 22,
    L_SIGURG = 23, L_SIGXCPU = 24, L_SIGXFSZ = 25, L_SIGVTALRM = 26, L_SIGPROF = 27, L_SIGWINCH = 28, L_SIGIO = 29,
    L_SIGPWR = 30, L_SIGSYS = 31, L_NSIG = 64,
};
enum : u32 {
    L_SA_NOCLDSTOP = 1, L_SA_NOCLDWAIT = 2, L_SA_SIGINFO = 4, L_SA_RESTORER = 0x04000000, L_SA_ONSTACK = 0x08000000,
    L_SA_RESTART = 0x10000000, L_SA_NODEFER = 0x40000000, L_SA_RESETHAND = 0x80000000,
};
// mmap
enum : u32 {
    L_PROT_READ = 1, L_PROT_WRITE = 2, L_PROT_EXEC = 4,
    L_MAP_SHARED = 1, L_MAP_PRIVATE = 2, L_MAP_FIXED = 0x10, L_MAP_ANONYMOUS = 0x20,
};
// clone
enum : u32 {
    L_CLONE_VM = 0x100, L_CLONE_FS = 0x200, L_CLONE_FILES = 0x400, L_CLONE_SIGHAND = 0x800, L_CLONE_PTRACE = 0x2000,
    L_CLONE_VFORK = 0x4000, L_CLONE_PARENT = 0x8000, L_CLONE_THREAD = 0x10000, L_CLONE_NEWNS = 0x20000,
    L_CLONE_SYSVSEM = 0x40000, L_CLONE_SETTLS = 0x80000, L_CLONE_PARENT_SETTID = 0x100000,
    L_CLONE_CHILD_CLEARTID = 0x200000, L_CLONE_DETACHED = 0x400000, L_CLONE_CHILD_SETTID = 0x1000000,
};
// file types
enum : u32 {
    L_S_IFMT = 0170000, L_S_IFSOCK = 0140000, L_S_IFLNK = 0120000, L_S_IFREG = 0100000, L_S_IFBLK = 060000,
    L_S_IFDIR = 040000, L_S_IFCHR = 020000, L_S_IFIFO = 010000,
};
enum { L_AT_FDCWD = -100, L_AT_SYMLINK_NOFOLLOW = 0x100, L_AT_REMOVEDIR = 0x200, L_AT_SYMLINK_FOLLOW = 0x400, L_AT_EMPTY_PATH = 0x1000 };
}  // namespace lnx

int host_errno_to_linux(int e);

// Guest-memory helpers are in memory.h
