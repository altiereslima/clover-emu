// Runtime loader for OpenGL > 1.1 entry points on Windows (see gl_api.h).
#include "gl_api.h"
#include "common.h"
#include <SDL.h>
#include <string>

#define CLOVER_GL_DEF(T, n) T clv_##n = nullptr;
CLOVER_GL_FUNCS(CLOVER_GL_DEF)
#undef CLOVER_GL_DEF

static void* load_one(const char* name) {
    void* p = SDL_GL_GetProcAddress(name);
    if (p) return p;
    // EXT_framebuffer_object entry points have identical core (ARB) equivalents
    std::string s(name);
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "EXT") == 0) {
        s.resize(s.size() - 3);
        p = SDL_GL_GetProcAddress(s.c_str());
        if (p) return p;
        p = SDL_GL_GetProcAddress((s + "ARB").c_str());
    }
    return p;
}

bool gl_load() {
    static bool done = false, ok = true;
    if (done) return ok;
    done = true;
#define CLOVER_GL_LOAD(T, n) \
    clv_##n = (T)load_one(#n); \
    if (!clv_##n) { LOGI("[gl] missing OpenGL entry point %s\n", #n); ok = false; }
    CLOVER_GL_FUNCS(CLOVER_GL_LOAD)
#undef CLOVER_GL_LOAD
    return ok;
}
