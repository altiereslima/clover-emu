// HLE of libSDL2 for guest programs: window, GL context, render targets, input, events, audio.
#include "hle.h"
#include "hle_gl.h"
#include "display.h"
#include <SDL.h>
#include <algorithm>

// SDL constants (stable across SDL2 versions)
enum : u32 {
    G_SDL_INIT_TIMER = 0x1, G_SDL_INIT_AUDIO = 0x10, G_SDL_INIT_VIDEO = 0x20, G_SDL_INIT_JOYSTICK = 0x200,
    G_SDL_INIT_HAPTIC = 0x1000, G_SDL_INIT_GAMECONTROLLER = 0x2000, G_SDL_INIT_EVENTS = 0x4000,
};
enum : u32 {
    EV_QUIT = 0x100, EV_KEYDOWN = 0x300, EV_KEYUP = 0x301, EV_JOYBUTTONDOWN = 0x603, EV_JOYBUTTONUP = 0x604,
    EV_JOYDEVICEADDED = 0x605, EV_JOYDEVICEREMOVED = 0x606, EV_CBUTTONDOWN = 0x651, EV_CBUTTONUP = 0x652,
    EV_CDEVICEADDED = 0x653, EV_CDEVICEREMOVED = 0x654,
};
constexpr u32 SC_POWER = 102, SC_VOLUMEUP = 128;
constexpr u32 SYM_POWER = 0x40000066, SYM_VOLUMEUP = 0x40000080;

// ---------------------------------------------------------------- input (host side shared state)
namespace input {
static std::mutex g_in_mtx;
static bool g_btn[NUM_PADS][NUM_BTN];
static bool g_attached[NUM_PADS] = {true, false};
static bool g_power = true;  // slide switch on
static bool g_reset = false;

static std::vector<std::shared_ptr<Process>> subscribers() {
    std::vector<std::shared_ptr<Process>> out;
    std::lock_guard lk(g_kernel.mtx);
    for (auto& [pid, p] : g_kernel.procs)
        if (!p->zombie && p->hle && p->hle->events_enabled) out.push_back(p);
    return out;
}

static void push_event(Process& p, const GuestEvent& e) {
    auto& st = *p.hle;
    std::lock_guard lk(st.mtx);
    if (st.events.size() > 1024) st.events.pop_front();
    st.events.push_back(e);
}

static GuestEvent mk(u32 type) {
    GuestEvent e{};
    memcpy(e.data, &type, 4);
    u32 ts = SDL_GetTicks();
    memcpy(e.data + 4, &ts, 4);
    return e;
}

void set_button(int pad, int btn, bool down) {
    {
        std::lock_guard lk(g_in_mtx);
        if (g_btn[pad][btn] == down) return;
        g_btn[pad][btn] = down;
        if (!g_attached[pad]) return;
    }
    GuestEvent e = mk(down ? EV_CBUTTONDOWN : EV_CBUTTONUP);
    s32 which = pad;
    memcpy(e.data + 8, &which, 4);
    e.data[12] = (u8)btn;
    e.data[13] = down ? 1 : 0;
    for (auto& p : subscribers()) push_event(*p, e);
}

bool get_button(int pad, int btn) {
    std::lock_guard lk(g_in_mtx);
    return g_attached[pad] && g_btn[pad][btn];
}

static void write_keystate(u32 sc, bool down) {
    for (auto& p : subscribers()) {
        auto& st = *p->hle;
        if (st.keystate && p->mem) p->mem->wr<u8>(st.keystate + sc, down ? 1 : 0);
    }
}

void broadcast_key(bool down, u32 scancode, u32 sym) {
    write_keystate(scancode, down);
    GuestEvent e = mk(down ? EV_KEYDOWN : EV_KEYUP);
    u32 win = 1;
    memcpy(e.data + 8, &win, 4);
    e.data[12] = down ? 1 : 0;
    memcpy(e.data + 16, &scancode, 4);
    memcpy(e.data + 20, &sym, 4);
    for (auto& p : subscribers()) push_event(*p, e);
}

void reset_key(bool down) {
    {
        std::lock_guard lk(g_in_mtx);
        if (g_reset == down) return;
        g_reset = down;
    }
    LOGI("[input] RESET button %s\n", down ? "pressed" : "released");
    broadcast_key(down, SC_VOLUMEUP, SYM_VOLUMEUP);
}

void power_off() {
    {
        std::lock_guard lk(g_in_mtx);
        if (!g_power) return;
        g_power = false;
    }
    LOGI("[input] POWER switch -> off\n");
    broadcast_key(false, SC_POWER, SYM_POWER);
}

bool power_on() {
    std::lock_guard lk(g_in_mtx);
    return g_power;
}

void set_attached(int pad, bool att) {
    {
        std::lock_guard lk(g_in_mtx);
        if (g_attached[pad] == att) return;
        g_attached[pad] = att;
    }
    for (auto& p : subscribers()) {
        GuestEvent e = mk(att ? EV_JOYDEVICEADDED : EV_JOYDEVICEREMOVED);
        s32 which = pad;
        memcpy(e.data + 8, &which, 4);
        push_event(*p, e);
        e = mk(att ? EV_CDEVICEADDED : EV_CDEVICEREMOVED);
        memcpy(e.data + 8, &which, 4);
        push_event(*p, e);
    }
}

int num_attached() {
    std::lock_guard lk(g_in_mtx);
    int n = 0;
    for (int i = 0; i < NUM_PADS; i++)
        if (g_attached[i]) n = i + 1;
    return n;
}
}  // namespace input

