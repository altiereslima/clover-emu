// Platform services for Windows.
#include "platform.h"
#include "winutil.h"
#include <timeapi.h>
#include <bcrypt.h>
#include <cstdio>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace plat {

namespace {
LARGE_INTEGER g_qpf;
}

void init() {
    QueryPerformanceFrequency(&g_qpf);
    // 1 ms scheduler tick: condition-variable timeouts and Sleep() are otherwise ~15.6 ms coarse.
    timeBeginPeriod(1);
    // Keep that timer resolution even when the window is minimised or occluded (Windows 11).
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION | PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    SetConsoleOutputCP(CP_UTF8);
}

uint64_t now_ns() {
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)g_qpf.QuadPart, v = (uint64_t)c.QuadPart;
    return (v / f) * 1000000000ull + (v % f) * 1000000000ull / f;
}

uint64_t realtime_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100ull;
}

void sleep_ns(uint64_t ns) {
    // High-resolution waitable timer for the bulk, then a short yield-spin for the last fraction:
    // the firmware paces its frames with nanosleep, so sub-millisecond accuracy matters.
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint64_t deadline = now_ns() + ns;
    if (ns > 1000000ull) {
        uint64_t coarse = ns - 500000ull;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(coarse / 100);
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep((DWORD)(coarse / 1000000ull));
        }
    }
    while (now_ns() < deadline) SwitchToThread() || (YieldProcessor(), 0);
}

void usleep_us(uint32_t us) {
    if (us >= 2000) Sleep(us / 1000);
    else sleep_ns((uint64_t)us * 1000ull);
}

void set_thread_name(const char* name) { SetThreadDescription(GetCurrentThread(), utf8_to_wide(name).c_str()); }

void set_thread_realtime() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }

std::string exe_path() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (!n || n >= buf.size()) return "";
    std::string s = wide_to_utf8(buf.data(), n);
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}

void random_bytes(void* buf, size_t n) {
    BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

int host_pid() { return (int)GetCurrentProcessId(); }

bool shm_create(Shm& out, size_t size) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32),
                                  (DWORD)(size & 0xffffffffu), nullptr);
    if (!h) return false;
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!p) {
        CloseHandle(h);
        return false;
    }
    out.handle = (intptr_t)h;
    out.mem = (uint8_t*)p;
    out.size = size;
    return true;
}

}  // namespace plat
