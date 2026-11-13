// Host display: owns the SDL window, the emulated framebuffer (/dev/fb0) and presentation.
#pragma once
#include "common.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace display {
struct Fb {
    intptr_t shm = -1;  // shared memory object (plat::Shm handle)
    u8* mem = nullptr;
    u32 size = 0;
    u32 xres = 1280, yres = 720, yres_virtual = 1440;
    std::atomic<u32> yoffset{0};
    std::atomic<u64> last_write_ns{0};  // last pan/ioctl activity
    std::atomic<bool> blank{false};
};
Fb& fb();
void init_fb();

// Main-thread side
bool init_window(int scale, bool fullscreen);
void main_loop();  // runs until shutdown
void draw_progress(float frac);  // first-run "preparing firmware" bar (main thread)
// Show a CPU-rendered RGBA8 image (w x h, top row first) stretched over the whole window.
void present_rgba(const u8* px, int w, int h);
void drawable_size(int& w, int& h);
void toggle_fullscreen();  // F11 / Alt+Enter
// In-game menu background: the guest GL thread hands its next frame over when asked.
extern std::atomic<bool> g_capture_req;
void deliver_capture(const u8* bgra_bottom_up, int w, int h);
// Current picture as RGBA, top row first (last guest GL frame, else /dev/fb0).
bool capture_frame(std::vector<u8>& rgba, int& w, int& h);
void request_quit();

// Called from any thread: run fn on main thread and wait.
void run_on_main(const std::function<void()>& fn);

SDL_Window* window();
// GL presentation ownership: guest GL contexts report activity so the FB presenter backs off.
void note_guest_gl_frame();
bool guest_gl_active();
void note_fb_activity();
void note_fb_flip();       // a new framebuffer page was flipped in: present it now
void wait_vsync_tick();    // block until the next 60 Hz refresh (FBIO_WAITFORVSYNC)
bool host_vsync_locked();  // ~60 Hz display: guest vsync follows the host's presented frames
extern bool g_snes;  // Super NES Classic firmware (extra buttons)
// debug screenshots
extern std::string g_shot_dir;
extern std::atomic<bool> g_shot_req;
void write_bmp(const std::string& path, const u8* bgra_bottom_up, int w, int h);
}  // namespace display
