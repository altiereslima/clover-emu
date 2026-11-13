// Guest 32-bit address space backed by one 4 GiB host reservation.
#pragma once
#include "common.h"
#include <map>
#include <mutex>
#include <cstring>
#include <memory>

constexpr u32 GUEST_PAGE = 4096;
constexpr u64 GUEST_SPACE = 1ull << 32;
constexpr u32 STACK_TOP = 0xbf000000;
constexpr u32 STACK_SIZE = 8u << 20;
constexpr u32 MMAP_TOP = 0xb7000000;
constexpr u32 KUSER_PAGE = 0xffff0000;

struct Region {
    u64 end;
    u32 prot;
    bool shared = false;     // host-shared mapping (e.g. framebuffer)
    int shm_fd = -1;         // host fd for shared mapping
    u64 shm_off = 0;
    std::string name;
};

class AddressSpace {
public:
    AddressSpace();
    ~AddressSpace();
    AddressSpace(const AddressSpace&) = delete;

    u8* base() const { return base_; }
    u8* ptr(u32 addr) const { return base_ + addr; }

    // Region management. All addresses page-aligned (guest 4K).
    // find_free: top-down search for a free gap of len bytes below MMAP_TOP.
    u32 find_free(u32 len, u32 align = GUEST_PAGE);
    bool is_free(u32 addr, u32 len);
    void map(u32 addr, u32 len, u32 prot, const std::string& name, bool zero = true);
    // Map host-shared memory (fd) at addr. addr/len must be 16K aligned (host page).
    bool map_shared(u32 addr, u32 len, int fd, u64 off, const std::string& name);
    void unmap(u32 addr, u32 len);
    void protect(u32 addr, u32 len, u32 prot);
    void zero(u32 addr, u32 len);
    bool is_mapped(u32 addr, u32 len);
    bool has_exec(u32 addr, u32 len);  // any executable mapping in range

    // Copy all mappings into another (fresh) address space (fork).
    void copy_to(AddressSpace& dst);

    std::string maps_text();

    u32 brk_start = 0, brk_cur = 0;
    std::recursive_mutex mtx;
    std::map<u32, Region> regions;  // keyed by start

    // memory access helpers
    template <typename T> T rd(u32 a) const { T v; memcpy(&v, base_ + a, sizeof(T)); return v; }
    template <typename T> void wr(u32 a, T v) { memcpy(base_ + a, &v, sizeof(T)); }
    std::string read_str(u32 a, size_t max = 65536) const;
    void write_bytes(u32 a, const void* p, size_t n) { memcpy(base_ + a, p, n); }
    void read_bytes(u32 a, void* p, size_t n) const { memcpy(p, base_ + a, n); }

private:
    void split_at(u32 addr);
    void erase_range(u32 addr, u32 end);
    u8* base_ = nullptr;
};

u32 host_page_size();
