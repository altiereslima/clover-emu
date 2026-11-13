// HLE core: function registry, stub ELF shared object generation, dispatch, per-process state.
#include "hle.h"
#include "syscalls.h"
#include <algorithm>

namespace {
struct Entry {
    HleLib lib;
    std::string name;
    HleFn fn;
};
std::vector<Entry>& table() {
    static std::vector<Entry> t;
    return t;
}
bool g_sorted = false;
void ensure_sorted() {
    if (g_sorted) return;
    auto& t = table();
    std::sort(t.begin(), t.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    g_sorted = true;
}

u32 elf_hash(const char* name) {
    u32 h = 0, g;
    while (*name) {
        h = (h << 4) + (u8)*name++;
        if ((g = h & 0xf0000000)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Build a minimal ARM ELF shared object exporting `names`, each a stub "svc #(BASE+idx); bx lr".
std::vector<u8> build_so(const std::vector<std::pair<std::string, u32>>& syms, const std::string& soname) {
    // layout
    const u32 ehsz = 52, phsz = 32, nph = 2;
    u32 off = ehsz + phsz * nph;
    u32 nsyms = (u32)syms.size() + 1;
    // dynstr
    std::string dynstr(1, '\0');
    std::vector<u32> name_off;
    for (auto& s : syms) {
        name_off.push_back((u32)dynstr.size());
        dynstr += s.first;
        dynstr += '\0';
    }
    u32 soname_off = (u32)dynstr.size();
    dynstr += soname;
    dynstr += '\0';
    auto align = [](u32 v, u32 a) { return (v + a - 1) & ~(a - 1); };
    u32 dynsym_off = align(off, 4);
    u32 dynstr_off = dynsym_off + nsyms * 16;
    u32 hash_off = align(dynstr_off + (u32)dynstr.size(), 4);
    u32 nbucket = std::max<u32>(1, nsyms);
    u32 hash_size = (2 + nbucket + nsyms) * 4;
    u32 text_off = align(hash_off + hash_size, 16);
    u32 text_size = (u32)syms.size() * 8;
    u32 dyn_off = align(text_off + text_size, 8);
    u32 ndyn = 7;
    u32 total = dyn_off + ndyn * 8;
    std::vector<u8> out(align(total, 4096), 0);
    auto w32 = [&](u32 o, u32 v) { memcpy(&out[o], &v, 4); };
    auto w16 = [&](u32 o, u16 v) { memcpy(&out[o], &v, 2); };
    // ELF header
    const u8 ident[16] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
    memcpy(&out[0], ident, 16);
    w16(16, 3);       // ET_DYN
    w16(18, 40);      // EM_ARM
    w32(20, 1);
    w32(24, 0);       // entry
    w32(28, ehsz);    // phoff
    w32(32, 0);       // shoff
    w32(36, 0x05000400);  // EABI5, hard-float
    w16(40, ehsz);
    w16(42, phsz);
    w16(44, nph);
    w16(46, 40);
    w16(48, 0);
    w16(50, 0);
    // PT_LOAD
    u32 ph = ehsz;
    w32(ph + 0, 1);
    w32(ph + 4, 0);
    w32(ph + 8, 0);
    w32(ph + 12, 0);
    w32(ph + 16, (u32)out.size());
    w32(ph + 20, (u32)out.size());
    w32(ph + 24, 7);
    w32(ph + 28, 0x1000);
    // PT_DYNAMIC
    ph += phsz;
    w32(ph + 0, 2);
    w32(ph + 4, dyn_off);
    w32(ph + 8, dyn_off);
    w32(ph + 12, dyn_off);
    w32(ph + 16, ndyn * 8);
    w32(ph + 20, ndyn * 8);
    w32(ph + 24, 6);
    w32(ph + 28, 4);
    // dynsym
    for (u32 i = 0; i < syms.size(); i++) {
        u32 so = dynsym_off + (i + 1) * 16;
        w32(so + 0, name_off[i]);
        w32(so + 4, text_off + i * 8);
        w32(so + 8, 8);
        out[so + 12] = 0x12;  // GLOBAL FUNC
        out[so + 13] = 0;
        w16(so + 14, 5);      // any defined section index
    }
    memcpy(&out[dynstr_off], dynstr.data(), dynstr.size());
    // hash
    w32(hash_off, nbucket);
    w32(hash_off + 4, nsyms);
    std::vector<u32> bucket(nbucket, 0), chain(nsyms, 0);
    for (u32 i = 1; i < nsyms; i++) {
        u32 h = elf_hash(syms[i - 1].first.c_str()) % nbucket;
        chain[i] = bucket[h];
        bucket[h] = i;
    }
    for (u32 i = 0; i < nbucket; i++) w32(hash_off + 8 + i * 4, bucket[i]);
    for (u32 i = 0; i < nsyms; i++) w32(hash_off + 8 + nbucket * 4 + i * 4, chain[i]);
    // text
    for (u32 i = 0; i < syms.size(); i++) {
        w32(text_off + i * 8, 0xEF000000u | (SVC_HLE_BASE + syms[i].second));
        w32(text_off + i * 8 + 4, 0xE12FFF1Eu);
    }
    // dynamic
    u32 d = dyn_off;
    auto dyn = [&](u32 tag, u32 val) { w32(d, tag); w32(d + 4, val); d += 8; };
    dyn(4, hash_off);
    dyn(5, dynstr_off);
    dyn(6, dynsym_off);
    dyn(10, (u32)dynstr.size());
    dyn(11, 16);
    dyn(14, soname_off);
    dyn(0, 0);
    return out;
}
}  // namespace

HleRegister::HleRegister(HleLib lib, const char* name, HleFn fn) {
    table().push_back({lib, name, fn});
    g_sorted = false;
}

bool hle_dispatch(Thread& t, u32 idx) {
    ensure_sorted();
    auto& tb = table();
    if (idx >= tb.size()) return false;
    HleArgs a(t);
    if (g_log_mask & LOG_HLE) LOGI("[hle] %d/%d %s(%x, %x, %x, %x)\n", t.proc->pid, t.tid, tb[idx].name.c_str(), a.R[0], a.R[1], a.R[2], a.R[3]);
    tb[idx].fn(a);
    return true;
}

void hle_init(const std::string& rundir) {
    ensure_sorted();
    auto& tb = table();
    std::string dir = rundir + "/hle";
    hfs::mkdir(dir, 0755);
    struct LibOut {
        HleLib lib;
        const char* guest;
        const char* soname;
    };
    const LibOut libs[] = {
        {LIB_MALI, "/usr/lib/libMali.so", "libMali.so"},
        {LIB_SDL, "/usr/lib/libSDL2.so", "libSDL2.so"},
        {LIB_ASOUND, "/usr/lib/libasound.so.2.0.0", "libasound.so.2"},
    };
    for (auto& L : libs) {
        std::vector<std::pair<std::string, u32>> syms;
        for (u32 i = 0; i < tb.size(); i++)
            if (tb[i].lib == L.lib) syms.push_back({tb[i].name, i});
        auto so = build_so(syms, L.soname);
        std::string host = dir + "/" + L.soname;
        if (!hfs::write_file(host, so.data(), so.size(), 0755)) fatal("cannot write %s", host.c_str());
        vfs::add_override(L.guest, host);
        LOGC(LOG_HLE, "[hle] %s: %zu stub functions\n", L.guest, syms.size());
    }
}

// ---------------------------------------------------------------- per-process state
static std::mutex g_hle_mtx;

HleProcState& hle_state(Process& p) {
    std::lock_guard lk(g_hle_mtx);
    if (!p.hle) {
        p.hle = std::make_shared<HleProcState>();
        p.hle->proc = p.shared_from_this();
    }
    return *p.hle;
}

u32 HleProcState::alloc(u32 size) {
    std::lock_guard lk(mtx);
    size = (size + 15) & ~15u;
    if (!arena || arena_used + size > arena_size) {
        auto p = proc.lock();
        if (!p) return 0;
        u32 sz = std::max<u32>(1u << 20, (size + 0xfff) & ~0xfffu);
        arena = guest_mmap_anon(*p, sz, "[hle-arena]");
        arena_size = sz;
        arena_used = 0;
    }
    u32 r = arena + arena_used;
    arena_used += size;
    return r;
}

u32 HleProcState::alloc_str(const std::string& s) {
    u32 a = alloc((u32)s.size() + 1);
    auto p = proc.lock();
    if (p && a) p->mem->write_bytes(a, s.c_str(), s.size() + 1);
    return a;
}

u32 HleProcState::intern(const std::string& s) {
    {
        std::lock_guard lk(mtx);
        auto it = interned.find(s);
        if (it != interned.end()) return it->second;
    }
    u32 a = alloc_str(s);
    std::lock_guard lk(mtx);
    interned[s] = a;
    return a;
}

extern void hle_sdl_process_cleanup(Process& p, HleProcState& st);

void hle_process_exit(Process& p) {
    if (!p.hle) return;
    hle_sdl_process_cleanup(p, *p.hle);
    p.hle.reset();
}
void hle_process_exec(Process& p) {
    if (!p.hle) return;
    hle_sdl_process_cleanup(p, *p.hle);
    p.hle.reset();
}
void hle_process_fork(Process& parent, Process& child) {
    // Child starts with no HLE state; window/GL/audio belong to the parent.
    (void)parent;
    child.hle.reset();
}

// Look up an exported symbol of a shared library already mapped into the guest
// (e.g. pthread_create in libpthread), by reading the library's ELF dynamic symbol table.
u32 guest_symbol(Process& p, const char* lib_substr, const char* sym) {
    static std::mutex mtx;
    static std::map<std::pair<AddressSpace*, std::string>, u32> cache;
    std::string key = std::string(lib_substr) + ":" + sym;
    {
        std::lock_guard lk(mtx);
        auto it = cache.find({p.mem.get(), key});
        if (it != cache.end()) return it->second;
    }
    u32 base = 0;
    std::string path;
    {
        std::lock_guard lk(p.mem->mtx);
        for (auto& [start, r] : p.mem->regions)
            if (r.name.find(lib_substr) != std::string::npos && (path.empty() || start < base)) {
                base = start;
                path = r.name;
            }
    }
    if (path.empty()) return 0;
    std::vector<u8> d;
    if (!vfs::read_file(p, path, d) || d.size() < 52) return 0;
    auto rd32 = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    auto rd16 = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
    u32 shoff = rd32(32);
    u16 shentsize = rd16(46), shnum = rd16(48);
    u32 result = 0;
    for (u32 i = 0; i < shnum && !result; i++) {
        size_t sh = shoff + (size_t)i * shentsize;
        if (sh + 40 > d.size() || rd32(sh + 4) != 11) continue;  // SHT_DYNSYM
        u32 symoff = rd32(sh + 16), symsz = rd32(sh + 20), link = rd32(sh + 24);
        size_t strsh = shoff + (size_t)link * shentsize;
        u32 stroff = rd32(strsh + 16);
        for (u32 so = symoff; so + 16 <= symoff + symsz; so += 16) {
            u32 name = rd32(so), value = rd32(so + 4);
            u16 shndx = rd16(so + 14);
            if (!shndx || !value) continue;
            if (strcmp((const char*)&d[stroff + name], sym) == 0) {
                result = base + value;
                break;
            }
        }
    }
    std::lock_guard lk(mtx);
    cache[{p.mem.get(), key}] = result;
    return result;
}
