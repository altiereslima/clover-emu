// HLE of OpenGL ES 2.0 (libMali) on top of macOS legacy OpenGL 2.1.
#define GL_SILENCE_DEPRECATION
#include "hle.h"
#include "hle_gl.h"
#include "display.h"
#include <SDL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <regex>
#include <map>
#include <mutex>
#include <time.h>

static constexpr int VW = 1280, VH = 720;
static thread_local GLCtx* t_cur = nullptr;

GLCtx* gl_current() { return t_cur; }

std::shared_ptr<GLCtx> gl_create_context() {
    auto c = std::make_shared<GLCtx>();
    display::run_on_main([&] {
        SDL_Window* w = display::window();
        SDL_GLContext prev = SDL_GL_GetCurrentContext();
        c->host = SDL_GL_CreateContext(w);
        if (!c->host) LOGI("[gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_GL_MakeCurrent(w, prev);
    });
    if (!c->host) return nullptr;
    // A new app is taking over the display: drop the previous app's last frame from fb0 so it
    // doesn't flash up again while this app starts.
    auto& fb = display::fb();
    if (fb.mem) memset(fb.mem, 0, fb.size);
    return c;
}

static void init_ctx(GLCtx* c) {
    glGenTextures(1, &c->vfb_tex);
    glBindTexture(GL_TEXTURE_2D, c->vfb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VW, VH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffersEXT(1, &c->vfb_depth);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, c->vfb_depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, VW, VH);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
    glGenFramebuffersEXT(1, &c->vfb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->vfb);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, c->vfb_tex, 0);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->vfb_depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, c->vfb_depth);
    GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) LOGI("[gl] virtual framebuffer incomplete: %x\n", st);
    glViewport(0, 0, VW, VH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    c->cur_fbo = 0;
    c->initialized = true;
}

void gl_make_current(GLCtx* c) {
    SDL_Window* w = display::window();
    if (!c) {
        SDL_GL_MakeCurrent(w, nullptr);
        t_cur = nullptr;
        return;
    }
    if (SDL_GL_MakeCurrent(w, (SDL_GLContext)c->host) != 0) LOGI("[gl] MakeCurrent failed: %s\n", SDL_GetError());
    t_cur = c;
    if (!c->initialized) {
        init_ctx(c);
        SDL_GL_SetSwapInterval(0);  // vsync is emulated in gl_swap()
    }
}

void gl_set_swap_interval(int i) {
    if (t_cur) t_cur->swap_interval = i;
}

// Emulated 60 Hz vsync: like Mali fbdev's eglSwapBuffers, block until the next refresh tick.
// (SDL's own vsync from a background thread on macOS can wait an extra frame and fights the
// firmware's own frame pacing.)
static void wait_vsync(GLCtx* c) {
    constexpr u64 period = 16666667ull;
    u64 n = now_ns();
    // Next "refresh" is one period after the previous one; if the app fell behind, re-sync to now
    // instead of waiting for a later tick (avoids dropping to 30 fps on small hiccups).
    u64 next = c->last_vsync + period;
    if (!c->last_vsync || n > next + period / 2) {
        c->last_vsync = n;
        return;
    }
    if (n >= next) {
        c->last_vsync = next;
        return;
    }
    while (true) {
        u64 cur = now_ns();
        if (cur >= next) break;
        u64 rem = next - cur;
        if (rem > 1500000ull) {
            timespec ts{0, (long)(rem - 1000000ull)};
            nanosleep(&ts, nullptr);
        }
    }
    c->last_vsync = next;
}

bool g_show_fps = false;
static void count_fps() {
    if (!g_show_fps) return;
    static std::mutex m;
    static std::map<void*, std::pair<u64, int>> st;  // per context: window start, frames
    std::lock_guard lk(m);
    auto& [start, n] = st[t_cur];
    u64 now = now_ns();
    if (!start) start = now;
    n++;
    if (now - start >= 2000000000ull) {
        LOGI("[fps] ctx %p: %.1f fps\n", (void*)t_cur, n * 1e9 / (now - start));
        start = now;
        n = 0;
    }
}

// Copy the virtual framebuffer into /dev/fb0 (Mali renders straight into fb0 on the real console;
// with MALI_NOCLEAR the last frame stays visible after the app exits).
static void readback_to_fb(GLCtx* c) {
    auto& fb = display::fb();
    if (!fb.mem) return;
    std::vector<u8> px(VW * VH * 4);
    GLint prev_read;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_read);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, c->vfb);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, VW, VH, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px.data());
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, prev_read);
    u32 yoff = fb.yoffset;
    for (int y = 0; y < VH; y++) {
        u8* dst = fb.mem + (size_t)(yoff + y) * fb.xres * 4;
        memcpy(dst, px.data() + (size_t)(VH - 1 - y) * VW * 4, VW * 4);
    }
}

