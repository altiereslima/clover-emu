// Host platform abstraction: time, threads, atomics on guest memory, shared memory, misc.
// Implemented by platform_posix.cpp (macOS/Linux) and platform_win.cpp (Windows).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>
#include <bit>

#if defined(_MSC_VER) && !defined(__clang__)
#define CLOVER_PRINTF(a, b)
#else
#define CLOVER_PRINTF(a, b) __attribute__((format(printf, a, b)))
#endif

// ---------------------------------------------------------------- atomics on raw guest memory
// The guest address space is plain host memory; these give sequentially consistent atomic access.
template <typename T> inline T atomic_load_raw(T* p) { return std::atomic_ref<T>(*p).load(std::memory_order_seq_cst); }
template <typename T> inline void atomic_store_raw(T* p, T v) { std::atomic_ref<T>(*p).store(v, std::memory_order_seq_cst); }
template <typename T> inline T atomic_exchange_raw(T* p, T v) { return std::atomic_ref<T>(*p).exchange(v, std::memory_order_seq_cst); }
template <typename T> inline T atomic_fetch_add_raw(T* p, T v) { return std::atomic_ref<T>(*p).fetch_add(v, std::memory_order_seq_cst); }
// Like __atomic_compare_exchange_n(strong): on failure *expected receives the current value.
template <typename T> inline bool atomic_cas_raw(T* p, T* expected, T desired) {
    return std::atomic_ref<T>(*p).compare_exchange_strong(*expected, desired, std::memory_order_seq_cst);
}

inline int ctz64(uint64_t v) { return std::countr_zero(v); }

// ---------------------------------------------------------------- platform services
namespace plat {
void init();                       // once, at startup (timer resolution, fault handler, console)
uint64_t now_ns();                 // monotonic
uint64_t realtime_ns();            // wall clock (Unix epoch)
void sleep_ns(uint64_t ns);        // precise short sleep (sub-millisecond accuracy where possible)
void usleep_us(uint32_t us);       // coarse sleep
void set_thread_name(const char* name);
void set_thread_realtime();        // raise the calling thread's scheduling priority / QoS
std::string exe_path();            // absolute path of the running executable ("" if unknown)
void random_bytes(void* buf, size_t n);
int host_pid();

// Anonymous shared memory object (e.g. the framebuffer): can be mapped into guest address spaces.
// `handle` is an fd on POSIX and a section HANDLE on Windows.
struct Shm {
    intptr_t handle = -1;
    uint8_t* mem = nullptr;  // host-side mapping
    size_t size = 0;
};
bool shm_create(Shm& out, size_t size);
}  // namespace plat
