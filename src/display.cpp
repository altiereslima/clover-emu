// Host window, framebuffer presentation, input pumping. Runs on the main thread.
#define GL_SILENCE_DEPRECATION
#include "display.h"
#include "hle.h"
#include "kernel.h"
#include <SDL.h>
#include <OpenGL/gl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
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
    u32 hp = host_page_size();
    g_fb.size = (g_fb.size + hp - 1) & ~(hp - 1);
    std::string path = g_kernel.rundir + "/fb0.XXXXXX";
    std::vector<char> buf(path.begin(), path.end());
    buf.push_back(0);
    int fd = mkstemp(buf.data());
    if (fd < 0) fatal("cannot create framebuffer backing file");
    unlink(buf.data());
    if (ftruncate(fd, g_fb.size) != 0) fatal("ftruncate fb");
    void* p = mmap(nullptr, g_fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) fatal("mmap fb");
    g_fb.shm_fd = fd;
    g_fb.mem = (u8*)p;
    memset(p, 0, g_fb.size);
}

void note_guest_gl_frame() { g_last_gl = now_ns(); }
bool guest_gl_active() { return now_ns() - g_last_gl.load() < 300000000ull; }
void note_fb_activity() { g_fb.last_write_ns = now_ns(); }

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

void wait_vsync_tick() {
    constexpr u64 period = 16666667ull;
    static const u64 epoch = now_ns();
    u64 n = now_ns();
    u64 next = epoch + ((n - epoch) / period + 1) * period;
    while (true) {
        u64 cur = now_ns();
        if (cur >= next) return;
        u64 rem = next - cur;
        timespec ts{0, (long)(rem > 300000ull ? rem - 200000ull : rem)};
        nanosleep(&ts, nullptr);
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
    g_win = SDL_CreateWindow("clover-emu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_win) {
        LOGI("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        LOGI("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_GL_SetSwapInterval(0);  // presentation is paced by the guest's page flips
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

// ---------------------------------------------------------------- input mapping
static SDL_GameController* g_pads[input::NUM_PADS] = {};

static void refresh_pads() {
    for (auto& p : g_pads)
        if (p && !SDL_GameControllerGetAttached(p)) {
            SDL_GameControllerClose(p);
            p = nullptr;
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
                LOGI("[input] host controller '%s' connected\n", SDL_GameControllerName(p));
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

static void handle_key(const SDL_KeyboardEvent& k) {
    bool down = k.type == SDL_KEYDOWN;
    if (k.repeat) return;
    using namespace input;
    switch (k.keysym.scancode) {
    case SDL_SCANCODE_UP: set_button(0, DUP, down); break;
    case SDL_SCANCODE_DOWN: set_button(0, DDOWN, down); break;
    case SDL_SCANCODE_LEFT: set_button(0, DLEFT, down); break;
    case SDL_SCANCODE_RIGHT: set_button(0, DRIGHT, down); break;
    case SDL_SCANCODE_X: case SDL_SCANCODE_K: set_button(0, A, down); break;
    case SDL_SCANCODE_Z: case SDL_SCANCODE_J: set_button(0, B, down); break;
    case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_TAB: case SDL_SCANCODE_BACKSPACE: set_button(0, BACK, down); break;
    case SDL_SCANCODE_RETURN: set_button(0, START, down); break;
    case SDL_SCANCODE_S: if (g_snes) set_button(0, X, down); break;
    case SDL_SCANCODE_A: if (g_snes) set_button(0, Y, down); break;
    case SDL_SCANCODE_Q: if (g_snes) set_button(0, LSHOULDER, down); break;
    case SDL_SCANCODE_W: if (g_snes) set_button(0, RSHOULDER, down); break;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_HOME: case SDL_SCANCODE_R: reset_key(down); break;
    case SDL_SCANCODE_F11:
        if (down) {
            bool fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(g_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        break;
    default: break;
    }
}

static void handle_cbutton(const SDL_ControllerButtonEvent& b) {
    int pad = pad_of(b.which);
    if (pad < 0) return;
    bool down = b.state == SDL_PRESSED;
    using namespace input;
    if (g_snes) {
        // Nintendo layout by position: right=A, bottom=B, top=X, left=Y
        switch (b.button) {
        case SDL_CONTROLLER_BUTTON_B: set_button(pad, A, down); return;
        case SDL_CONTROLLER_BUTTON_A: set_button(pad, B, down); return;
        case SDL_CONTROLLER_BUTTON_Y: set_button(pad, X, down); return;
        case SDL_CONTROLLER_BUTTON_X: set_button(pad, Y, down); return;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: set_button(pad, LSHOULDER, down); return;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_button(pad, RSHOULDER, down); return;
        default: break;
        }
    }
    switch (b.button) {
    case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_Y: set_button(pad, A, down); break;
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_X: set_button(pad, B, down); break;
    case SDL_CONTROLLER_BUTTON_BACK: set_button(pad, BACK, down); break;
    case SDL_CONTROLLER_BUTTON_START: set_button(pad, START, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: set_button(pad, DUP, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: set_button(pad, DDOWN, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: set_button(pad, DLEFT, down); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: set_button(pad, DRIGHT, down); break;
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
        set_button(pad, DLEFT, ax.value < -th);
        set_button(pad, DRIGHT, ax.value > th);
    } else if (ax.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        set_button(pad, DUP, ax.value < -th);
        set_button(pad, DDOWN, ax.value > th);
    }
}

void request_quit() {
    if (g_quit_requested) return;
    g_quit_requested = true;
    g_quit_time = now_ns();
    LOGI("[display] window closed: switching console power off\n");
    input::power_off();
}

void main_loop() {
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
        if (!guest_gl_active() && (flips != seen_flips || n - last_present > 33000000ull)) {
            seen_flips = flips;
            present_fb();
            last_present = n;
        }
    }
    drain_queue();
}
}  // namespace display

