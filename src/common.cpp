#include "common.h"
#include <cerrno>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include "winutil.h"
#endif

u32 g_log_mask = 0;
static std::mutex g_log_mtx;

void log_raw(const char* fmt, ...) {
    std::lock_guard lk(g_log_mtx);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void (*g_fatal_hook)(const char* msg) = nullptr;

void fatal(const char* fmt, ...) {
    char msg[2048];
    {
        std::lock_guard lk(g_log_mtx);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        fprintf(stderr, "FATAL: %s\n", msg);
        fflush(stderr);
    }
    if (g_fatal_hook) g_fatal_hook(msg);
    hard_exit(1);
}

void hard_exit(int code) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (unsigned)code);
#endif
    _Exit(code);
}

#ifdef EPERM
#define CLOVER_ERRNO_CASE_EPERM(n) case EPERM: return n;
#else
#define CLOVER_ERRNO_CASE_EPERM(n)
#endif
#ifdef ENOENT
#define CLOVER_ERRNO_CASE_ENOENT(n) case ENOENT: return n;
#else
#define CLOVER_ERRNO_CASE_ENOENT(n)
#endif
#ifdef ESRCH
#define CLOVER_ERRNO_CASE_ESRCH(n) case ESRCH: return n;
#else
#define CLOVER_ERRNO_CASE_ESRCH(n)
#endif
#ifdef EINTR
#define CLOVER_ERRNO_CASE_EINTR(n) case EINTR: return n;
#else
#define CLOVER_ERRNO_CASE_EINTR(n)
#endif
#ifdef EIO
#define CLOVER_ERRNO_CASE_EIO(n) case EIO: return n;
#else
#define CLOVER_ERRNO_CASE_EIO(n)
#endif
#ifdef ENXIO
#define CLOVER_ERRNO_CASE_ENXIO(n) case ENXIO: return n;
#else
#define CLOVER_ERRNO_CASE_ENXIO(n)
#endif
#ifdef E2BIG
#define CLOVER_ERRNO_CASE_E2BIG(n) case E2BIG: return n;
#else
#define CLOVER_ERRNO_CASE_E2BIG(n)
#endif
#ifdef ENOEXEC
#define CLOVER_ERRNO_CASE_ENOEXEC(n) case ENOEXEC: return n;
#else
#define CLOVER_ERRNO_CASE_ENOEXEC(n)
#endif
#ifdef EBADF
#define CLOVER_ERRNO_CASE_EBADF(n) case EBADF: return n;
#else
#define CLOVER_ERRNO_CASE_EBADF(n)
#endif
#ifdef ECHILD
#define CLOVER_ERRNO_CASE_ECHILD(n) case ECHILD: return n;
#else
#define CLOVER_ERRNO_CASE_ECHILD(n)
#endif
#ifdef EAGAIN
#define CLOVER_ERRNO_CASE_EAGAIN(n) case EAGAIN: return n;
#else
#define CLOVER_ERRNO_CASE_EAGAIN(n)
#endif
#ifdef ENOMEM
#define CLOVER_ERRNO_CASE_ENOMEM(n) case ENOMEM: return n;
#else
#define CLOVER_ERRNO_CASE_ENOMEM(n)
#endif
#ifdef EACCES
#define CLOVER_ERRNO_CASE_EACCES(n) case EACCES: return n;
#else
#define CLOVER_ERRNO_CASE_EACCES(n)
#endif
#ifdef EFAULT
#define CLOVER_ERRNO_CASE_EFAULT(n) case EFAULT: return n;
#else
#define CLOVER_ERRNO_CASE_EFAULT(n)
#endif
#ifdef EBUSY
#define CLOVER_ERRNO_CASE_EBUSY(n) case EBUSY: return n;
#else
#define CLOVER_ERRNO_CASE_EBUSY(n)
#endif
#ifdef EEXIST
#define CLOVER_ERRNO_CASE_EEXIST(n) case EEXIST: return n;
#else
#define CLOVER_ERRNO_CASE_EEXIST(n)
#endif
#ifdef EXDEV
#define CLOVER_ERRNO_CASE_EXDEV(n) case EXDEV: return n;
#else
#define CLOVER_ERRNO_CASE_EXDEV(n)
#endif
#ifdef ENODEV
#define CLOVER_ERRNO_CASE_ENODEV(n) case ENODEV: return n;
#else
#define CLOVER_ERRNO_CASE_ENODEV(n)
#endif
#ifdef ENOTDIR
#define CLOVER_ERRNO_CASE_ENOTDIR(n) case ENOTDIR: return n;
#else
#define CLOVER_ERRNO_CASE_ENOTDIR(n)
#endif
#ifdef EISDIR
#define CLOVER_ERRNO_CASE_EISDIR(n) case EISDIR: return n;
#else
#define CLOVER_ERRNO_CASE_EISDIR(n)
#endif
#ifdef EINVAL
#define CLOVER_ERRNO_CASE_EINVAL(n) case EINVAL: return n;
#else
#define CLOVER_ERRNO_CASE_EINVAL(n)
#endif
#ifdef ENFILE
#define CLOVER_ERRNO_CASE_ENFILE(n) case ENFILE: return n;
#else
#define CLOVER_ERRNO_CASE_ENFILE(n)
#endif
#ifdef EMFILE
#define CLOVER_ERRNO_CASE_EMFILE(n) case EMFILE: return n;
#else
#define CLOVER_ERRNO_CASE_EMFILE(n)
#endif
#ifdef ENOTTY
#define CLOVER_ERRNO_CASE_ENOTTY(n) case ENOTTY: return n;
#else
#define CLOVER_ERRNO_CASE_ENOTTY(n)
#endif
#ifdef ETXTBSY
#define CLOVER_ERRNO_CASE_ETXTBSY(n) case ETXTBSY: return n;
#else
#define CLOVER_ERRNO_CASE_ETXTBSY(n)
#endif
#ifdef EFBIG
#define CLOVER_ERRNO_CASE_EFBIG(n) case EFBIG: return n;
#else
#define CLOVER_ERRNO_CASE_EFBIG(n)
#endif
#ifdef ENOSPC
#define CLOVER_ERRNO_CASE_ENOSPC(n) case ENOSPC: return n;
#else
#define CLOVER_ERRNO_CASE_ENOSPC(n)
#endif
#ifdef ESPIPE
#define CLOVER_ERRNO_CASE_ESPIPE(n) case ESPIPE: return n;
#else
#define CLOVER_ERRNO_CASE_ESPIPE(n)
#endif
#ifdef EROFS
#define CLOVER_ERRNO_CASE_EROFS(n) case EROFS: return n;
#else
#define CLOVER_ERRNO_CASE_EROFS(n)
#endif
#ifdef EMLINK
#define CLOVER_ERRNO_CASE_EMLINK(n) case EMLINK: return n;
#else
#define CLOVER_ERRNO_CASE_EMLINK(n)
#endif
#ifdef EPIPE
#define CLOVER_ERRNO_CASE_EPIPE(n) case EPIPE: return n;
#else
#define CLOVER_ERRNO_CASE_EPIPE(n)
#endif
#ifdef EDOM
#define CLOVER_ERRNO_CASE_EDOM(n) case EDOM: return n;
#else
#define CLOVER_ERRNO_CASE_EDOM(n)
#endif
#ifdef ERANGE
#define CLOVER_ERRNO_CASE_ERANGE(n) case ERANGE: return n;
#else
#define CLOVER_ERRNO_CASE_ERANGE(n)
#endif
#ifdef EDEADLK
#define CLOVER_ERRNO_CASE_EDEADLK(n) case EDEADLK: return n;
#else
#define CLOVER_ERRNO_CASE_EDEADLK(n)
#endif
#ifdef ENAMETOOLONG
#define CLOVER_ERRNO_CASE_ENAMETOOLONG(n) case ENAMETOOLONG: return n;
#else
#define CLOVER_ERRNO_CASE_ENAMETOOLONG(n)
#endif
#ifdef ENOLCK
#define CLOVER_ERRNO_CASE_ENOLCK(n) case ENOLCK: return n;
#else
#define CLOVER_ERRNO_CASE_ENOLCK(n)
#endif
#ifdef ENOSYS
#define CLOVER_ERRNO_CASE_ENOSYS(n) case ENOSYS: return n;
#else
#define CLOVER_ERRNO_CASE_ENOSYS(n)
#endif
#ifdef ENOTEMPTY
#define CLOVER_ERRNO_CASE_ENOTEMPTY(n) case ENOTEMPTY: return n;
#else
#define CLOVER_ERRNO_CASE_ENOTEMPTY(n)
#endif
#ifdef ELOOP
#define CLOVER_ERRNO_CASE_ELOOP(n) case ELOOP: return n;
#else
#define CLOVER_ERRNO_CASE_ELOOP(n)
#endif
#ifdef ENOMSG
#define CLOVER_ERRNO_CASE_ENOMSG(n) case ENOMSG: return n;
#else
#define CLOVER_ERRNO_CASE_ENOMSG(n)
#endif
#ifdef ENODATA
#define CLOVER_ERRNO_CASE_ENODATA(n) case ENODATA: return n;
#else
#define CLOVER_ERRNO_CASE_ENODATA(n)
#endif
#ifdef ETIME
#define CLOVER_ERRNO_CASE_ETIME(n) case ETIME: return n;
#else
#define CLOVER_ERRNO_CASE_ETIME(n)
#endif
#ifdef EOVERFLOW
#define CLOVER_ERRNO_CASE_EOVERFLOW(n) case EOVERFLOW: return n;
#else
#define CLOVER_ERRNO_CASE_EOVERFLOW(n)
#endif
#ifdef EILSEQ
#define CLOVER_ERRNO_CASE_EILSEQ(n) case EILSEQ: return n;
#else
#define CLOVER_ERRNO_CASE_EILSEQ(n)
#endif
#ifdef ENOTSOCK
#define CLOVER_ERRNO_CASE_ENOTSOCK(n) case ENOTSOCK: return n;
#else
#define CLOVER_ERRNO_CASE_ENOTSOCK(n)
#endif
#ifdef EDESTADDRREQ
#define CLOVER_ERRNO_CASE_EDESTADDRREQ(n) case EDESTADDRREQ: return n;
#else
#define CLOVER_ERRNO_CASE_EDESTADDRREQ(n)
#endif
#ifdef EMSGSIZE
#define CLOVER_ERRNO_CASE_EMSGSIZE(n) case EMSGSIZE: return n;
#else
#define CLOVER_ERRNO_CASE_EMSGSIZE(n)
#endif
#ifdef EPROTOTYPE
#define CLOVER_ERRNO_CASE_EPROTOTYPE(n) case EPROTOTYPE: return n;
#else
#define CLOVER_ERRNO_CASE_EPROTOTYPE(n)
#endif
#ifdef ENOPROTOOPT
#define CLOVER_ERRNO_CASE_ENOPROTOOPT(n) case ENOPROTOOPT: return n;
#else
#define CLOVER_ERRNO_CASE_ENOPROTOOPT(n)
#endif
#ifdef EPROTONOSUPPORT
#define CLOVER_ERRNO_CASE_EPROTONOSUPPORT(n) case EPROTONOSUPPORT: return n;
#else
#define CLOVER_ERRNO_CASE_EPROTONOSUPPORT(n)
#endif
#ifdef ESOCKTNOSUPPORT
#define CLOVER_ERRNO_CASE_ESOCKTNOSUPPORT(n) case ESOCKTNOSUPPORT: return n;
#else
#define CLOVER_ERRNO_CASE_ESOCKTNOSUPPORT(n)
#endif
#ifdef EOPNOTSUPP
#define CLOVER_ERRNO_CASE_EOPNOTSUPP(n) case EOPNOTSUPP: return n;
#else
#define CLOVER_ERRNO_CASE_EOPNOTSUPP(n)
#endif
#ifdef EPFNOSUPPORT
#define CLOVER_ERRNO_CASE_EPFNOSUPPORT(n) case EPFNOSUPPORT: return n;
#else
#define CLOVER_ERRNO_CASE_EPFNOSUPPORT(n)
#endif
#ifdef EAFNOSUPPORT
#define CLOVER_ERRNO_CASE_EAFNOSUPPORT(n) case EAFNOSUPPORT: return n;
#else
#define CLOVER_ERRNO_CASE_EAFNOSUPPORT(n)
#endif
#ifdef EADDRINUSE
#define CLOVER_ERRNO_CASE_EADDRINUSE(n) case EADDRINUSE: return n;
#else
#define CLOVER_ERRNO_CASE_EADDRINUSE(n)
#endif
#ifdef EADDRNOTAVAIL
#define CLOVER_ERRNO_CASE_EADDRNOTAVAIL(n) case EADDRNOTAVAIL: return n;
#else
#define CLOVER_ERRNO_CASE_EADDRNOTAVAIL(n)
#endif
#ifdef ENETDOWN
#define CLOVER_ERRNO_CASE_ENETDOWN(n) case ENETDOWN: return n;
#else
#define CLOVER_ERRNO_CASE_ENETDOWN(n)
#endif
#ifdef ENETUNREACH
#define CLOVER_ERRNO_CASE_ENETUNREACH(n) case ENETUNREACH: return n;
#else
#define CLOVER_ERRNO_CASE_ENETUNREACH(n)
#endif
#ifdef ENETRESET
#define CLOVER_ERRNO_CASE_ENETRESET(n) case ENETRESET: return n;
#else
#define CLOVER_ERRNO_CASE_ENETRESET(n)
#endif
#ifdef ECONNABORTED
#define CLOVER_ERRNO_CASE_ECONNABORTED(n) case ECONNABORTED: return n;
#else
#define CLOVER_ERRNO_CASE_ECONNABORTED(n)
#endif
#ifdef ECONNRESET
#define CLOVER_ERRNO_CASE_ECONNRESET(n) case ECONNRESET: return n;
#else
#define CLOVER_ERRNO_CASE_ECONNRESET(n)
#endif
#ifdef ENOBUFS
#define CLOVER_ERRNO_CASE_ENOBUFS(n) case ENOBUFS: return n;
#else
#define CLOVER_ERRNO_CASE_ENOBUFS(n)
#endif
#ifdef EISCONN
#define CLOVER_ERRNO_CASE_EISCONN(n) case EISCONN: return n;
#else
#define CLOVER_ERRNO_CASE_EISCONN(n)
#endif
#ifdef ENOTCONN
#define CLOVER_ERRNO_CASE_ENOTCONN(n) case ENOTCONN: return n;
#else
#define CLOVER_ERRNO_CASE_ENOTCONN(n)
#endif
#ifdef ESHUTDOWN
#define CLOVER_ERRNO_CASE_ESHUTDOWN(n) case ESHUTDOWN: return n;
#else
#define CLOVER_ERRNO_CASE_ESHUTDOWN(n)
#endif
#ifdef ETIMEDOUT
#define CLOVER_ERRNO_CASE_ETIMEDOUT(n) case ETIMEDOUT: return n;
#else
#define CLOVER_ERRNO_CASE_ETIMEDOUT(n)
#endif
#ifdef ECONNREFUSED
#define CLOVER_ERRNO_CASE_ECONNREFUSED(n) case ECONNREFUSED: return n;
#else
#define CLOVER_ERRNO_CASE_ECONNREFUSED(n)
#endif
#ifdef EHOSTDOWN
#define CLOVER_ERRNO_CASE_EHOSTDOWN(n) case EHOSTDOWN: return n;
#else
#define CLOVER_ERRNO_CASE_EHOSTDOWN(n)
#endif
#ifdef EHOSTUNREACH
#define CLOVER_ERRNO_CASE_EHOSTUNREACH(n) case EHOSTUNREACH: return n;
#else
#define CLOVER_ERRNO_CASE_EHOSTUNREACH(n)
#endif
#ifdef EALREADY
#define CLOVER_ERRNO_CASE_EALREADY(n) case EALREADY: return n;
#else
#define CLOVER_ERRNO_CASE_EALREADY(n)
#endif
#ifdef EINPROGRESS
#define CLOVER_ERRNO_CASE_EINPROGRESS(n) case EINPROGRESS: return n;
#else
#define CLOVER_ERRNO_CASE_EINPROGRESS(n)
#endif
#ifdef ESTALE
#define CLOVER_ERRNO_CASE_ESTALE(n) case ESTALE: return n;
#else
#define CLOVER_ERRNO_CASE_ESTALE(n)
#endif
#ifdef EDQUOT
#define CLOVER_ERRNO_CASE_EDQUOT(n) case EDQUOT: return n;
#else
#define CLOVER_ERRNO_CASE_EDQUOT(n)
#endif
#ifdef ECANCELED
#define CLOVER_ERRNO_CASE_ECANCELED(n) case ECANCELED: return n;
#else
#define CLOVER_ERRNO_CASE_ECANCELED(n)
#endif

