// Host window, framebuffer presentation, input pumping. Runs on the main thread.
#include "display.h"
#include "hle.h"
#include "kernel.h"
#include "launcher.h"
#include "ui.h"
#include <chrono>
#include <condition_variable>
#include <SDL.h>
#include "gl_api.h"
#include <algorithm>
#include <deque>
#include <future>

extern bool g_show_fps;

namespace display {
static Fb g_fb;
static SDL_Window* g_win = nullptr;
static SDL_GLContext g_ctx = nullptr;
static GLuint g_fbtex = 0;
static std::atomic<u64> g_last_gl{0};
static std::mutex g_q_mtx;
static std::deque<std::pair<std::function<void()>, std::promise<void>*>> g_q;
static SDL_threadID g_main_tid;
static bool g_quit_requested = false;
static u64 g_quit_time = 0;

Fb& fb() { return g_fb; }
std::string g_shot_dir;
bool g_snes = false;
std::atomic<bool> g_shot_req{false};
static int g_shot_n = 0;

void write_bmp(const std::string& path, const u8* px, int w, int h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    u32 datasz = w * h * 4;
    u8 hdr[54] = {'B', 'M'};
    u32 fsz = 54 + datasz, off = 54, hsz = 40, planes_bpp = 1 | (32 << 16);
    memcpy(hdr + 2, &fsz, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &hsz, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    memcpy(hdr + 34, &datasz, 4);
    fwrite(hdr, 1, 54, f);
    fwrite(px, 1, datasz, f);
    fclose(f);
}
static std::string next_shot_name() {
    char b[64];
    snprintf(b, sizeof b, "/shot%03d.bmp", g_shot_n++);
    return g_shot_dir + b;
}
std::string shot_name() { return next_shot_name(); }
SDL_Window* window() { return g_win; }

void init_fb() {
    g_fb.size = g_fb.xres * g_fb.yres_virtual * 4;
    // 64 KiB granularity: Windows maps shared views at allocation-granularity boundaries
    g_fb.size = (g_fb.size + 0xffff) & ~0xffffu;
    plat::Shm shm;
    if (!plat::shm_create(shm, g_fb.size)) fatal("cannot create framebuffer shared memory");
    g_fb.shm = shm.handle;
    g_fb.mem = shm.mem;
    memset(g_fb.mem, 0, g_fb.size);
}

void note_guest_gl_frame() { g_last_gl = now_ns(); }
bool guest_gl_active() { return now_ns() - g_last_gl.load() < 300000000ull; }
void note_fb_activity() { g_fb.last_write_ns = now_ns(); }

// ---------------------------------------------------------------- frame capture (in-game menu)
std::atomic<bool> g_capture_req{false};
static std::mutex g_cap_mtx;
static std::condition_variable g_cap_cv;
static std::vector<u8> g_cap;  // BGRA, bottom row first
static int g_cap_w = 0, g_cap_h = 0;
static bool g_cap_ready = false;

void deliver_capture(const u8* px, int w, int h) {
    std::lock_guard lk(g_cap_mtx);
    g_cap.assign(px, px + (size_t)w * h * 4);
    g_cap_w = w;
    g_cap_h = h;
    g_cap_ready = true;
    g_cap_cv.notify_all();
}

bool capture_frame(std::vector<u8>& rgba, int& w, int& h) {
    if (guest_gl_active()) {
        std::unique_lock lk(g_cap_mtx);
        g_cap_ready = false;
        g_capture_req = true;
        if (g_cap_cv.wait_for(lk, std::chrono::milliseconds(150), [] { return g_cap_ready; })) {
            w = g_cap_w;
            h = g_cap_h;
            rgba.resize((size_t)w * h * 4);
            for (int y = 0; y < h; y++) {
                const u8* s = &g_cap[(size_t)(h - 1 - y) * w * 4];
                u8* d = &rgba[(size_t)y * w * 4];
                for (int x = 0; x < w; x++, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
            }
            return true;
        }
        g_capture_req = false;
    }
    if (!g_fb.mem) return false;
    w = (int)g_fb.xres;
    h = (int)g_fb.yres;
    rgba.resize((size_t)w * h * 4);
    u32 yoff = g_fb.yoffset;
    for (int y = 0; y < h; y++) {
        const u8* s = g_fb.mem + (size_t)(yoff + y) * g_fb.xres * 4;
        u8* d = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
    }
    return true;
}

static std::atomic<u64> g_fb_flips{0};
static std::atomic<bool> g_flip_event_pending{false};
void note_fb_flip() {
    g_fb.last_write_ns = now_ns();
    g_fb_flips++;
    if (g_show_fps) {
        static u64 start = 0;
        static int n = 0;
        u64 now = now_ns();
        if (!start) start = now;
        if (++n, now - start >= 2000000000ull) {
            LOGI("[fps] fb0 flips: %.1f fps\n", n * 1e9 / (now - start));
            start = now;
            n = 0;
        }
    }
    if (!g_flip_event_pending.exchange(true)) {  // wake the main loop immediately
        SDL_Event e{};
        e.type = SDL_USEREVENT;
        e.user.code = 1;
        SDL_PushEvent(&e);
    }
}

// ---------------------------------------------------------------- frame pacing
// On a ~60 Hz display the console's vsync follows the host's real refresh: the window is presented
// with vsync and every presented frame is one "vblank" for the guest. Frames then reach the screen
// at an even cadence (a free-running 60 Hz timer drifts against the monitor, which shows up as
// tearing and frames held for one or two refreshes). Other refresh rates keep the timer.
static bool g_lock_vsync = false;
static std::mutex g_vbl_mtx;
static std::condition_variable g_vbl_cv;
static u64 g_vblank = 0;

bool host_vsync_locked() { return g_lock_vsync; }

static void note_vblank() {
    {
        std::lock_guard lk(g_vbl_mtx);
        g_vblank++;
    }
    g_vbl_cv.notify_all();
}

static void timer_tick() {
    constexpr u64 period = 16666667ull;
    static const u64 epoch = now_ns();
    u64 n = now_ns();
    u64 next = epoch + ((n - epoch) / period + 1) * period;
    while (true) {
        u64 cur = now_ns();
        if (cur >= next) return;
        u64 rem = next - cur;
        plat::sleep_ns(rem > 300000ull ? rem - 200000ull : rem);
    }
}

void wait_vsync_tick() {
    if (!g_lock_vsync || guest_gl_active()) {
        timer_tick();
        return;
    }
    // next host vblank (presented frame); fall back to the timer if presentation stalls
    // (minimised window, display asleep)
    static std::atomic<u64> last_tick{0};
    std::unique_lock lk(g_vbl_mtx);
    u64 seen = g_vblank;
    if (!g_vbl_cv.wait_for(lk, std::chrono::milliseconds(25), [&] { return g_vblank != seen; })) {
        lk.unlock();
        timer_tick();
    } else {
        lk.unlock();
    }
    // never faster than ~70 Hz, even if swaps stop blocking
    u64 now = now_ns(), prev = last_tick.exchange(now);
    if (prev && now - prev < 14000000ull) {
        plat::sleep_ns(14000000ull - (now - prev));
        last_tick = now_ns();
    }
}

void run_on_main(const std::function<void()>& fn) {
    if (SDL_ThreadID() == g_main_tid) {
        fn();
        return;
    }
    std::promise<void> pr;
    auto fut = pr.get_future();
    {
        std::lock_guard lk(g_q_mtx);
        g_q.push_back({fn, &pr});
    }
    SDL_Event e{};
    e.type = SDL_USEREVENT;
    SDL_PushEvent(&e);
    fut.wait();
}

static void drain_queue() {
    while (true) {
        std::pair<std::function<void()>, std::promise<void>*> item;
        {
            std::lock_guard lk(g_q_mtx);
            if (g_q.empty()) return;
            item = std::move(g_q.front());
            g_q.pop_front();
        }
        item.first();
        item.second->set_value();
    }
}

bool init_window(int scale, bool fullscreen) {
    g_main_tid = SDL_ThreadID();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    int w = 1280 * scale / 2, h = 720 * scale / 2;
    u32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_win = SDL_CreateWindow(ui::APP_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_win) {
        LOGI("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        LOGI("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    if (!gl_load()) LOGI("[display] some OpenGL functions are missing; graphics may not work\n");
    {
        SDL_DisplayMode dm{};
        int rr = SDL_GetCurrentDisplayMode(std::max(0, SDL_GetWindowDisplayIndex(g_win)), &dm) == 0 ? dm.refresh_rate : 0;
        const char* env = getenv("CLOVER_VSYNC");
        g_lock_vsync = env ? atoi(env) != 0 : (rr >= 59 && rr <= 61);
        LOGI("[display] %d Hz display: %s\n", rr, g_lock_vsync ? "console vsync follows the display" : "timer-paced 60 Hz");
    }
    // always present with vsync (no tearing); see wait_vsync_tick() for the pacing
    SDL_GL_SetSwapInterval(1);
    glGenTextures(1, &g_fbtex);
    glBindTexture(GL_TEXTURE_2D, g_fbtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_fb.xres, g_fb.yres, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(g_win);
    SDL_GL_MakeCurrent(g_win, nullptr);
    return true;
}

static void present_fb() {
    SDL_GL_MakeCurrent(g_win, g_ctx);
    int dw, dh;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g_shot_req.exchange(false)) {
        std::vector<u8> px(1280 * 720 * 4);
        const u8* src = g_fb.mem + (size_t)g_fb.yoffset * g_fb.xres * 4;
        for (int y = 0; y < 720; y++) memcpy(px.data() + (size_t)(719 - y) * 1280 * 4, src + (size_t)y * 1280 * 4, 1280 * 4);
        write_bmp(next_shot_name(), px.data(), 1280, 720);
    }
    if (!g_fb.blank) {
        glBindTexture(GL_TEXTURE_2D, g_fbtex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, g_fb.xres);
        const u8* src = g_fb.mem + (size_t)g_fb.yoffset * g_fb.xres * 4;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_fb.xres, g_fb.yres, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        int tw = dw, th = dw * 720 / 1280;
        if (th > dh) {
            th = dh;
            tw = dh * 1280 / 720;
        }
        int x0 = (dw - tw) / 2, y0 = (dh - th) / 2;
        glViewport(x0, y0, tw, th);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 1); glVertex2f(-1, -1);
        glTexCoord2f(1, 1); glVertex2f(1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, 1);
        glTexCoord2f(0, 0); glVertex2f(-1, 1);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
    SDL_GL_SwapWindow(g_win);
    SDL_GL_MakeCurrent(g_win, nullptr);
}

// Language-neutral progress bar shown while the firmware image is extracted on first run.
void drawable_size(int& w, int& h) { SDL_GL_GetDrawableSize(g_win, &w, &h); }

void present_rgba(const u8* px, int w, int h) {
    static GLuint tex = 0;
    static int tw = 0, th = 0;
    SDL_GL_MakeCurrent(g_win, g_ctx);
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != tw || h != th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        tw = w;
        th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fbtex);
    SDL_GL_SwapWindow(g_win);
    SDL_GL_MakeCurrent(g_win, nullptr);
}

// A negative value just clears the screen to black.
void draw_progress(float frac) {
    SDL_GL_MakeCurrent(g_win, g_ctx);
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (frac < 0) {
        SDL_GL_SwapWindow(g_win);
        SDL_GL_MakeCurrent(g_win, nullptr);
        return;
    }
    frac = std::min(frac, 1.0f);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    // bar geometry in pixels, converted to clip space
    float bw = dw * 0.4f, bh = std::max(8.0f, dh * 0.012f), x0 = (dw - bw) / 2, y0 = (dh - bh) / 2;
    auto quad = [&](float x, float y, float w, float h) {
        float l = x / dw * 2 - 1, r = (x + w) / dw * 2 - 1, b = y / dh * 2 - 1, t = (y + h) / dh * 2 - 1;
        glBegin(GL_QUADS);
        glVertex2f(l, b); glVertex2f(r, b); glVertex2f(r, t); glVertex2f(l, t);
        glEnd();
    };
    float pad = std::max(2.0f, bh * 0.25f);
    glColor4f(0.35f, 0.35f, 0.38f, 1);
    quad(x0 - pad, y0 - pad, bw + 2 * pad, bh + 2 * pad);
    glColor4f(0.08f, 0.08f, 0.09f, 1);
    quad(x0, y0, bw, bh);
    glColor4f(0.90f, 0.16f, 0.16f, 1);
    quad(x0, y0, bw * frac, bh);
    SDL_GL_SwapWindow(g_win);
    SDL_GL_MakeCurrent(g_win, nullptr);
}

// ---------------------------------------------------------------- input mapping
static SDL_GameController* g_pads[input::NUM_PADS] = {};

// Several host sources (keyboard, controller buttons, analog stick) can drive the same guest
// button; the guest sees it pressed while any source holds it, so e.g. centering the stick does
// not release a D-pad direction that is still held.
enum Src : u8 { SRC_KEY = 1, SRC_BTN = 2, SRC_STICK = 4 };
static u8 g_src[input::NUM_PADS][input::NUM_BTN];
static bool g_combo[input::NUM_PADS];  // Select+Start held on a controller -> pause menu
static bool g_menu_req = false;         // open the pause menu at the next main-loop turn
static u64 g_reset_release_at = 0;      // "game list" from the menu: RESET press, released later

static void press(int pad, int btn, Src s, bool down) {
    u8& m = g_src[pad][btn];
    m = down ? (u8)(m | s) : (u8)(m & ~s);
    input::set_button(pad, btn, m != 0);
}

// Drop everything a controller source holds on `pad` (controller unplugged or moved to another slot).
static void release_pad(int pad) {
    for (int b = 0; b < input::NUM_BTN; b++)
        if (g_src[pad][b] & (SRC_BTN | SRC_STICK)) press(pad, b, Src(SRC_BTN | SRC_STICK), false);
    g_combo[pad] = false;
}

// Everything up, from every source (before the pause menu takes over the input).
static void release_all() {
    for (int p = 0; p < input::NUM_PADS; p++) {
        for (int b = 0; b < input::NUM_BTN; b++)
            if (g_src[p][b]) press(p, b, Src(SRC_KEY | SRC_BTN | SRC_STICK), false);
        g_combo[p] = false;
    }
    input::reset_key(false);
}

// Optional SDL mapping database for controllers SDL does not know out of the box:
// https://github.com/mdqinc/SDL_GameControllerDB (gamecontrollerdb.txt next to the exe or in the
// current folder). SDL_GAMECONTROLLERCONFIG in the environment is honored by SDL itself.
static void load_pad_mappings() {
    std::vector<std::string> dirs = {""};
    if (char* bp = SDL_GetBasePath()) {
        dirs.push_back(bp);
        SDL_free(bp);
    }
    for (auto& d : dirs) {
        std::string f = d + "gamecontrollerdb.txt";
        int n = SDL_GameControllerAddMappingsFromFile(f.c_str());
        if (n > 0) LOGI("[input] loaded %d controller mappings from %s\n", n, f.c_str());
    }
}

static const char* pad_kind(SDL_GameController* p) {
    switch (SDL_GameControllerGetType(p)) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360 / XInput";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One / XInput";
    case SDL_CONTROLLER_TYPE_PS3: return "PS3";
    case SDL_CONTROLLER_TYPE_PS4: return "PS4";
    case SDL_CONTROLLER_TYPE_PS5: return "PS5";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    default: return "generic";
    }
}

static void refresh_pads() {
    for (int i = 0; i < input::NUM_PADS; i++)
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i])) {
            LOGI("[input] host controller '%s' disconnected\n", SDL_GameControllerName(g_pads[i]));
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = nullptr;
            release_pad(i);
        }
    // keep controllers packed from player 1 so a lone controller is never stranded on player 2
    for (int i = 0; i + 1 < input::NUM_PADS; i++)
        if (!g_pads[i])
            for (int j = i + 1; j < input::NUM_PADS; j++)
                if (g_pads[j]) {
                    release_pad(j);
                    g_pads[i] = g_pads[j];
                    g_pads[j] = nullptr;
                    break;
                }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool have = false;
        for (auto* p : g_pads)
            if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) have = true;
        if (have) continue;
        for (auto& p : g_pads)
            if (!p) {
                p = SDL_GameControllerOpen(i);
                if (p) {
                    const char* path = SDL_GameControllerPath(p);
                    LOGI("[input] host controller '%s' (%s, %s) connected as player %d\n", SDL_GameControllerName(p),
                         pad_kind(p), path ? path : "?", int(&p - g_pads) + 1);
                }
                break;
            }
    }
    int n = 0;
    for (auto* p : g_pads)
        if (p) n++;
    // Pad 1 always present (keyboard); pad 2 when a second host controller exists.
    input::set_attached(1, n >= 2);
}