void gl_destroy_context(GLCtx* c) {
    if (!c || !c->host) return;
    if (t_cur == c) {
        if (c->initialized && c->frames) readback_to_fb(c);
        SDL_GL_MakeCurrent(display::window(), nullptr);
        t_cur = nullptr;
    } else if (c->initialized && c->frames) {
        display::run_on_main([&] {
            SDL_GLContext prev = SDL_GL_GetCurrentContext();
            SDL_GL_MakeCurrent(display::window(), (SDL_GLContext)c->host);
            readback_to_fb(c);
            SDL_GL_MakeCurrent(display::window(), prev);
        });
    }
    display::note_fb_activity();
    void* h = c->host;
    c->host = nullptr;
    display::run_on_main([h] { SDL_GL_DeleteContext((SDL_GLContext)h); });
}

void gl_swap() {
    GLCtx* c = t_cur;
    if (!c) return;
    SDL_Window* w = display::window();
    int dw, dh;
    SDL_GL_GetDrawableSize(w, &dw, &dh);
    // save state
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint draw_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &draw_fbo);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean cmask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // letterbox 16:9
    int tw = dw, th = dw * VH / VW;
    if (th > dh) {
        th = dh;
        tw = dh * VW / VH;
    }
    int x0 = (dw - tw) / 2, y0 = (dh - th) / 2;
    bool integer = (tw % VW == 0) && (th % VH == 0);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, c->vfb);
    if (display::g_shot_req.exchange(false)) {
        std::vector<u8> px(VW * VH * 4);
        glReadPixels(0, 0, VW, VH, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px.data());
        extern std::string shot_name_fwd();
        display::write_bmp(shot_name_fwd(), px.data(), VW, VH);
    }
    glBlitFramebufferEXT(0, 0, VW, VH, x0, y0, x0 + tw, y0 + th, GL_COLOR_BUFFER_BIT, integer ? GL_NEAREST : GL_LINEAR);
    SDL_GL_SwapWindow(w);
    // restore
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, draw_fbo);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    c->frames++;
    display::note_guest_gl_frame();
    count_fps();
    if (c->swap_interval > 0) wait_vsync(c);  // eglSwapBuffers on Mali blocks until the next refresh
}

namespace display { std::string shot_name(); }
std::string shot_name_fwd() { return display::shot_name(); }

void gl_create_target(int w, int h, u32* tex, u32* fbo) {
    GLint prev_tex, prev_fbo;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fbo);
    GLuint t, f;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffersEXT(1, &f);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, f);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, t, 0);
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, prev_fbo);
    *tex = t;
    *fbo = f;
}

void gl_destroy_target(u32 tex, u32 fbo) {
    GLuint t = tex, f = fbo;
    if (f) glDeleteFramebuffersEXT(1, &f);
    if (t) glDeleteTextures(1, &t);
}

void gl_bind_target(u32 fbo, int w, int h) {
    GLCtx* c = t_cur;
    if (!c) return;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo ? fbo : c->vfb);
    c->cur_fbo = fbo;
    glViewport(0, 0, w, h);
}

void gl_unbind_texture() { glBindTexture(GL_TEXTURE_2D, 0); }

void gl_upload(u32 tex, int x, int y, int w, int h, int row_len, const GLFormat& f, const void* data) {
    GLint prev, prev_unit, prev_align, prev_row;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
    GLint prev_buf;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_buf);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, f.format, f.type, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prev_buf);
    glBindTexture(GL_TEXTURE_2D, prev);
    glActiveTexture(prev_unit);
}

