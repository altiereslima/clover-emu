#include "memory.h"
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

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
        // fresh anonymous pages
        void* r = mmap(base_ + fa, fe - fa, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED) memset(base_ + fa, 0, fe - fa);
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
        if (it->second.shared) {
            // replace host shared mapping with private anonymous memory
            u64 len = it->second.end - it->first;
            mmap(base_ + it->first, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        }
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

bool AddressSpace::map_shared(u32 addr, u32 len, int fd, u64 off, const std::string& name) {
    std::lock_guard lk(mtx);
    u32 hp = host_page_size();
    if ((addr & (hp - 1)) || (off & (hp - 1))) return false;
    u32 hlen = (len + hp - 1) & ~(hp - 1);
    erase_range(addr, addr + hlen);
    void* r = mmap(base_ + addr, hlen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)off);
    if (r == MAP_FAILED) return false;
    Region reg;
    reg.end = (u64)addr + hlen;
    reg.prot = 3;
    reg.shared = true;
    reg.shm_fd = fd;
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
            mmap(dst.base_ + start, r.end - start, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, r.shm_fd, (off_t)r.shm_off);
            continue;
        }
        u64 a = start & ~(hp - 1);
        u64 e = (r.end + hp - 1) & ~(hp - 1);
        kern_return_t kr = vm_copy(mach_task_self(), (vm_address_t)(base_ + a), (vm_size_t)(e - a), (vm_address_t)(dst.base_ + a));
        if (kr != KERN_SUCCESS) memcpy(dst.base_ + start, base_ + start, r.end - start);
        dst.regions[start] = r;
    }
    // Shared regions may have been clobbered by neighbouring vm_copy rounding; re-establish.
    for (auto& [start, r] : dst.regions)
        if (r.shared)
            mmap(dst.base_ + start, r.end - start, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, r.shm_fd, (off_t)r.shm_off);
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