static int pad_of(SDL_JoystickID id) {
    for (int i = 0; i < input::NUM_PADS; i++)
        if (g_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) return i;
    return -1;
}

void toggle_fullscreen() {
    bool fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(g_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
}

static void handle_key(const SDL_KeyboardEvent& k) {
    bool down = k.type == SDL_KEYDOWN;
    if (k.repeat) return;
    using namespace input;
    auto kb = [&](int btn) { press(0, btn, SRC_KEY, down); };
    switch (k.keysym.scancode) {
    case SDL_SCANCODE_UP: kb(DUP); break;
    case SDL_SCANCODE_DOWN: kb(DDOWN); break;
    case SDL_SCANCODE_LEFT: kb(DLEFT); break;
    case SDL_SCANCODE_RIGHT: kb(DRIGHT); break;
    case SDL_SCANCODE_X: case SDL_SCANCODE_K: kb(A); break;
    case SDL_SCANCODE_Z: case SDL_SCANCODE_J: kb(B); break;
    case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_TAB: case SDL_SCANCODE_BACKSPACE: kb(BACK); break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
        if (down && (k.keysym.mod & KMOD_ALT)) toggle_fullscreen();  // Alt+Enter is not a START press
        else kb(START);
        break;
    case SDL_SCANCODE_S: if (g_snes) kb(X); break;
    case SDL_SCANCODE_A: if (g_snes) kb(Y); break;
    case SDL_SCANCODE_Q: if (g_snes) kb(LSHOULDER); break;
    case SDL_SCANCODE_W: if (g_snes) kb(RSHOULDER); break;
    case SDL_SCANCODE_ESCAPE: if (down) g_menu_req = true; break;
    case SDL_SCANCODE_HOME: case SDL_SCANCODE_R: reset_key(down); break;
    case SDL_SCANCODE_F11:
        if (down) toggle_fullscreen();
        break;
    default: break;
    }
}

static void handle_cbutton(const SDL_ControllerButtonEvent& b) {
    int pad = pad_of(b.which);
    if (pad < 0) return;
    bool down = b.state == SDL_PRESSED;
    using namespace input;
    auto pb = [&](int btn) { press(pad, btn, SRC_BTN, down); };
    // Select+Start together = pause menu (resume / game list / fullscreen / turn off). The Xbox
    // Guide button is usually taken by the Windows Game Bar, so this is the pad's way out.
    if (b.button == SDL_CONTROLLER_BUTTON_BACK || b.button == SDL_CONTROLLER_BUTTON_START) {
        SDL_GameController* gc = g_pads[pad];
        bool both = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK) &&
                    SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START);
        if (both && !g_combo[pad]) {
            g_combo[pad] = true;
            g_menu_req = true;
            return;  // the second button of the combo never reaches the game
        }
        if (!both) g_combo[pad] = false;
    }
    if (g_snes) {
        // Nintendo layout by position: right=A, bottom=B, top=X, left=Y
        switch (b.button) {
        case SDL_CONTROLLER_BUTTON_B: pb(A); return;
        case SDL_CONTROLLER_BUTTON_A: pb(B); return;
        case SDL_CONTROLLER_BUTTON_Y: pb(X); return;
        case SDL_CONTROLLER_BUTTON_X: pb(Y); return;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: pb(LSHOULDER); return;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: pb(RSHOULDER); return;
        default: break;
        }
    }
    switch (b.button) {
    // NES: right / left face buttons = A, bottom / top = B
    case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_X: pb(A); break;
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_Y: pb(B); break;
    case SDL_CONTROLLER_BUTTON_BACK: pb(BACK); break;
    case SDL_CONTROLLER_BUTTON_START: pb(START); break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: pb(DUP); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: pb(DDOWN); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: pb(DLEFT); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: pb(DRIGHT); break;
    case SDL_CONTROLLER_BUTTON_GUIDE: reset_key(down); break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (!g_snes) reset_key(down); break;
    default: break;
    }
}

