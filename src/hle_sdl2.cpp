// More libSDL2 HLE (used by the SNES Classic's canoe emulator): threads, semaphores, atomics,
// timing, audio helpers, WAV loading, surfaces and the 2D renderer.
#include "hle.h"
#include "hle_gl.h"
#include <SDL.h>

u32 guest_symbol(Process& p, const char* lib_substr, const char* sym);

// ---------------------------------------------------------------- threads
// SDL threads are real guest pthreads: call the guest's own pthread_create so glibc sets up TLS.
HLE(LIB_SDL, SDL_CreateThread) {
    Process& p = *a.t.proc;
    u32 fn = a.u(0), name = a.u(1), data = a.u(2);
    u32 pc = guest_symbol(p, "/libpthread", "pthread_create");
    if (!pc) {
        LOGI("[sdl] SDL_CreateThread: pthread_create not found\n");
        a.ret(0);
        return;
    }
    auto& st = hle_state(p);
    u32 th = st.alloc(16);
    memset(a.m.ptr(th), 0, 16);
    u32 r = guest_call(p.shared_from_this(), pc, {th, 0, fn, data}, a.t.tls);
    LOGC(LOG_HLE, "[sdl] CreateThread '%s' fn %08x -> %d\n", name ? a.m.read_str(name).c_str() : "", fn, r);
    a.ret(r == 0 ? th : 0);
}
HLE(LIB_SDL, SDL_WaitThread) {
    Process& p = *a.t.proc;
    u32 th = a.u(0), statusp = a.u(1);
    if (!th) return;
    u32 pj = guest_symbol(p, "/libpthread", "pthread_join");
    auto& st = hle_state(p);
    u32 retp = st.alloc(8);
    a.m.wr<u32>(retp, 0);
    if (pj) guest_call(p.shared_from_this(), pj, {a.m.rd<u32>(th), retp}, a.t.tls);
    if (statusp) a.m.wr<s32>(statusp, a.m.rd<s32>(retp));
}

// ---------------------------------------------------------------- semaphores (host side)
namespace {
struct Sem {
    std::mutex m;
    std::condition_variable cv;
    u32 count = 0;
};
std::mutex g_sem_mtx;
std::map<u32, std::shared_ptr<Sem>> g_sems;
u32 g_sem_next = 1;
std::shared_ptr<Sem> get_sem(HleArgs& a, u32 h) {
    if (!h) return nullptr;
    std::lock_guard lk(g_sem_mtx);
    auto it = g_sems.find(a.m.rd<u32>(h));
    return it == g_sems.end() ? nullptr : it->second;
}
}  // namespace

HLE(LIB_SDL, SDL_CreateSemaphore) {
    auto s = std::make_shared<Sem>();
    s->count = a.u(0);
    u32 id;
    {
        std::lock_guard lk(g_sem_mtx);
        id = g_sem_next++;
        g_sems[id] = s;
    }
    u32 h = hle_state(*a.t.proc).alloc(8);
    a.m.wr<u32>(h, id);
    a.ret(h);
}
HLE(LIB_SDL, SDL_DestroySemaphore) {
    if (a.u(0)) {
        std::lock_guard lk(g_sem_mtx);
        g_sems.erase(a.m.rd<u32>(a.u(0)));
    }
}
HLE(LIB_SDL, SDL_SemPost) {
    auto s = get_sem(a, a.u(0));
    if (!s) { a.ret((u32)-1); return; }
    {
        std::lock_guard lk(s->m);
        s->count++;
    }
    s->cv.notify_one();
    a.ret(0);
}
HLE(LIB_SDL, SDL_SemWait) {
    auto s = get_sem(a, a.u(0));
    if (!s) { a.ret((u32)-1); return; }
    std::unique_lock lk(s->m);
    while (s->count == 0) {
        if (a.t.exit_req) { a.ret((u32)-1); return; }
        s->cv.wait_for(lk, std::chrono::milliseconds(20));
    }
    s->count--;
    a.ret(0);
}
HLE(LIB_SDL, SDL_SemTryWait) {
    auto s = get_sem(a, a.u(0));
    if (!s) { a.ret((u32)-1); return; }
    std::lock_guard lk(s->m);
    if (!s->count) { a.ret(1); return; }  // SDL_MUTEX_TIMEDOUT
    s->count--;
    a.ret(0);
}
HLE(LIB_SDL, SDL_SemValue) {
    auto s = get_sem(a, a.u(0));
    if (!s) { a.ret(0); return; }
    std::lock_guard lk(s->m);
    a.ret(s->count);
}

