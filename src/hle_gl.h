#pragma once
#include "common.h"
#include <memory>
#include <set>

struct GLCtx {
    void* host = nullptr;  // SDL_GLContext
    u32 guest_handle = 0;
    bool initialized = false;
    unsigned vfb = 0, vfb_tex = 0, vfb_depth = 0;  // virtual default framebuffer (1280x720)
    unsigned array_buffer = 0;
    unsigned cur_fbo = 0;  // guest-visible binding
    int swap_interval = 1;
    unsigned pending_error = 0;
    std::set<unsigned> binary_failed;  // programs "loaded" via glProgramBinaryOES (unsupported)
    u64 frames = 0;
    u64 last_vsync = 0;
};

std::shared_ptr<GLCtx> gl_create_context();
void gl_destroy_context(GLCtx* c);
void gl_make_current(GLCtx* c);
GLCtx* gl_current();
void gl_set_swap_interval(int i);
void gl_swap();
void gl_create_target(int w, int h, u32* tex, u32* fbo);
void gl_destroy_target(u32 tex, u32 fbo);
void gl_bind_target(u32 fbo, int w, int h);
void gl_unbind_texture();

struct GLFormat {
    unsigned format, type, bpp;
};
void gl_upload(u32 tex, int x, int y, int w, int h, int row_len, const GLFormat& f, const void* data);
void gl_render_copy(u32 tex, int tw, int th, const int src[4], const int dst[4], int vw, int vh, bool blend);
void gl_render_clear();
void gl_set_draw_color(float r, float g, float b, float a);