static float g_draw_color[4] = {0, 0, 0, 1};
void gl_set_draw_color(float r, float g, float b, float a) {
    g_draw_color[0] = r;
    g_draw_color[1] = g;
    g_draw_color[2] = b;
    g_draw_color[3] = a;
}
void gl_render_clear() {
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(g_draw_color[0], g_draw_color[1], g_draw_color[2], g_draw_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

// SDL_RenderCopy: textured quad with fixed-function GL, preserving the guest's GL state.
void gl_render_copy(u32 tex, int tw, int th, const int src[4], const int dst[4], int vw, int vh, bool blend) {
    GLint prog, unit, arraybuf;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arraybuf);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_VIEWPORT_BIT | GL_CURRENT_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glViewport(0, 0, vw, vh);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, vw, vh, 0, -1, 1);  // SDL: top-left origin
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    float u0 = (float)src[0] / tw, v0 = (float)src[1] / th, u1 = (float)(src[0] + src[2]) / tw, v1 = (float)(src[1] + src[3]) / th;
    float x0 = dst[0], y0 = dst[1], x1 = dst[0] + dst[2], y1 = dst[1] + dst[3];
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopClientAttrib();
    glPopAttrib();
    glBindBuffer(GL_ARRAY_BUFFER, arraybuf);
    glActiveTexture(unit);
    glUseProgram(prog);
}

// ---------------------------------------------------------------- shader translation
static std::string translate_shader(const std::string& src, GLenum type) {
    std::string s = src;
    // strip #version and precision statements, GLES-only extensions
    s = std::regex_replace(s, std::regex(R"(#\s*version[^\n]*)"), "");
    s = std::regex_replace(s, std::regex(R"(\bprecision\s+(lowp|mediump|highp)\s+\w+\s*;)"), "");
    s = std::regex_replace(s, std::regex(R"(#\s*extension\s+GL_OES_[^\n]*)"), "");
    std::string hdr = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    return hdr + s;
}

// ---------------------------------------------------------------- GL entry points
#define P(i) a.p(i)
static inline void* cptr(HleArgs& a, int i) { return a.p(i); }