// ---------------------------------------------------------------- atomics
HLE(LIB_SDL, SDL_AtomicAdd) { a.ret(atomic_fetch_add_raw((s32*)a.p(0), a.s(1))); }
HLE(LIB_SDL, SDL_AtomicSet) { a.ret(atomic_exchange_raw((s32*)a.p(0), a.s(1))); }
HLE(LIB_SDL, SDL_AtomicGet) { a.ret(atomic_load_raw((s32*)a.p(0))); }
HLE(LIB_SDL, SDL_AtomicCAS) {
    s32 expected = a.s(1);
    a.ret(atomic_cas_raw((s32*)a.p(0), &expected, a.s(2)) ? 1 : 0);
}
HLE(LIB_SDL, SDL_AtomicGetPtr) { a.ret(atomic_load_raw((u32*)a.p(0))); }
HLE(LIB_SDL, SDL_AtomicSetPtr) { a.ret(atomic_exchange_raw((u32*)a.p(0), a.u(1))); }
HLE(LIB_SDL, SDL_AtomicCASPtr) {
    u32 expected = a.u(1);
    a.ret(atomic_cas_raw((u32*)a.p(0), &expected, a.u(2)) ? 1 : 0);
}

// ---------------------------------------------------------------- misc
HLE(LIB_SDL, SDL_Delay) { sleep_ns(a.t, (u64)a.u(0) * 1000000ull); }
HLE(LIB_SDL, SDL_GetTicks) { a.ret(SDL_GetTicks()); }
HLE(LIB_SDL, SDL_GetPerformanceCounter) { a.ret64(SDL_GetPerformanceCounter()); }
HLE(LIB_SDL, SDL_GetPerformanceFrequency) { a.ret64(SDL_GetPerformanceFrequency()); }
HLE(LIB_SDL, SDL_memset) {
    if (a.u(0)) memset(a.p(0), a.s(1), a.u(2));
    a.ret(a.u(0));
}
HLE(LIB_SDL, SDL_LogInfo) {
    if (g_log_mask & LOG_HLE) LOGI("[sdl-log] %s\n", a.str(1).c_str());
}
HLE(LIB_SDL, SDL_Log) {
    if (g_log_mask & LOG_HLE) LOGI("[sdl-log] %s\n", a.str(0).c_str());
}
HLE(LIB_SDL, SDL_LogSetAllPriority) {}
HLE(LIB_SDL, SDL_EventState) { a.ret(1); }
HLE(LIB_SDL, SDL_GetScancodeFromKey) { a.ret(SDL_GetScancodeFromKey((SDL_Keycode)a.s(0))); }
HLE(LIB_SDL, SDL_GetWindowWMInfo) {
    u32 info = a.u(1);
    if (info) {
        u32 ver = a.m.rd<u32>(info) & 0xffffff;  // keep caller's version bytes
        memset(a.m.ptr(info), 0, 64);
        a.m.wr<u32>(info, ver);
        a.m.wr<u32>(info + 4, 0);  // SDL_SYSWM_UNKNOWN
    }
    a.ret(1);
}