static const char* pad_name(int i) { return i == 0 ? "Nintendo Clovercon - controller1" : "Nintendo Clovercon - controller2"; }

static void sdl_enable_events(HleArgs& a, u32 flags) {
    auto& st = hle_state(*a.t.proc);
    bool had = st.events_enabled;
    st.sdl_flags |= flags;
    st.events_enabled = true;
    if (!st.keystate) {
        st.keystate = st.alloc(512);
        if (input::power_on()) a.m.wr<u8>(st.keystate + SC_POWER, 1);
    }
    if (!had || (flags & (G_SDL_INIT_JOYSTICK | G_SDL_INIT_GAMECONTROLLER))) {
        // device added events for attached pads
        int n = input::num_attached();
        std::lock_guard lk(st.mtx);
        for (int i = 0; i < n; i++) {
            GuestEvent e{};
            u32 type = EV_JOYDEVICEADDED;
            memcpy(e.data, &type, 4);
            memcpy(e.data + 8, &i, 4);
            st.events.push_back(e);
            type = EV_CDEVICEADDED;
            memcpy(e.data, &type, 4);
            st.events.push_back(e);
        }
    }
}

HLE(LIB_SDL, SDL_Init) { sdl_enable_events(a, a.u(0)); a.ret(0); }
HLE(LIB_SDL, SDL_InitSubSystem) { sdl_enable_events(a, a.u(0)); a.ret(0); }
HLE(LIB_SDL, SDL_QuitSubSystem) { a.ret(0); }
HLE(LIB_SDL, SDL_Quit) {
    auto& st = hle_state(*a.t.proc);
    st.events_enabled = false;
    a.ret(0);
}
HLE(LIB_SDL, SDL_SetHint) { a.ret(1); }
HLE(LIB_SDL, SDL_GetError) {
    auto& st = hle_state(*a.t.proc);
    a.ret(st.intern(""));
}
HLE(LIB_SDL, SDL_free) { a.ret(0); }

