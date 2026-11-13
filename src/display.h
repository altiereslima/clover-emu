// Host display: owns the SDL window, the emulated framebuffer (/dev/fb0) and presentation.
#pragma once
#include "common.h"
#include <atomic>
#include <functional>
#include <string>

struct SDL_Window;

namespace display {
struct Fb {
    int shm_fd = -1;
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
extern bool g_snes;  // Super NES Classic firmware (extra buttons, title)
// debug screenshots
extern std::string g_shot_dir;
extern std::atomic<bool> g_shot_req;
void write_bmp(const std::string& path, const u8* bgra_bottom_up, int w, int h);
}  // namespace display
