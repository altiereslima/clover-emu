// High-level emulation of the hardware-facing libraries: libSDL2, libMali (EGL/GLES), libasound.
#pragma once
#include "kernel.h"
#include <map>
#include <deque>

enum HleLib { LIB_MALI = 0, LIB_SDL = 1, LIB_ASOUND = 2, LIB_COUNT };

struct HleArgs {
    Thread& t;
    AddressSpace& m;
    std::array<u32, 16>& R;
    std::array<u32, 64>& X;
    HleArgs(Thread& th) : t(th), m(*th.proc->mem), R(th.jit->Regs()), X(th.jit->ExtRegs()) {}
    u32 u(int i) const { return i < 4 ? R[i] : m.rd<u32>(R[13] + (i - 4) * 4); }
    s32 s(int i) const { return (s32)u(i); }
    // i-th VFP single-precision argument register (s0..s15)
    float f(int i) const { float v; memcpy(&v, &X[i], 4); return v; }
    double d(int i) const { double v; memcpy(&v, &X[i * 2], 8); return v; }
    void* p(int i) const { u32 a = u(i); return a ? m.ptr(a) : nullptr; }
    std::string str(int i) const { u32 a = u(i); return a ? m.read_str(a) : std::string(); }
    void ret(u32 v) { R[0] = v; }
    void ret64(u64 v) { R[0] = (u32)v; R[1] = (u32)(v >> 32); }
    void retf(float v) { memcpy(&X[0], &v, 4); }
};

using HleFn = void (*)(HleArgs&);
struct HleRegister {
    HleRegister(HleLib lib, const char* name, HleFn fn);
};
#define HLE(lib, name)                                         \
    static void hle_fn_##name(HleArgs& a);                     \
    static HleRegister hle_reg_##name(lib, #name, hle_fn_##name); \
    static void hle_fn_##name(HleArgs& a)

// ---------------------------------------------------------------- per-process state
struct GuestEvent {
    u8 data[56];
};

struct GLCtx;      // hle_gl.cpp
struct AlsaPcm;    // hle_alsa.cpp

struct HleProcState {
    std::mutex mtx;
    // guest arena for HLE-owned objects
    u32 arena = 0, arena_used = 0, arena_size = 0;
    u32 alloc(u32 size);
    u32 alloc_str(const std::string& s);
    std::map<std::string, u32> interned;  // persistent strings (names, errors)
    u32 intern(const std::string& s);

    // SDL
    u32 sdl_flags = 0;
    bool events_enabled = false;
    std::deque<GuestEvent> events;
    u32 keystate = 0;           // guest Uint8[512]
    u32 window = 0;             // guest handle
    u32 window_surface = 0;
    std::map<u32, int> controllers;  // guest handle -> pad index
    std::map<u32, int> joysticks;
    u32 error_str = 0;
    std::vector<std::shared_ptr<GLCtx>> gl_contexts;
    // audio (SDL_OpenAudio)
    bool audio_open = false;
    u32 audio_cb = 0, audio_userdata = 0;
    u32 audio_buf = 0;
    int audio_freq = 0, audio_channels = 0, audio_samples = 0;
    u16 audio_format = 0;
    u32 audio_dev = 0;  // host SDL_AudioDeviceID
    std::atomic<bool> audio_paused{true};
    u32 audio_tls = 0;
    // alsa
    std::map<u32, std::shared_ptr<AlsaPcm>> pcms;
    std::weak_ptr<Process> proc;
};

HleProcState& hle_state(Process& p);

// Input (shared host state) — implemented in hle_sdl.cpp
namespace input {
constexpr int NUM_PADS = 2;
// SDL_GameControllerButton indices
enum Btn { A = 0, B = 1, X = 2, Y = 3, BACK = 4, GUIDE = 5, START = 6, LSTICK = 7, RSTICK = 8, LSHOULDER = 9,
           RSHOULDER = 10, DUP = 11, DDOWN = 12, DLEFT = 13, DRIGHT = 14, NUM_BTN = 15 };
void set_button(int pad, int btn, bool down);
bool get_button(int pad, int btn);
void reset_key(bool down);   // console reset button
void power_off();            // slide power switch off
void broadcast_key(bool down, u32 scancode, u32 sym);
void set_attached(int pad, bool att);
bool power_on();
}  // namespace input

void hle_init(const std::string& rundir);