// ---------------------------------------------------------------- events
static bool pop_event(HleArgs& a, u32 evp) {
    auto& st = hle_state(*a.t.proc);
    std::lock_guard lk(st.mtx);
    if (st.events.empty()) return false;
    if (evp) {
        a.m.write_bytes(evp, st.events.front().data, 56);
        st.events.pop_front();
    }
    return true;
}
HLE(LIB_SDL, SDL_PollEvent) { a.ret(pop_event(a, a.u(0)) ? 1 : 0); }
HLE(LIB_SDL, SDL_WaitEvent) {
    u32 evp = a.u(0);
    while (true) {
        if (pop_event(a, evp)) { a.ret(1); return; }
        if (a.t.signal_pending()) { a.ret(0); return; }
        sleep_ns(a.t, 5000000ull);
    }
}
HLE(LIB_SDL, SDL_GetKeyboardState) {
    auto& st = hle_state(*a.t.proc);
    if (!st.keystate) {
        st.keystate = st.alloc(512);
        if (input::power_on()) a.m.wr<u8>(st.keystate + SC_POWER, 1);
    }
    if (a.u(0)) a.m.wr<s32>(a.u(0), 512);
    a.ret(st.keystate);
}
HLE(LIB_SDL, SDL_GetRelativeMouseState) {
    if (a.u(0)) a.m.wr<s32>(a.u(0), 0);
    if (a.u(1)) a.m.wr<s32>(a.u(1), 0);
    a.ret(0);
}

// ---------------------------------------------------------------- joysticks / controllers
HLE(LIB_SDL, SDL_NumJoysticks) { a.ret(input::num_attached()); }
HLE(LIB_SDL, SDL_IsGameController) { a.ret(a.s(0) >= 0 && a.s(0) < input::num_attached() ? 1 : 0); }
HLE(LIB_SDL, SDL_JoystickNameForIndex) {
    int i = a.s(0);
    auto& st = hle_state(*a.t.proc);
    a.ret(i >= 0 && i < input::NUM_PADS ? st.intern(pad_name(i)) : 0);
}
static u32 open_handle(HleArgs& a, int idx, bool controller) {
    auto& st = hle_state(*a.t.proc);
    if (idx < 0 || idx >= input::NUM_PADS) return 0;
    auto& m = controller ? st.controllers : st.joysticks;
    for (auto& [h, i] : m)
        if (i == idx) return h;
    u32 h = st.alloc(64);
    a.m.wr<s32>(h, idx);
    m[h] = idx;
    return h;
}
static int handle_index(HleArgs& a, u32 h) {
    auto& st = hle_state(*a.t.proc);
    auto it = st.controllers.find(h);
    if (it != st.controllers.end()) return it->second;
    it = st.joysticks.find(h);
    if (it != st.joysticks.end()) return it->second;
    return -1;
}
HLE(LIB_SDL, SDL_GameControllerOpen) { a.ret(open_handle(a, a.s(0), true)); }
HLE(LIB_SDL, SDL_JoystickOpen) { a.ret(open_handle(a, a.s(0), false)); }
HLE(LIB_SDL, SDL_GameControllerFromInstanceID) {
    auto& st = hle_state(*a.t.proc);
    for (auto& [h, i] : st.controllers)
        if (i == a.s(0)) { a.ret(h); return; }
    a.ret(0);
}
HLE(LIB_SDL, SDL_GameControllerClose) { a.ret(0); }
HLE(LIB_SDL, SDL_JoystickClose) { a.ret(0); }
HLE(LIB_SDL, SDL_GameControllerGetJoystick) { a.ret(open_handle(a, handle_index(a, a.u(0)), false)); }
HLE(LIB_SDL, SDL_JoystickInstanceID) { a.ret(handle_index(a, a.u(0))); }
HLE(LIB_SDL, SDL_JoystickName) {
    int i = handle_index(a, a.u(0));
    auto& st = hle_state(*a.t.proc);
    a.ret(i >= 0 ? st.intern(pad_name(i)) : 0);
}
HLE(LIB_SDL, SDL_GameControllerGetAttached) {
    int i = handle_index(a, a.u(0));
    a.ret(i >= 0 && i < input::num_attached() ? 1 : 0);
}
HLE(LIB_SDL, SDL_GameControllerGetButton) {
    int i = handle_index(a, a.u(0));
    int b = a.s(1);
    a.ret(i >= 0 && b >= 0 && b < input::NUM_BTN && input::get_button(i, b) ? 1 : 0);
}
HLE(LIB_SDL, SDL_GameControllerGetAxis) { a.ret(0); }
HLE(LIB_SDL, SDL_GameControllerUpdate) { a.ret(0); }
HLE(LIB_SDL, SDL_GameControllerAddMapping) { a.ret(1); }
HLE(LIB_SDL, SDL_GameControllerAddMappingsFromRW) { a.ret(1); }
HLE(LIB_SDL, SDL_RWFromFile) {
    // Opaque RWops consumed only by our own functions (AddMappingsFromRW, LoadWAV_RW).
    // Private field +40 holds the guest path.
    auto& st = hle_state(*a.t.proc);
    u32 h = st.alloc(64);
    memset(a.m.ptr(h), 0, 64);
    a.m.wr<u32>(h + 20, 2);  // SDL_RWOPS_STDFILE
    a.m.wr<u32>(h + 40, st.alloc_str(a.str(0)));
    a.ret(h);
}