// ---------------------------------------------------------------- audio helpers
HLE(LIB_SDL, SDL_MixAudioFormat) {
    if (a.u(0) && a.u(1)) SDL_MixAudioFormat((Uint8*)a.p(0), (const Uint8*)a.p(1), (SDL_AudioFormat)a.u(2), a.u(3), a.s(4));
}
HLE(LIB_SDL, SDL_MixAudio) {
    auto& st = hle_state(*a.t.proc);
    if (a.u(0) && a.u(1))
        SDL_MixAudioFormat((Uint8*)a.p(0), (const Uint8*)a.p(1), st.audio_format ? st.audio_format : AUDIO_S16LSB, a.u(2), a.s(3));
}
HLE(LIB_SDL, SDL_LoadWAV_RW) {
    // (src, freesrc, spec*, audio_buf**, audio_len*) -> spec*
    u32 rw = a.u(0), spec = a.u(2), bufpp = a.u(3), lenp = a.u(4);
    if (!rw || !a.m.rd<u32>(rw + 40)) { a.ret(0); return; }
    std::string path = a.m.read_str(a.m.rd<u32>(rw + 40));
    Resolved r = vfs::resolve(*a.t.proc, path, true);
    SDL_AudioSpec hs{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (r.err || r.kind != Resolved::HOST || !SDL_LoadWAV(r.host.c_str(), &hs, &buf, &len)) {
        LOGI("[sdl] LoadWAV %s failed\n", path.c_str());
        a.ret(0);
        return;
    }
    auto& st = hle_state(*a.t.proc);
    u32 g = st.alloc(len + 16);
    memcpy(a.m.ptr(g), buf, len);
    SDL_FreeWAV(buf);
    memset(a.m.ptr(spec), 0, 24);
    a.m.wr<s32>(spec, hs.freq);
    a.m.wr<u16>(spec + 4, hs.format);
    a.m.wr<u8>(spec + 6, hs.channels);
    a.m.wr<u8>(spec + 7, hs.silence);
    a.m.wr<u16>(spec + 8, hs.samples);
    a.m.wr<u32>(spec + 12, len);
    a.m.wr<u32>(bufpp, g);
    a.m.wr<u32>(lenp, len);
    a.ret(spec);
}
HLE(LIB_SDL, SDL_FreeWAV) {}
HLE(LIB_SDL, SDL_RWclose) { a.ret(0); }

// ---------------------------------------------------------------- surfaces & 2D renderer
// SDL_Surface (32-bit): flags 0, format* 4, w 8, h 12, pitch 16, pixels 20 ...
// SDL_PixelFormat: format 0, BitsPerPixel 8, BytesPerPixel 9, R/G/B/A masks 12..24
// Texture struct (ours, SDL_Texture-compatible prefix): fmt 4, access 8, w 12, h 16, driverdata 68 (-> gl tex),
// fbo 80, lock buffer 84, blend 88

static void gl_fmt_from_sdl(u32 fmt, GLFormat& f) {
    f = {0x80E1, 0x8367, 4};  // GL_BGRA, UNSIGNED_INT_8_8_8_8_REV
    switch (fmt) {
    case 0x16762004: f = {0x1908, 0x1401, 4}; break;  // ABGR8888 -> RGBA bytes
    case 0x16462004: f = {0x1908, 0x8035, 4}; break;  // RGBA8888 -> RGBA, 8_8_8_8
    case 0x16862004: f = {0x80E1, 0x8035, 4}; break;  // BGRA8888
    case 0x15151002: f = {0x1907, 0x8363, 2}; break;  // RGB565
    case 0x15551002: f = {0x1907, 0x8364, 2}; break;  // BGR565
    case 0x15321002: f = {0x80E1, 0x8366, 2}; break;  // ARGB1555
    case 0x15421002: f = {0x1908, 0x8033, 2}; break;  // RGBA4444
    case 0x15321001: f = {0x80E1, 0x8366, 2}; break;
    case 0x17101803: f = {0x1907, 0x1401, 3}; break;  // RGB24
    case 0x17401803: f = {0x80E0, 0x1401, 3}; break;  // BGR24
    }
}

static u32 new_texture(HleArgs& a, u32 renderer, u32 fmt, u32 access, s32 w, s32 h) {
    auto& st = hle_state(*a.t.proc);
    u32 tex = st.alloc(96);
    u32 dd = st.alloc(48);
    memset(a.m.ptr(tex), 0, 96);
    memset(a.m.ptr(dd), 0, 48);
    a.m.wr<u32>(tex + 4, fmt);
    a.m.wr<s32>(tex + 8, access);
    a.m.wr<s32>(tex + 12, w);
    a.m.wr<s32>(tex + 16, h);
    a.m.wr<u32>(tex + 32, renderer);
    a.m.wr<u32>(tex + 68, dd);
    u32 gltex = 0, fbo = 0;
    gl_create_target(w, h, &gltex, &fbo);
    a.m.wr<u32>(dd + 0, gltex);
    a.m.wr<u32>(dd + 4, 0x0DE1);
    a.m.wr<u32>(tex + 80, fbo);
    return tex;
}

HLE(LIB_SDL, SDL_CreateRGBSurfaceFrom) {
    // (pixels, w, h, depth, pitch, Rmask, Gmask, Bmask, Amask)
    auto& st = hle_state(*a.t.proc);
    u32 s = st.alloc(64), f = st.alloc(44);
    memset(a.m.ptr(s), 0, 64);
    memset(a.m.ptr(f), 0, 44);
    s32 depth = a.s(3);
    a.m.wr<u8>(f + 8, (u8)depth);
    a.m.wr<u8>(f + 9, (u8)((depth + 7) / 8));
    a.m.wr<u32>(f + 12, a.u(5));
    a.m.wr<u32>(f + 16, a.u(6));
    a.m.wr<u32>(f + 20, a.u(7));
    a.m.wr<u32>(f + 24, a.u(8));
    a.m.wr<s32>(f + 36, 1);
    a.m.wr<u32>(s + 4, f);
    a.m.wr<s32>(s + 8, a.s(1));
    a.m.wr<s32>(s + 12, a.s(2));
    a.m.wr<s32>(s + 16, a.s(4));
    a.m.wr<u32>(s + 20, a.u(0));
    a.m.wr<s32>(s + 56, 1);
    a.ret(s);
}
HLE(LIB_SDL, SDL_FreeSurface) {}
HLE(LIB_SDL, SDL_CreateTextureFromSurface) {
    u32 r = a.u(0), s = a.u(1);
    if (!s) { a.ret(0); return; }
    u32 f = a.m.rd<u32>(s + 4);
    s32 w = a.m.rd<s32>(s + 8), h = a.m.rd<s32>(s + 12), pitch = a.m.rd<s32>(s + 16);
    u32 pixels = a.m.rd<u32>(s + 20);
    u8 bpp = a.m.rd<u8>(f + 8);
    u32 rmask = a.m.rd<u32>(f + 12), amask = a.m.rd<u32>(f + 24);
    u32 sdlfmt;
    if (bpp == 32) sdlfmt = rmask == 0x000000ff ? 0x16762004 : rmask == 0xff000000 ? 0x16462004 : 0x16362004;
    else if (bpp == 24) sdlfmt = rmask == 0x000000ff ? 0x17101803 : 0x17401803;
    else sdlfmt = 0x15151002;
    u32 tex = new_texture(a, r, sdlfmt, 0, w, h);
    a.m.wr<u32>(tex + 88, amask ? 1 : 0);  // blended if the surface has alpha
    GLFormat gf;
    gl_fmt_from_sdl(sdlfmt, gf);
    if (pixels) gl_upload(a.m.rd<u32>(a.m.rd<u32>(tex + 68)), 0, 0, w, h, pitch / gf.bpp, gf, a.m.ptr(pixels));
    a.ret(tex);
}
HLE(LIB_SDL, SDL_LockTexture) {
    // (tex, rect*, pixels**, pitch*)
    u32 tex = a.u(0), rect = a.u(1);
    s32 w = a.m.rd<s32>(tex + 12), h = a.m.rd<s32>(tex + 16);
    GLFormat gf;
    gl_fmt_from_sdl(a.m.rd<u32>(tex + 4), gf);
    u32 buf = a.m.rd<u32>(tex + 84);
    if (!buf) {
        buf = hle_state(*a.t.proc).alloc(w * h * gf.bpp);
        a.m.wr<u32>(tex + 84, buf);
    }
    s32 x = 0, y = 0, rw = w, rh = h;
    if (rect) {
        x = a.m.rd<s32>(rect);
        y = a.m.rd<s32>(rect + 4);
        rw = a.m.rd<s32>(rect + 8);
        rh = a.m.rd<s32>(rect + 12);
    }
    a.m.wr<s32>(tex + 52, x);
    a.m.wr<s32>(tex + 56, y);
    a.m.wr<s32>(tex + 60, rw);
    a.m.wr<s32>(tex + 64, rh);
    a.m.wr<u32>(a.u(2), buf + (y * w + x) * gf.bpp);
    a.m.wr<s32>(a.u(3), w * gf.bpp);
    a.ret(0);
}
HLE(LIB_SDL, SDL_UnlockTexture) {
    u32 tex = a.u(0);
    s32 w = a.m.rd<s32>(tex + 12);
    GLFormat gf;
    gl_fmt_from_sdl(a.m.rd<u32>(tex + 4), gf);
    u32 buf = a.m.rd<u32>(tex + 84);
    s32 x = a.m.rd<s32>(tex + 52), y = a.m.rd<s32>(tex + 56), rw = a.m.rd<s32>(tex + 60), rh = a.m.rd<s32>(tex + 64);
    if (buf) gl_upload(a.m.rd<u32>(a.m.rd<u32>(tex + 68)), x, y, rw, rh, w, gf, a.m.ptr(buf + (y * w + x) * gf.bpp));
    a.ret(0);
}
HLE(LIB_SDL, SDL_UpdateTexture) {
    // (tex, rect*, pixels, pitch)
    u32 tex = a.u(0), rect = a.u(1);
    s32 w = a.m.rd<s32>(tex + 12), h = a.m.rd<s32>(tex + 16);
    GLFormat gf;
    gl_fmt_from_sdl(a.m.rd<u32>(tex + 4), gf);
    s32 x = 0, y = 0, rw = w, rh = h;
    if (rect) {
        x = a.m.rd<s32>(rect);
        y = a.m.rd<s32>(rect + 4);
        rw = a.m.rd<s32>(rect + 8);
        rh = a.m.rd<s32>(rect + 12);
    }
    gl_upload(a.m.rd<u32>(a.m.rd<u32>(tex + 68)), x, y, rw, rh, a.s(3) / gf.bpp, gf, a.p(2));
    a.ret(0);
}
static void read_rect(HleArgs& a, u32 p, int def_w, int def_h, int r[4]) {
    if (p) {
        for (int i = 0; i < 4; i++) r[i] = a.m.rd<s32>(p + i * 4);
    } else {
        r[0] = r[1] = 0;
        r[2] = def_w;
        r[3] = def_h;
    }
}
HLE(LIB_SDL, SDL_RenderCopy) {
    // (renderer, tex, srcrect*, dstrect*)
    u32 rnd = a.u(0), tex = a.u(1);
    if (!tex) { a.ret((u32)-1); return; }
    s32 tw = a.m.rd<s32>(tex + 12), th = a.m.rd<s32>(tex + 16);
    u32 target = a.m.rd<u32>(rnd);  // current render target texture (or 0)
    int vw = target ? a.m.rd<s32>(target + 12) : 1280, vh = target ? a.m.rd<s32>(target + 16) : 720;
    int src[4], dst[4];
    read_rect(a, a.u(2), tw, th, src);
    read_rect(a, a.u(3), vw, vh, dst);
    gl_render_copy(a.m.rd<u32>(a.m.rd<u32>(tex + 68)), tw, th, src, dst, vw, vh, a.m.rd<u32>(tex + 88) != 0);
    a.ret(0);
}
HLE(LIB_SDL, SDL_RenderPresent) { gl_swap(); }
HLE(LIB_SDL, SDL_RenderClear) {
    gl_render_clear();
    a.ret(0);
}
HLE(LIB_SDL, SDL_SetRenderDrawColor) {
    gl_set_draw_color(a.u(1) / 255.f, a.u(2) / 255.f, a.u(3) / 255.f, a.u(4) / 255.f);
    a.ret(0);
}
HLE(LIB_SDL, SDL_SetTextureBlendMode) {
    if (a.u(0)) a.m.wr<u32>(a.u(0) + 88, a.u(1) != 0);
    a.ret(0);
}