HLE(LIB_MALI, glActiveTexture) { glActiveTexture(a.u(0)); }
HLE(LIB_MALI, glAttachShader) { glAttachShader(a.u(0), a.u(1)); }
HLE(LIB_MALI, glBindAttribLocation) { glBindAttribLocation(a.u(0), a.u(1), (const char*)P(2)); }
HLE(LIB_MALI, glBindBuffer) {
    if (a.u(0) == GL_ARRAY_BUFFER && t_cur) t_cur->array_buffer = a.u(1);
    glBindBuffer(a.u(0), a.u(1));
}
HLE(LIB_MALI, glBindFramebuffer) {
    GLCtx* c = t_cur;
    u32 f = a.u(1);
    if (c) c->cur_fbo = f;
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, f == 0 && c ? c->vfb : f);
}
HLE(LIB_MALI, glBindRenderbuffer) { glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, a.u(1)); }
HLE(LIB_MALI, glBindTexture) { glBindTexture(a.u(0), a.u(1)); }
HLE(LIB_MALI, glBlendEquation) { glBlendEquation(a.u(0)); }
HLE(LIB_MALI, glBlendFunc) { glBlendFunc(a.u(0), a.u(1)); }
HLE(LIB_MALI, glBlendFuncSeparate) { glBlendFuncSeparate(a.u(0), a.u(1), a.u(2), a.u(3)); }
HLE(LIB_MALI, glBufferData) { glBufferData(a.u(0), (GLsizeiptr)a.s(1), cptr(a, 2), a.u(3)); }
HLE(LIB_MALI, glBufferSubData) { glBufferSubData(a.u(0), (GLintptr)a.s(1), (GLsizeiptr)a.s(2), cptr(a, 3)); }
HLE(LIB_MALI, glCheckFramebufferStatus) { a.ret(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)); }
HLE(LIB_MALI, glClear) { glClear(a.u(0)); }
HLE(LIB_MALI, glClearColor) { glClearColor(a.f(0), a.f(1), a.f(2), a.f(3)); }
HLE(LIB_MALI, glClearDepthf) { glClearDepth(a.f(0)); }
HLE(LIB_MALI, glClearStencil) { glClearStencil(a.s(0)); }
HLE(LIB_MALI, glColorMask) { glColorMask(a.u(0), a.u(1), a.u(2), a.u(3)); }
HLE(LIB_MALI, glCompileShader) {
    glCompileShader(a.u(0));
    GLint ok = 0;
    glGetShaderiv(a.u(0), GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(a.u(0), sizeof log, nullptr, log);
        LOGI("[gl] shader %u compile failed: %s\n", a.u(0), log);
    }
}
HLE(LIB_MALI, glCreateProgram) { a.ret(glCreateProgram()); }
HLE(LIB_MALI, glCreateShader) { a.ret(glCreateShader(a.u(0))); }
HLE(LIB_MALI, glCullFace) { glCullFace(a.u(0)); }
HLE(LIB_MALI, glDeleteBuffers) { glDeleteBuffers(a.s(0), (const GLuint*)P(1)); }
HLE(LIB_MALI, glDeleteFramebuffers) {
    std::vector<GLuint> v((const GLuint*)P(1), (const GLuint*)P(1) + a.s(0));
    for (auto& x : v)
        if (t_cur && x == t_cur->vfb) x = 0;
    glDeleteFramebuffersEXT((GLsizei)v.size(), v.data());
}
HLE(LIB_MALI, glDeleteProgram) { glDeleteProgram(a.u(0)); }
HLE(LIB_MALI, glDeleteRenderbuffers) { glDeleteRenderbuffersEXT(a.s(0), (const GLuint*)P(1)); }
HLE(LIB_MALI, glDeleteShader) { glDeleteShader(a.u(0)); }
HLE(LIB_MALI, glDeleteTextures) { glDeleteTextures(a.s(0), (const GLuint*)P(1)); }
HLE(LIB_MALI, glDepthFunc) { glDepthFunc(a.u(0)); }
HLE(LIB_MALI, glDepthMask) { glDepthMask(a.u(0)); }
HLE(LIB_MALI, glDisable) { glDisable(a.u(0)); }
HLE(LIB_MALI, glDisableVertexAttribArray) { glDisableVertexAttribArray(a.u(0)); }
HLE(LIB_MALI, glDrawArrays) { glDrawArrays(a.u(0), a.s(1), a.s(2)); }
HLE(LIB_MALI, glDrawElements) {
    GLint eab = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eab);
    glDrawElements(a.u(0), a.s(1), a.u(2), eab ? (void*)(uintptr_t)a.u(3) : cptr(a, 3));
}
HLE(LIB_MALI, glEnable) { glEnable(a.u(0)); }
HLE(LIB_MALI, glEnableVertexAttribArray) { glEnableVertexAttribArray(a.u(0)); }
HLE(LIB_MALI, glFinish) { glFinish(); }
HLE(LIB_MALI, glFlush) { glFlush(); }
HLE(LIB_MALI, glFramebufferRenderbuffer) { glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, a.u(1), GL_RENDERBUFFER_EXT, a.u(3)); }
HLE(LIB_MALI, glFramebufferTexture2D) { glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, a.u(1), a.u(2), a.u(3), a.s(4)); }
HLE(LIB_MALI, glGenBuffers) { glGenBuffers(a.s(0), (GLuint*)P(1)); }
HLE(LIB_MALI, glGenFramebuffers) { glGenFramebuffersEXT(a.s(0), (GLuint*)P(1)); }
HLE(LIB_MALI, glGenRenderbuffers) { glGenRenderbuffersEXT(a.s(0), (GLuint*)P(1)); }
HLE(LIB_MALI, glGenTextures) { glGenTextures(a.s(0), (GLuint*)P(1)); }
HLE(LIB_MALI, glGenerateMipmap) { glGenerateMipmapEXT(a.u(0)); }
HLE(LIB_MALI, glGetAttribLocation) { a.ret(glGetAttribLocation(a.u(0), (const char*)P(1))); }
HLE(LIB_MALI, glGetError) {
    GLCtx* c = t_cur;
    if (c && c->pending_error) {
        a.ret(c->pending_error);
        c->pending_error = 0;
        return;
    }
    GLenum e = glGetError();
    a.ret(e);
}
HLE(LIB_MALI, glGetIntegerv) {
    GLenum pname = a.u(0);
    GLint* out = (GLint*)P(1);
    GLCtx* c = t_cur;
    switch (pname) {
    case 0x8CA6:  // GL_FRAMEBUFFER_BINDING
        *out = c ? (GLint)c->cur_fbo : 0;
        return;
    case 0x87FE: case 0x8DF9: *out = 0; return;  // NUM_PROGRAM_BINARY_FORMATS_OES / NUM_SHADER_BINARY_FORMATS
    case 0x87FF: case 0x8DF8: return;
    case 0x8DFA: *out = 1; return;               // SHADER_COMPILER
    case 0x8DFB: *out = 256; return;             // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFC: *out = 12; return;              // MAX_VARYING_VECTORS
    case 0x8DFD: *out = 256; return;             // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8B9A: *out = GL_UNSIGNED_BYTE; return;  // IMPLEMENTATION_COLOR_READ_TYPE
    case 0x8B9B: *out = GL_RGBA; return;           // IMPLEMENTATION_COLOR_READ_FORMAT
    }
    glGetIntegerv(pname, out);
}
HLE(LIB_MALI, glGetString) {
    auto& st = hle_state(*a.t.proc);
    switch (a.u(0)) {
    case GL_VENDOR: a.ret(st.intern("ARM")); return;
    case GL_RENDERER: a.ret(st.intern("Mali-400 MP")); return;
    case GL_VERSION: a.ret(st.intern("OpenGL ES 2.0")); return;
    case 0x8B8C: a.ret(st.intern("OpenGL ES GLSL ES 1.00")); return;
    case GL_EXTENSIONS: a.ret(st.intern("GL_OES_texture_npot GL_OES_depth24 GL_OES_packed_depth_stencil GL_EXT_texture_format_BGRA8888 GL_OES_rgb8_rgba8")); return;
    }
    a.ret(0);
}
HLE(LIB_MALI, glGetProgramBinaryOES) {
    if (a.u(2)) a.m.wr<s32>(a.u(2), 0);
    if (t_cur) t_cur->pending_error = GL_INVALID_OPERATION;
}
HLE(LIB_MALI, glProgramBinaryOES) {
    if (t_cur) {
        t_cur->binary_failed.insert(a.u(0));
        t_cur->pending_error = GL_INVALID_ENUM;
    }
}
HLE(LIB_MALI, glShaderBinary) {
    if (t_cur) t_cur->pending_error = GL_INVALID_ENUM;
}
HLE(LIB_MALI, glGetProgramInfoLog) { glGetProgramInfoLog(a.u(0), a.s(1), (GLsizei*)P(2), (char*)P(3)); }
HLE(LIB_MALI, glGetProgramiv) {
    GLint* out = (GLint*)P(2);
    if (t_cur && a.u(1) == GL_LINK_STATUS && t_cur->binary_failed.count(a.u(0))) {
        *out = 0;
        return;
    }
    if (a.u(1) == 0x8741) {  // GL_PROGRAM_BINARY_LENGTH_OES
        *out = 0;
        return;
    }
    glGetProgramiv(a.u(0), a.u(1), out);
}
HLE(LIB_MALI, glGetShaderInfoLog) { glGetShaderInfoLog(a.u(0), a.s(1), (GLsizei*)P(2), (char*)P(3)); }
HLE(LIB_MALI, glGetShaderiv) { glGetShaderiv(a.u(0), a.u(1), (GLint*)P(2)); }
HLE(LIB_MALI, glGetUniformLocation) { a.ret(glGetUniformLocation(a.u(0), (const char*)P(1))); }
HLE(LIB_MALI, glLinkProgram) {
    if (t_cur) t_cur->binary_failed.erase(a.u(0));
    glLinkProgram(a.u(0));
    GLint ok = 0;
    glGetProgramiv(a.u(0), GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(a.u(0), sizeof log, nullptr, log);
        LOGI("[gl] program %u link failed: %s\n", a.u(0), log);
    }
}
HLE(LIB_MALI, glLineWidth) { glLineWidth(a.f(0)); }
HLE(LIB_MALI, glPixelStorei) { glPixelStorei(a.u(0), a.s(1)); }
HLE(LIB_MALI, glReadPixels) {
    GLenum fmt = a.u(4);
    glReadPixels(a.s(0), a.s(1), a.s(2), a.s(3), fmt, a.u(5), cptr(a, 6));
}
HLE(LIB_MALI, glRenderbufferStorage) {
    GLenum f = a.u(1);
    if (f == 0x8D62) f = GL_RGB8;        // RGB565
    else if (f == 0x8056 || f == 0x8057) f = GL_RGBA8;  // RGBA4 / RGB5_A1
    else if (f == 0x88F0) f = GL_DEPTH24_STENCIL8_EXT;
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, f, a.s(2), a.s(3));
}
HLE(LIB_MALI, glScissor) { glScissor(a.s(0), a.s(1), a.s(2), a.s(3)); }
HLE(LIB_MALI, glShaderSource) {
    GLuint sh = a.u(0);
    s32 n = a.s(1);
    u32 strs = a.u(2), lens = a.u(3);
    std::string src;
    for (s32 i = 0; i < n; i++) {
        u32 sp = a.m.rd<u32>(strs + i * 4);
        s32 len = lens ? a.m.rd<s32>(lens + i * 4) : -1;
        if (len < 0) src += a.m.read_str(sp);
        else src.append((const char*)a.m.ptr(sp), len);
    }
    GLint type = 0;
    glGetShaderiv(sh, GL_SHADER_TYPE, &type);
    std::string out = translate_shader(src, type);
    const char* p = out.c_str();
    glShaderSource(sh, 1, &p, nullptr);
}
HLE(LIB_MALI, glStencilFunc) { glStencilFunc(a.u(0), a.s(1), a.u(2)); }
HLE(LIB_MALI, glStencilOp) { glStencilOp(a.u(0), a.u(1), a.u(2)); }
HLE(LIB_MALI, glStencilMask) { glStencilMask(a.u(0)); }
HLE(LIB_MALI, glTexImage2D) {
    GLenum ifmt = a.u(2), fmt = a.u(6);
    if (ifmt == 0x80E1) ifmt = GL_RGBA;  // BGRA_EXT internal
    glTexImage2D(a.u(0), a.s(1), ifmt, a.s(3), a.s(4), a.s(5), fmt, a.u(7), cptr(a, 8));
}
HLE(LIB_MALI, glTexSubImage2D) {
    glTexSubImage2D(a.u(0), a.s(1), a.s(2), a.s(3), a.s(4), a.s(5), a.u(6), a.u(7), cptr(a, 8));
}
HLE(LIB_MALI, glTexParameteri) { glTexParameteri(a.u(0), a.u(1), a.s(2)); }
HLE(LIB_MALI, glTexParameterf) { glTexParameterf(a.u(0), a.u(1), a.f(0)); }
HLE(LIB_MALI, glUniform1f) { glUniform1f(a.s(0), a.f(0)); }
HLE(LIB_MALI, glUniform2f) { glUniform2f(a.s(0), a.f(0), a.f(1)); }
HLE(LIB_MALI, glUniform3f) { glUniform3f(a.s(0), a.f(0), a.f(1), a.f(2)); }
HLE(LIB_MALI, glUniform4f) { glUniform4f(a.s(0), a.f(0), a.f(1), a.f(2), a.f(3)); }
HLE(LIB_MALI, glUniform1i) { glUniform1i(a.s(0), a.s(1)); }
HLE(LIB_MALI, glUniform2i) { glUniform2i(a.s(0), a.s(1), a.s(2)); }
HLE(LIB_MALI, glUniform1fv) { glUniform1fv(a.s(0), a.s(1), (const GLfloat*)P(2)); }
HLE(LIB_MALI, glUniform2fv) { glUniform2fv(a.s(0), a.s(1), (const GLfloat*)P(2)); }
HLE(LIB_MALI, glUniform3fv) { glUniform3fv(a.s(0), a.s(1), (const GLfloat*)P(2)); }
HLE(LIB_MALI, glUniform4fv) { glUniform4fv(a.s(0), a.s(1), (const GLfloat*)P(2)); }
HLE(LIB_MALI, glUniformMatrix3fv) { glUniformMatrix3fv(a.s(0), a.s(1), a.u(2), (const GLfloat*)P(3)); }
HLE(LIB_MALI, glUniformMatrix4fv) { glUniformMatrix4fv(a.s(0), a.s(1), a.u(2), (const GLfloat*)P(3)); }
HLE(LIB_MALI, glUseProgram) { glUseProgram(a.u(0)); }
HLE(LIB_MALI, glVertexAttribPointer) {
    GLCtx* c = t_cur;
    const void* ptr;
    if (c && c->array_buffer) ptr = (const void*)(uintptr_t)a.u(5);
    else ptr = cptr(a, 5);
    glVertexAttribPointer(a.u(0), a.s(1), a.u(2), a.u(3), a.s(4), ptr);
}
HLE(LIB_MALI, glVertexAttrib4f) { glVertexAttrib4f(a.u(0), a.f(0), a.f(1), a.f(2), a.f(3)); }
HLE(LIB_MALI, glViewport) { glViewport(a.s(0), a.s(1), a.s(2), a.s(3)); }
HLE(LIB_MALI, glHint) { glHint(a.u(0), a.u(1)); }
HLE(LIB_MALI, glFrontFace) { glFrontFace(a.u(0)); }
HLE(LIB_MALI, glIsEnabled) { a.ret(glIsEnabled(a.u(0))); }
HLE(LIB_MALI, glGetFloatv) { glGetFloatv(a.u(0), (GLfloat*)P(1)); }
HLE(LIB_MALI, glGetBooleanv) { glGetBooleanv(a.u(0), (GLboolean*)P(1)); }