// ---------------------------------------------------------------- video
static constexpr int GUEST_W = 1280, GUEST_H = 720;

static u32 make_format(HleArgs& a, u32 fmt) {
    auto& st = hle_state(*a.t.proc);
    u32 f = st.alloc(44);
    u8* p = a.m.ptr(f);
    memset(p, 0, 44);
    memcpy(p, &fmt, 4);
    p[8] = 32;
    p[9] = 4;
    u32 rm = 0x00ff0000, gm = 0x0000ff00, bm = 0x000000ff, am = 0xff000000;
    memcpy(p + 12, &rm, 4);
    memcpy(p + 16, &gm, 4);
    memcpy(p + 20, &bm, 4);
    memcpy(p + 24, &am, 4);
    p[32] = 16; p[33] = 8; p[34] = 0; p[35] = 24;  // shifts
    s32 rc = 1;
    memcpy(p + 36, &rc, 4);
    return f;
}

HLE(LIB_SDL, SDL_CreateWindow) {
    auto& st = hle_state(*a.t.proc);
    if (!st.window) {
        st.window = st.alloc(64);
        a.m.wr<u32>(st.window, 0x57494e44);  // 'WIND'
    }
    LOGC(LOG_HLE, "[sdl] pid %d CreateWindow '%s' %dx%d flags %x\n", a.t.proc->pid, a.str(0).c_str(), a.s(3), a.s(4), a.u(5));
    a.ret(st.window);
}
HLE(LIB_SDL, SDL_DestroyWindow) { a.ret(0); }
HLE(LIB_SDL, SDL_GetWindowSize) {
    if (a.u(1)) a.m.wr<s32>(a.u(1), GUEST_W);
    if (a.u(2)) a.m.wr<s32>(a.u(2), GUEST_H);
    a.ret(0);
}
HLE(LIB_SDL, SDL_SetWindowSize) { a.ret(0); }
HLE(LIB_SDL, SDL_SetWindowFullscreen) { a.ret(0); }
HLE(LIB_SDL, SDL_GetWindowID) { a.ret(1); }
HLE(LIB_SDL, SDL_GetWindowFromID) { a.ret(hle_state(*a.t.proc).window); }
HLE(LIB_SDL, SDL_GetWindowPixelFormat) { a.ret(0x16362004); }  // ARGB8888
HLE(LIB_SDL, SDL_GetNumVideoDisplays) { a.ret(1); }
HLE(LIB_SDL, SDL_GetDesktopDisplayMode) {
    u32 m = a.u(1);
    a.m.wr<u32>(m, 0x16362004);
    a.m.wr<s32>(m + 4, GUEST_W);
    a.m.wr<s32>(m + 8, GUEST_H);
    a.m.wr<s32>(m + 12, 60);
    a.m.wr<u32>(m + 16, 0);
    a.ret(0);
}
HLE(LIB_SDL, SDL_AllocFormat) { a.ret(make_format(a, a.u(0))); }
HLE(LIB_SDL, SDL_FreeFormat) { a.ret(0); }
HLE(LIB_SDL, SDL_GetWindowSurface) {
    auto& st = hle_state(*a.t.proc);
    if (!st.window_surface) {
        u32 s = st.alloc(64);
        memset(a.m.ptr(s), 0, 64);
        a.m.wr<u32>(s + 4, make_format(a, 0x16362004));
        a.m.wr<s32>(s + 8, GUEST_W);
        a.m.wr<s32>(s + 12, GUEST_H);
        a.m.wr<s32>(s + 16, GUEST_W * 4);
        a.m.wr<s32>(s + 52, GUEST_W);
        a.m.wr<s32>(s + 56, GUEST_H);
        a.m.wr<s32>(s + 64, 1);
        st.window_surface = s;
    }
    a.ret(st.window_surface);
}