int host_errno_to_linux(int e) {
    switch (e) {
// errno names differ between hosts (MSVC lacks some); only map the ones this host defines
#define C(name, n) CLOVER_ERRNO_CASE_##name(n)
    C(EPERM, 1) C(ENOENT, 2) C(ESRCH, 3) C(EINTR, 4) C(EIO, 5)
    C(ENXIO, 6) C(E2BIG, 7) C(ENOEXEC, 8) C(EBADF, 9) C(ECHILD, 10)
    C(EAGAIN, 11) C(ENOMEM, 12) C(EACCES, 13) C(EFAULT, 14)
    C(EBUSY, 16) C(EEXIST, 17) C(EXDEV, 18) C(ENODEV, 19)
    C(ENOTDIR, 20) C(EISDIR, 21) C(EINVAL, 22) C(ENFILE, 23)
    C(EMFILE, 24) C(ENOTTY, 25) C(ETXTBSY, 26) C(EFBIG, 27)
    C(ENOSPC, 28) C(ESPIPE, 29) C(EROFS, 30) C(EMLINK, 31)
    C(EPIPE, 32) C(EDOM, 33) C(ERANGE, 34) C(EDEADLK, 35)
    C(ENAMETOOLONG, 36) C(ENOLCK, 37) C(ENOSYS, 38) C(ENOTEMPTY, 39)
    C(ELOOP, 40) C(ENOMSG, 42) C(ENODATA, 61) C(ETIME, 62)
    C(EOVERFLOW, 75) C(EILSEQ, 84) C(ENOTSOCK, 88) C(EDESTADDRREQ, 89)
    C(EMSGSIZE, 90) C(EPROTOTYPE, 91) C(ENOPROTOOPT, 92)
    C(EPROTONOSUPPORT, 93) C(ESOCKTNOSUPPORT, 94) C(EOPNOTSUPP, 95)
    C(EPFNOSUPPORT, 96) C(EAFNOSUPPORT, 97) C(EADDRINUSE, 98)
    C(EADDRNOTAVAIL, 99) C(ENETDOWN, 100) C(ENETUNREACH, 101)
    C(ENETRESET, 102) C(ECONNABORTED, 103) C(ECONNRESET, 104)
    C(ENOBUFS, 105) C(EISCONN, 106) C(ENOTCONN, 107) C(ESHUTDOWN, 108)
    C(ETIMEDOUT, 110) C(ECONNREFUSED, 111) C(EHOSTDOWN, 112)
    C(EHOSTUNREACH, 113) C(EALREADY, 114) C(EINPROGRESS, 115)
    C(ESTALE, 116) C(EDQUOT, 122) C(ECANCELED, 125)
    default: return 5;
    }
}