static void handle_axis(const SDL_ControllerAxisEvent& ax) {
    int pad = pad_of(ax.which);
    if (pad < 0) return;
    const int th = 16000;
    using namespace input;
    if (ax.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        press(pad, DLEFT, SRC_STICK, ax.value < -th);
        press(pad, DRIGHT, SRC_STICK, ax.value > th);
    } else if (ax.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        press(pad, DUP, SRC_STICK, ax.value < -th);
        press(pad, DDOWN, SRC_STICK, ax.value > th);
    }
}

void request_quit() {
    if (g_quit_requested) return;
    g_quit_requested = true;
    g_quit_time = now_ns();
    LOGI("[display] window closed: switching console power off\n");
    input::power_off();
}

// Pause the guest and show the in-game menu over its last frame.
static void open_pause_menu() {
    g_menu_req = false;
    release_all();
    bool was_gl = guest_gl_active();
    std::vector<u8> frame;
    int fw = 0, fh = 0;
    capture_frame(frame, fw, fh);
    g_kernel.set_paused(true);
    SDL_Delay(20);  // let a guest frame that is mid-swap finish
    LOGI("[display] pause menu opened\n");
    int act = launcher::pause_menu(frame, fw, fh, [] { drain_queue(); });
    g_kernel.set_paused(false);
    LOGI("[display] pause menu closed (action %d)\n", act);
    if (was_gl) note_guest_gl_frame();  // don't let a stale fb0 picture flash before the next GL frame
    if (act == launcher::MENU_GAME_LIST) {
        input::reset_key(true);
        g_reset_release_at = now_ns() + 150000000ull;
    } else if (act == launcher::MENU_QUIT) {
        request_quit();
    }
    SDL_ShowCursor((SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? SDL_DISABLE : SDL_ENABLE);
}

void main_loop() {
    load_pad_mappings();
    refresh_pads();
    u64 last_present = 0, last_shot = 0, seen_flips = 0;
    while (!g_kernel.shutting_down) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 4)) {
            g_flip_event_pending = false;
            do {
                switch (e.type) {
                case SDL_QUIT: request_quit(); break;
                case SDL_KEYDOWN: case SDL_KEYUP: handle_key(e.key); break;
                case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: handle_cbutton(e.cbutton); break;
                case SDL_CONTROLLERAXISMOTION: handle_axis(e.caxis); break;
                case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: refresh_pads(); break;
                default: break;
                }
            } while (SDL_PollEvent(&e));
        }
        drain_queue();
        if (g_menu_req) {
            if (g_quit_requested) g_menu_req = false;
            else open_pause_menu();
        }
        if (g_reset_release_at && now_ns() >= g_reset_release_at) {
            g_reset_release_at = 0;
            input::reset_key(false);
        }
        if (g_quit_requested && now_ns() - g_quit_time > 20000000000ull) {
            LOGI("[display] guest did not power off in time; exiting\n");
            break;
        }
        u64 n = now_ns();
        if (!g_shot_dir.empty() && n - last_shot > 2000000000ull) {
            g_shot_req = true;
            last_shot = n;
        }
        u64 flips = g_fb_flips.load();
        if (!guest_gl_active()) {
            if (g_lock_vsync) {
                // present every refresh (the swap blocks until vblank) and tick the guest's vsync
                seen_flips = flips;
                present_fb();
                last_present = n;
                note_vblank();
            } else if (flips != seen_flips || n - last_present > 33000000ull) {
                seen_flips = flips;
                present_fb();
                last_present = n;
            }
        }
    }
    drain_queue();
}
}  // namespace display