// GL contexts
HLE(LIB_SDL, SDL_GL_SetAttribute) { a.ret(0); }
HLE(LIB_SDL, SDL_GL_CreateContext) {
    auto& st = hle_state(*a.t.proc);
    auto ctx = gl_create_context();
    if (!ctx) { a.ret(0); return; }
    ctx->guest_handle = st.alloc(16);
    {
        std::lock_guard lk(st.mtx);
        st.gl_contexts.push_back(ctx);
    }
    gl_make_current(ctx.get());
    LOGC(LOG_HLE | LOG_GL, "[sdl] pid %d GL context %08x created\n", a.t.proc->pid, ctx->guest_handle);
    a.ret(ctx->guest_handle);
}
static std::shared_ptr<GLCtx> find_ctx(HleArgs& a, u32 h) {
    auto& st = hle_state(*a.t.proc);
    std::lock_guard lk(st.mtx);
    for (auto& c : st.gl_contexts)
        if (c->guest_handle == h) return c;
    return nullptr;
}
HLE(LIB_SDL, SDL_GL_MakeCurrent) {
    auto c = find_ctx(a, a.u(1));
    gl_make_current(c.get());
    a.ret(0);
}
HLE(LIB_SDL, SDL_GL_DeleteContext) {
    auto c = find_ctx(a, a.u(0));
    if (c) {
        auto& st = hle_state(*a.t.proc);
        if (gl_current() == c.get()) gl_make_current(nullptr);
        gl_destroy_context(c.get());
        std::lock_guard lk(st.mtx);
        st.gl_contexts.erase(std::remove(st.gl_contexts.begin(), st.gl_contexts.end(), c), st.gl_contexts.end());
    }
    a.ret(0);
}
HLE(LIB_SDL, SDL_GL_SetSwapInterval) {
    gl_set_swap_interval(a.s(0));
    a.ret(0);
}
HLE(LIB_SDL, SDL_GL_SwapWindow) {
    gl_swap();
    a.ret(0);
}

