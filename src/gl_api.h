// OpenGL entry points. macOS/Linux link GL directly; on Windows, opengl32.dll only exports GL 1.1,
// so everything newer is loaded at runtime (gl_load(), after a context is current).
// Generated list: keep in sync with the gl* calls in hle_gl.cpp / display.cpp.
#pragma once
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
inline bool gl_load() { return true; }
#elif defined(_WIN32)
#include <SDL_opengl.h>
bool gl_load();
#define CLOVER_GL_FUNCS(X) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBINDFRAMEBUFFEREXTPROC, glBindFramebufferEXT) \
    X(PFNGLBINDRENDERBUFFEREXTPROC, glBindRenderbufferEXT) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBLITFRAMEBUFFEREXTPROC, glBlitFramebufferEXT) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC, glCheckFramebufferStatusEXT) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLDELETEFRAMEBUFFERSEXTPROC, glDeleteFramebuffersEXT) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETERENDERBUFFERSEXTPROC, glDeleteRenderbuffersEXT) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC, glFramebufferRenderbufferEXT) \
    X(PFNGLFRAMEBUFFERTEXTURE2DEXTPROC, glFramebufferTexture2DEXT) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLGENFRAMEBUFFERSEXTPROC, glGenFramebuffersEXT) \
    X(PFNGLGENRENDERBUFFERSEXTPROC, glGenRenderbuffersEXT) \
    X(PFNGLGENERATEMIPMAPEXTPROC, glGenerateMipmapEXT) \
    X(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLRENDERBUFFERSTORAGEEXTPROC, glRenderbufferStorageEXT) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM2FVPROC, glUniform2fv) \
    X(PFNGLUNIFORM2IPROC, glUniform2i) \
    X(PFNGLUNIFORM3FPROC, glUniform3f) \
    X(PFNGLUNIFORM3FVPROC, glUniform3fv) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
#define CLOVER_GL_DECL(T, n) extern T clv_##n;
CLOVER_GL_FUNCS(CLOVER_GL_DECL)
#undef CLOVER_GL_DECL
#define glActiveTexture clv_glActiveTexture
#define glAttachShader clv_glAttachShader
#define glBindAttribLocation clv_glBindAttribLocation
#define glBindBuffer clv_glBindBuffer
#define glBindFramebufferEXT clv_glBindFramebufferEXT
#define glBindRenderbufferEXT clv_glBindRenderbufferEXT
#define glBlendEquation clv_glBlendEquation
#define glBlendFuncSeparate clv_glBlendFuncSeparate
#define glBlitFramebufferEXT clv_glBlitFramebufferEXT
#define glBufferData clv_glBufferData
#define glBufferSubData clv_glBufferSubData
#define glCheckFramebufferStatusEXT clv_glCheckFramebufferStatusEXT
#define glCompileShader clv_glCompileShader
#define glCreateProgram clv_glCreateProgram
#define glCreateShader clv_glCreateShader
#define glDeleteBuffers clv_glDeleteBuffers
#define glDeleteFramebuffersEXT clv_glDeleteFramebuffersEXT
#define glDeleteProgram clv_glDeleteProgram
#define glDeleteRenderbuffersEXT clv_glDeleteRenderbuffersEXT
#define glDeleteShader clv_glDeleteShader
#define glDisableVertexAttribArray clv_glDisableVertexAttribArray
#define glEnableVertexAttribArray clv_glEnableVertexAttribArray
#define glFramebufferRenderbufferEXT clv_glFramebufferRenderbufferEXT
#define glFramebufferTexture2DEXT clv_glFramebufferTexture2DEXT
#define glGenBuffers clv_glGenBuffers
#define glGenFramebuffersEXT clv_glGenFramebuffersEXT
#define glGenRenderbuffersEXT clv_glGenRenderbuffersEXT
#define glGenerateMipmapEXT clv_glGenerateMipmapEXT
#define glGetAttribLocation clv_glGetAttribLocation
#define glGetProgramInfoLog clv_glGetProgramInfoLog
#define glGetProgramiv clv_glGetProgramiv
#define glGetShaderInfoLog clv_glGetShaderInfoLog
#define glGetShaderiv clv_glGetShaderiv
#define glGetUniformLocation clv_glGetUniformLocation
#define glLinkProgram clv_glLinkProgram
#define glRenderbufferStorageEXT clv_glRenderbufferStorageEXT
#define glShaderSource clv_glShaderSource
#define glUniform1f clv_glUniform1f
#define glUniform1fv clv_glUniform1fv
#define glUniform1i clv_glUniform1i
#define glUniform2f clv_glUniform2f
#define glUniform2fv clv_glUniform2fv
#define glUniform2i clv_glUniform2i
#define glUniform3f clv_glUniform3f
#define glUniform3fv clv_glUniform3fv
#define glUniform4f clv_glUniform4f
#define glUniform4fv clv_glUniform4fv
#define glUniformMatrix3fv clv_glUniformMatrix3fv
#define glUniformMatrix4fv clv_glUniformMatrix4fv
#define glUseProgram clv_glUseProgram
#define glVertexAttrib4f clv_glVertexAttrib4f
#define glVertexAttribPointer clv_glVertexAttribPointer
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
inline bool gl_load() { return true; }
#endif
