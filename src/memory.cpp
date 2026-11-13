#include "memory.h"

// ================================================================ host memory primitives
#ifdef _WIN32
#include "winutil.h"

// The guest arena is one 4 GiB (+64 KiB slop) reservation per process. Like the macOS
// MAP_NORESERVE mapping, every guest address must read as zero until touched, so pages are
// committed lazily by a vectored exception handler on first access. The SHARED_WIN window stays a
// placeholder so that shared memory (the framebuffer) can be mapped there with MapViewOfFile3.
namespace {
constexpr u64 ARENA_SIZE = GUEST_SPACE + 0x10000;
constexpr u64 CHUNK = 0x10000;
constexpr int MAX_ARENAS = 512;
std::atomic<u8*> g_arenas[MAX_ARENAS];

using VirtualAlloc2_t = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3_t = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2_t = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);
VirtualAlloc2_t pVirtualAlloc2;
MapViewOfFile3_t pMapViewOfFile3;
UnmapViewOfFile2_t pUnmapViewOfFile2;

LONG CALLBACK guest_fault_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    u8* addr = (u8*)ep->ExceptionRecord->ExceptionInformation[1];
    for (auto& slot : g_arenas) {
        u8* base = slot.load(std::memory_order_acquire);
        if (!base || addr < base || addr >= base + ARENA_SIZE) continue;
        u64 off = (u64)(addr - base);
        if (off >= SHARED_WIN && off < (u64)SHARED_WIN + SHARED_WIN_SIZE) return EXCEPTION_CONTINUE_SEARCH;
        u64 c = off & ~(CHUNK - 1);
        u64 ce = std::min<u64>(c + CHUNK, ARENA_SIZE);
        if (VirtualAlloc(base + c, (SIZE_T)(ce - c), MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
        u64 pg = off & ~0xfffull;
        if (VirtualAlloc(base + pg, 0x1000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void win_mem_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (kb) {
            pVirtualAlloc2 = (VirtualAlloc2_t)GetProcAddress(kb, "VirtualAlloc2");
            pMapViewOfFile3 = (MapViewOfFile3_t)GetProcAddress(kb, "MapViewOfFile3");
            pUnmapViewOfFile2 = (UnmapViewOfFile2_t)GetProcAddress(kb, "UnmapViewOfFile2");
        }
        if (!pVirtualAlloc2 || !pMapViewOfFile3 || !pUnmapViewOfFile2)
            fatal("Windows 10 version 1803 or later is required (VirtualAlloc2/MapViewOfFile3 not found)");
        AddVectoredExceptionHandler(1, guest_fault_handler);
    });
}

// Release (decommit) whole pages in [a, e) of the arena, skipping the shared window.
void decommit(u8* base, u64 a, u64 e) {
    auto one = [&](u64 x, u64 y) {
        if (x < y) VirtualFree(base + x, (SIZE_T)(y - x), MEM_DECOMMIT);
    };
    u64 w0 = SHARED_WIN, w1 = (u64)SHARED_WIN + SHARED_WIN_SIZE;
    one(a, std::min(e, w0));
    one(std::max(a, w1), e);
}
}  // namespace

u32 host_page_size() { return 4096; }

AddressSpace::AddressSpace() {
    win_mem_init();
    u8* p = (u8*)pVirtualAlloc2(GetCurrentProcess(), nullptr, (SIZE_T)ARENA_SIZE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                PAGE_NOACCESS, nullptr, 0);
    if (!p) fatal("cannot reserve guest address space (error %lu)", GetLastError());
    // split: [0, WIN) | [WIN, WIN+SIZE) | [WIN+SIZE, end)
    if (!VirtualFree(p + SHARED_WIN, SHARED_WIN_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        fatal("cannot split guest address space (error %lu)", GetLastError());
    u64 hi = (u64)SHARED_WIN + SHARED_WIN_SIZE;
    if (!pVirtualAlloc2(GetCurrentProcess(), p, SHARED_WIN, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) ||
        !pVirtualAlloc2(GetCurrentProcess(), p + hi, (SIZE_T)(ARENA_SIZE - hi), MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                        PAGE_READWRITE, nullptr, 0))
        fatal("cannot reserve guest address space (error %lu)", GetLastError());
    base_ = p;
    for (auto& slot : g_arenas) {
        u8* expected = nullptr;
        if (slot.compare_exchange_strong(expected, p)) return;
    }
    fatal("too many guest address spaces");
}

AddressSpace::~AddressSpace() {
    if (!base_) return;
    for (auto& slot : g_arenas) {
        u8* expected = base_;
        if (slot.compare_exchange_strong(expected, nullptr)) break;
    }
    if (view_addr_) pUnmapViewOfFile2(GetCurrentProcess(), base_ + view_addr_, MEM_PRESERVE_PLACEHOLDER);
    u64 hi = (u64)SHARED_WIN + SHARED_WIN_SIZE;
    VirtualFree(base_, 0, MEM_RELEASE);
    VirtualFree(base_ + hi, 0, MEM_RELEASE);
    // window placeholders (possibly split): coalesce, then release
    VirtualFree(base_ + SHARED_WIN, SHARED_WIN_SIZE, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    VirtualFree(base_ + SHARED_WIN, 0, MEM_RELEASE);
}

bool AddressSpace::host_map_shared(u32 addr, u64 len, intptr_t shm, u64 off) {
    len = (len + CHUNK - 1) & ~(CHUNK - 1);
    if ((addr & (CHUNK - 1)) || (off & (CHUNK - 1))) return false;
    if (addr < SHARED_WIN || (u64)addr + len > (u64)SHARED_WIN + SHARED_WIN_SIZE) return false;
    HANDLE proc = GetCurrentProcess();
    if (view_addr_) {
        pUnmapViewOfFile2(proc, base_ + view_addr_, MEM_PRESERVE_PLACEHOLDER);
        view_addr_ = 0;
    }
    VirtualFree(base_ + SHARED_WIN, SHARED_WIN_SIZE, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    if (len < SHARED_WIN_SIZE && !VirtualFree(base_ + addr, (SIZE_T)len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return false;
    void* v = pMapViewOfFile3((HANDLE)shm, proc, base_ + addr, off, (SIZE_T)len, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!v) {
        LOGI("[mem] MapViewOfFile3 failed (error %lu)\n", GetLastError());
        return false;
    }
    view_addr_ = addr;
    return true;
}

void AddressSpace::host_unmap_shared(u32 addr, u64 len) {
    if (view_addr_ && view_addr_ >= addr && view_addr_ < addr + len) {
        pUnmapViewOfFile2(GetCurrentProcess(), base_ + view_addr_, MEM_PRESERVE_PLACEHOLDER);
        view_addr_ = 0;
    }
}

static void host_release(u8* base, u64 a, u64 e) { decommit(base, a, e); }

// fork: copy only the committed parts of a private region.
static void host_copy_private(u8* src, u8* dst, u64 a, u64 e) {
    u64 cur = a;
    while (cur < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(src + cur, &mi, sizeof mi)) break;
        u64 rend = std::min<u64>(e, (u64)((u8*)mi.BaseAddress + mi.RegionSize - src));
        if (mi.State == MEM_COMMIT && rend > cur) {
            if (VirtualAlloc(dst + cur, (SIZE_T)(rend - cur), MEM_COMMIT, PAGE_READWRITE)) memcpy(dst + cur, src + cur, rend - cur);
        }
        if (rend <= cur) break;
        cur = rend;
    }
}

#else  // POSIX
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

u32 host_page_size() {
    static u32 ps = (u32)getpagesize();
    return ps;
}

AddressSpace::AddressSpace() {
    // 4 GiB + 64 KiB slop so that unaligned/vector accesses at the very top never leave the arena.
    size_t sz = GUEST_SPACE + 0x10000;
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve guest address space");
    base_ = (u8*)p;
}

AddressSpace::~AddressSpace() {
    if (base_) munmap(base_, GUEST_SPACE + 0x10000);
}

bool AddressSpace::host_map_shared(u32 addr, u64 len, intptr_t shm, u64 off) {
    void* r = mmap(base_ + addr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, (int)shm, (off_t)off);
    return r != MAP_FAILED;
}

void AddressSpace::host_unmap_shared(u32 addr, u64 len) {
    // replace host shared mapping with private anonymous memory
    mmap(base_ + addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

// fresh anonymous pages for [a, e) (page aligned)
static void host_release(u8* base, u64 a, u64 e) {
    void* r = mmap(base + a, e - a, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) memset(base + a, 0, e - a);
}

static void host_copy_private(u8* src, u8* dst, u64 a, u64 e) {
#if defined(__APPLE__)
    kern_return_t kr = vm_copy(mach_task_self(), (vm_address_t)(src + a), (vm_size_t)(e - a), (vm_address_t)(dst + a));
    if (kr == KERN_SUCCESS) return;
#endif
    memcpy(dst + a, src + a, e - a);
}
#endif

// ================================================================ address space bookkeeping
std::string AddressSpace::read_str(u32 a, size_t max) const {
    std::string s;
    const char* p = (const char*)(base_ + a);
    size_t n = strnlen(p, max);
    s.assign(p, n);
    return s;
}

// Release host memory in [addr, addr+len) and make it read as zero.
void AddressSpace::zero(u32 addr, u32 len) {
    if (!len) return;
    u64 a = addr, e = (u64)addr + len;
    u64 hp = host_page_size();
    u64 fa = (a + hp - 1) & ~(hp - 1);
    u64 fe = e & ~(hp - 1);
    if (fa < fe) {
        host_release(base_, fa, fe);
        if (a < fa) memset(base_ + a, 0, fa - a);
        if (fe < e) memset(base_ + fe, 0, e - fe);
    } else {
        memset(base_ + a, 0, e - a);
    }
}

void AddressSpace::split_at(u32 addr) {
    auto it = regions.upper_bound(addr);
    if (it == regions.begin()) return;
    --it;
    if (it->first < addr && it->second.end > addr) {
        Region tail = it->second;
        it->second.end = addr;
        if (tail.shared) tail.shm_off += addr - it->first;
        regions[addr] = tail;
    }
}

void AddressSpace::erase_range(u32 addr, u32 end_) {
    u64 end = end_ ? end_ : GUEST_SPACE;
    split_at(addr);
    if (end < GUEST_SPACE) split_at((u32)end);
    auto it = regions.lower_bound(addr);
    while (it != regions.end() && it->first < end) {
        if (it->second.shared) host_unmap_shared(it->first, it->second.end - it->first);
        it = regions.erase(it);
    }
}

bool AddressSpace::is_free(u32 addr, u32 len) {
    u64 end = (u64)addr + len;
    auto it = regions.upper_bound(addr);
    if (it != regions.begin()) {
        auto p = std::prev(it);
        if (p->second.end > addr) return false;
    }
    if (it != regions.end() && it->first < end) return false;
    return true;
}

bool AddressSpace::has_exec(u32 addr, u32 len) {
    std::lock_guard lk(mtx);
    u64 end = (u64)addr + len;
    auto it = regions.upper_bound(addr);
    if (it != regions.begin()) --it;
    for (; it != regions.end() && it->first < end; ++it)
        if (it->second.end > addr && (it->second.prot & 4)) return true;
    return false;
}

bool AddressSpace::is_mapped(u32 addr, u32 len) {
    u64 cur = addr, end = (u64)addr + len;
    while (cur < end) {
        auto it = regions.upper_bound((u32)cur);
        if (it == regions.begin()) return false;
        --it;
        if (it->second.end <= cur) return false;
        cur = it->second.end;
    }
    return true;
}

u32 AddressSpace::find_free(u32 len, u32 align) {
    std::lock_guard lk(mtx);
    len = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    // Walk gaps top-down starting at MMAP_TOP.
    u64 top = MMAP_TOP;
    auto it = regions.lower_bound(MMAP_TOP);
    // Walk gaps in descending address order.
    while (true) {
        u64 gap_lo = 0x00100000;  // keep low memory free
        bool has_prev = it != regions.begin();
        auto prev = it;
        if (has_prev) {
            --prev;
            gap_lo = std::max<u64>(gap_lo, prev->second.end);
        }
        if (gap_lo < top && top - gap_lo >= len) {
            u64 cand = (top - len) & ~(u64)(align - 1);
            if (cand >= gap_lo) return (u32)cand;
        }
        if (!has_prev) break;
        top = std::min<u64>(top, prev->first);
        it = prev;
        if (top <= 0x00100000) break;
    }
    return 0;
}

void AddressSpace::map(u32 addr, u32 len, u32 prot, const std::string& name, bool zero_mem) {
    std::lock_guard lk(mtx);
    len = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    u64 end = (u64)addr + len;
    erase_range(addr, (u32)end);
    if (zero_mem) zero(addr, len);
    Region r;
    r.end = end;
    r.prot = prot;
    r.name = name;
    regions[addr] = r;
}

bool AddressSpace::map_shared(u32 addr, u32 len, intptr_t shm, u64 off, const std::string& name) {
    std::lock_guard lk(mtx);
    u32 hp = host_page_size();
    if ((addr & (hp - 1)) || (off & (hp - 1))) return false;
    u32 hlen = (len + hp - 1) & ~(hp - 1);
    erase_range(addr, addr + hlen);
    if (!host_map_shared(addr, hlen, shm, off)) return false;
    Region reg;
    reg.end = (u64)addr + hlen;
    reg.prot = 3;
    reg.shared = true;
    reg.shm = shm;
    reg.shm_off = off;
    reg.name = name;
    regions[addr] = reg;
    return true;
}

void AddressSpace::unmap(u32 addr, u32 len) {
    std::lock_guard lk(mtx);
    len = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    u64 end = (u64)addr + len;
    // Determine which parts were actually mapped and non-shared to zero them.
    erase_range(addr, (u32)end);
    zero(addr, len);
}

void AddressSpace::protect(u32 addr, u32 len, u32 prot) {
    std::lock_guard lk(mtx);
    len = (len + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    u64 end = (u64)addr + len;
    split_at(addr);
    if (end < GUEST_SPACE) split_at((u32)end);
    for (auto it = regions.lower_bound(addr); it != regions.end() && it->first < end; ++it) it->second.prot = prot;
}

void AddressSpace::copy_to(AddressSpace& dst) {
    std::lock_guard lk(mtx);
    u64 hp = host_page_size();
    for (auto& [start, r] : regions) {
        if (r.shared) {
            dst.regions[start] = r;
            continue;
        }
        u64 a = start & ~(hp - 1);
        u64 e = (r.end + hp - 1) & ~(hp - 1);
        host_copy_private(base_, dst.base_, a, e);
        dst.regions[start] = r;
    }
    // Shared regions last: neighbouring page-rounded copies must not clobber them.
    for (auto& [start, r] : dst.regions)
        if (r.shared) dst.host_map_shared(start, r.end - start, r.shm, r.shm_off);
    dst.brk_start = brk_start;
    dst.brk_cur = brk_cur;
}

std::string AddressSpace::maps_text() {
    std::lock_guard lk(mtx);
    std::string out;
    char line[512];
    for (auto& [start, r] : regions) {
        snprintf(line, sizeof line, "%08x-%08x %c%c%c%c %08x 00:00 0          %s\n", start, (u32)(r.end - 1) + 1,
                 (r.prot & 1) ? 'r' : '-', (r.prot & 2) ? 'w' : '-', (r.prot & 4) ? 'x' : '-', r.shared ? 's' : 'p', 0u,
                 r.name.c_str());
        out += line;
    }
    return out;
}