// SDL_Renderer emulation on top of the current GL context
HLE(LIB_SDL, SDL_CreateRenderer) {
    auto& st = hle_state(*a.t.proc);
    u32 r = st.alloc(64);
    memset(a.m.ptr(r), 0, 64);
    LOGC(LOG_HLE, "[sdl] CreateRenderer flags %x -> %08x\n", a.u(2), r);
    a.ret(r);
}
HLE(LIB_SDL, SDL_DestroyRenderer) { a.ret(0); }
HLE(LIB_SDL, SDL_CreateTexture) {
    auto& st = hle_state(*a.t.proc);
    u32 fmt = a.u(1), access = a.u(2);
    s32 w = a.s(3), h = a.s(4);
    // SDL_Texture layout (SDL 2.0.x, 32-bit) + GLES2_TextureData
    u32 tex = st.alloc(96);
    u32 dd = st.alloc(48);
    memset(a.m.ptr(tex), 0, 96);
    memset(a.m.ptr(dd), 0, 48);
    a.m.wr<u32>(tex + 4, fmt);
    a.m.wr<s32>(tex + 8, access);
    a.m.wr<s32>(tex + 12, w);
    a.m.wr<s32>(tex + 16, h);
    a.m.wr<u32>(tex + 32, a.u(0));
    a.m.wr<u32>(tex + 68, dd);
    u32 gltex = 0, fbo = 0;
    gl_create_target(w, h, &gltex, &fbo);
    a.m.wr<u32>(dd + 0, gltex);
    a.m.wr<u32>(dd + 4, 0x0DE1);  // GL_TEXTURE_2D
    a.m.wr<u32>(dd + 8, 0x1908);  // GL_RGBA
    a.m.wr<u32>(dd + 12, 0x1401); // GL_UNSIGNED_BYTE
    a.m.wr<u32>(tex + 80, fbo);   // private: fbo name
    LOGC(LOG_HLE | LOG_GL, "[sdl] CreateTexture %dx%d fmt %x access %d -> tex %u fbo %u\n", w, h, fmt, access, gltex, fbo);
    a.ret(tex);
}
HLE(LIB_SDL, SDL_DestroyTexture) {
    u32 tex = a.u(0);
    if (tex) {
        u32 dd = a.m.rd<u32>(tex + 68);
        gl_destroy_target(a.m.rd<u32>(dd), a.m.rd<u32>(tex + 80));
    }
    a.ret(0);
}
HLE(LIB_SDL, SDL_SetRenderTarget) {
    u32 r = a.u(0), tex = a.u(1);
    a.m.wr<u32>(r + 0, tex);  // remember current target in renderer object
    if (tex) {
        u32 fbo = a.m.rd<u32>(tex + 80);
        gl_bind_target(fbo, a.m.rd<s32>(tex + 12), a.m.rd<s32>(tex + 16));
    } else {
        gl_bind_target(0, GUEST_W, GUEST_H);
    }
    a.ret(0);
}
HLE(LIB_SDL, SDL_GetRenderTarget) { a.ret(a.m.rd<u32>(a.u(0))); }
HLE(LIB_SDL, SDL_GL_UnbindTexture) {
    gl_unbind_texture();
    a.ret(0);
}

// ---------------------------------------------------------------- audio
static void SDLCALL audio_cb(void* ud, Uint8* stream, int len) {
    auto* st = (HleProcState*)ud;
    auto p = st->proc.lock();
    if (!p || p->zombie || !p->mem || st->audio_paused) {
        memset(stream, 0, len);
        return;
    }
    if ((u32)len > (u32)st->audio_samples * 16) len = st->audio_samples * 16;
    memset(p->mem->ptr(st->audio_buf), st->audio_format == AUDIO_U8 ? 0x80 : 0, len);
    guest_call(p, st->audio_cb, {st->audio_userdata, st->audio_buf, (u32)len}, st->audio_tls);
    if (p->mem) memcpy(stream, p->mem->ptr(st->audio_buf), len);
}

