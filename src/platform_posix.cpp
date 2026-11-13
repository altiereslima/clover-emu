// Platform services for macOS / Linux.
#include "platform.h"
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#include <mach-o/dyld.h>
#else
#include <sys/random.h>
#endif

namespace plat {

void init() { signal(SIGPIPE, SIG_IGN); }

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
uint64_t realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void sleep_ns(uint64_t ns) {
    timespec ts{(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    nanosleep(&ts, nullptr);
}
void usleep_us(uint32_t us) { usleep(us); }

void set_thread_name(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    char b[16];
    strncpy(b, name, 15);
    b[15] = 0;
    pthread_setname_np(pthread_self(), b);
#endif
}

void set_thread_realtime() {
#if defined(__APPLE__)
    // Highest QoS: minimal timer leeway and P-core scheduling (the guest is a real-time console).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

std::string exe_path() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) return buf;
    return "";
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
#endif
}

void random_bytes(void* buf, size_t n) {
#if defined(__APPLE__)
    arc4random_buf(buf, n);
#else
    size_t done = 0;
    while (done < n) {
        ssize_t r = getrandom((char*)buf + done, n - done, 0);
        if (r <= 0) break;
        done += (size_t)r;
    }
#endif
}

int host_pid() { return (int)getpid(); }

bool shm_create(Shm& out, size_t size) {
    const char* tmp = getenv("TMPDIR");
    std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/clover-shm.XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back(0);
    int fd = mkstemp(buf.data());
    if (fd < 0) return false;
    unlink(buf.data());
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return false; }
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return false; }
    out.handle = fd;
    out.mem = (uint8_t*)p;
    out.size = size;
    return true;
}

}  // namespace plat