// Shared by SDL_OpenAudio / SDL_OpenAudioDevice. Returns host device id or 0.
static u32 open_audio(HleArgs& a, u32 des, u32 obt, bool legacy) {
    auto& st = hle_state(*a.t.proc);
    if (st.audio_open) return 0;
    SDL_AudioSpec want{}, have{};
    want.freq = a.m.rd<s32>(des);
    want.format = a.m.rd<u16>(des + 4);
    want.channels = a.m.rd<u8>(des + 6);
    want.samples = a.m.rd<u16>(des + 8);
    want.callback = audio_cb;
    want.userdata = &st;
    st.audio_cb = a.m.rd<u32>(des + 16);
    st.audio_userdata = a.m.rd<u32>(des + 20);
    st.audio_tls = a.t.tls;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev) {
        LOGI("[sdl] OpenAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    st.audio_dev = dev;
    st.audio_freq = have.freq;
    st.audio_format = have.format;
    st.audio_channels = have.channels;
    st.audio_samples = have.samples;
    st.audio_buf = st.alloc(have.samples * 16 + 64);
    st.audio_open = true;
    st.audio_paused = true;
    u32 out = obt ? obt : (legacy ? des : 0);
    if (out) {
        a.m.wr<s32>(out, have.freq);
        a.m.wr<u16>(out + 4, have.format);
        a.m.wr<u8>(out + 6, have.channels);
        a.m.wr<u8>(out + 7, have.silence);
        a.m.wr<u16>(out + 8, have.samples);
        a.m.wr<u32>(out + 12, have.size);
        a.m.wr<u32>(out + 16, st.audio_cb);
        a.m.wr<u32>(out + 20, st.audio_userdata);
    }
    LOGC(LOG_HLE, "[sdl] OpenAudio %d Hz fmt %x ch %d samples %d\n", have.freq, have.format, have.channels, have.samples);
    return dev;
}
HLE(LIB_SDL, SDL_OpenAudio) { a.ret(open_audio(a, a.u(0), a.u(1), true) ? 0 : (u32)-1); }
HLE(LIB_SDL, SDL_OpenAudioDevice) { a.ret(open_audio(a, a.u(2), a.u(3), false)); }
HLE(LIB_SDL, SDL_PauseAudioDevice) {
    auto& st = hle_state(*a.t.proc);
    st.audio_paused = a.u(1) != 0;
    if (st.audio_dev) SDL_PauseAudioDevice(st.audio_dev, st.audio_paused ? 1 : 0);
    a.ret(0);
}
HLE(LIB_SDL, SDL_PauseAudio) {
    auto& st = hle_state(*a.t.proc);
    st.audio_paused = a.u(0) != 0;
    if (st.audio_dev) SDL_PauseAudioDevice(st.audio_dev, st.audio_paused ? 1 : 0);
    a.ret(0);
}
HLE(LIB_SDL, SDL_GetAudioStatus) {
    auto& st = hle_state(*a.t.proc);
    a.ret(!st.audio_open ? 0 : st.audio_paused ? 2 : 1);
}
static void close_audio(HleProcState& st) {
    if (st.audio_dev) {
        st.audio_paused = true;
        SDL_CloseAudioDevice(st.audio_dev);
        st.audio_dev = 0;
    }
    st.audio_open = false;
}
HLE(LIB_SDL, SDL_CloseAudioDevice) {
    close_audio(hle_state(*a.t.proc));
    a.ret(0);
}
HLE(LIB_SDL, SDL_CloseAudio) {
    close_audio(hle_state(*a.t.proc));
    a.ret(0);
}

extern void alsa_process_cleanup(HleProcState& st);

void hle_sdl_process_cleanup(Process& p, HleProcState& st) {
    st.events_enabled = false;
    close_audio(st);
    alsa_process_cleanup(st);
    std::vector<std::shared_ptr<GLCtx>> ctxs;
    {
        std::lock_guard lk(st.mtx);
        ctxs.swap(st.gl_contexts);
    }
    for (auto& c : ctxs) gl_destroy_context(c.get());
}

void hle_sdl_push_quit(Process& p) {
    if (!p.hle) return;
    GuestEvent e{};
    u32 type = EV_QUIT;
    memcpy(e.data, &type, 4);
    u32 ts = SDL_GetTicks();
    memcpy(e.data + 4, &ts, 4);
    {
        std::lock_guard lk(p.hle->mtx);
        p.hle->events.push_back(e);
    }
    std::lock_guard lk(p.pmtx);
    for (Thread* t : p.threads) {
        std::lock_guard wl(t->wmtx);
        t->wcv.notify_all();
    }
}
