/* gl_null.c -- a null EGL 1.4 / OpenGL ES 3.0 driver (see gl_layer.h).
 *
 * It renders nothing, but every call the engine makes behaves the way a real
 * driver's would where the engine can observe it: object names are unique and
 * nonzero, shaders compile and programs link, framebuffers are complete, syncs
 * are signalled, capability queries return plausible GLES 3.0 minimums, mapped
 * buffers are real memory, and eglSwapBuffers paces at 60 Hz. Any gl* name
 * without an entry here resolves to one no-op that returns 0 -- safe for every
 * signature under the ARM calling convention (the result register is r0, and
 * softfp returns floats there too).
 *
 * Unknown glGetIntegerv enums are logged once each, which doubles as a record
 * of the capabilities the engine actually cares about. MIT.
 */
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "gl_layer.h"
#include "util.h"

#if !DCR_GL_MESA

typedef int32_t GLint;
typedef uint32_t GLuint, GLenum;
typedef int32_t GLsizei;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;

void dcr_window_size(int *w, int *h); /* android_ndk.c */

static uint32_t g_next_name = 1;
static uint32_t g_frames;
uint32_t dcr_gl_frames(void) { return g_frames; }

static GLuint new_name(void) { return __atomic_fetch_add(&g_next_name, 1, __ATOMIC_RELAXED); }

/* ================================== GLES =================================== */
static int gl_noop(void) { return 0; }

static void gen_names(GLsizei n, GLuint *out) {
  for (GLsizei i = 0; out && i < n; i++)
    out[i] = new_name();
}
static void n_glGenBuffers(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenTextures(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenFramebuffers(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenRenderbuffers(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenVertexArrays(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenQueries(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenSamplers(GLsizei n, GLuint *o) { gen_names(n, o); }
static void n_glGenTransformFeedbacks(GLsizei n, GLuint *o) { gen_names(n, o); }
static GLuint n_glCreateProgram(void) { return new_name(); }
static GLuint n_glCreateShader(GLenum type) { return new_name(); }
static void *n_glFenceSync(GLenum cond, uint32_t flags) { return (void *)(uintptr_t)new_name(); }
static GLenum n_glClientWaitSync(void *s, uint32_t flags, uint64_t timeout) { return 0x911A; /* ALREADY_SIGNALED */ }
static GLenum n_glCheckFramebufferStatus(GLenum t) { return 0x8CD5; /* FRAMEBUFFER_COMPLETE */ }
static GLint n_glGetUniformLocation(GLuint p, const char *name) { return -1; }
static GLint n_glGetAttribLocation(GLuint p, const char *name) { return -1; }
static GLuint n_glGetUniformBlockIndex(GLuint p, const char *name) { return 0xFFFFFFFFu; }
static uint8_t n_glIsVertexArray(GLuint a) { return a != 0; }

static const char *const g_exts[] = {
    "GL_OES_depth24", "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8",
    "GL_EXT_texture_format_BGRA8888", "GL_OES_element_index_uint",
};
static const char g_ext_string[] =
    "GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 "
    "GL_EXT_texture_format_BGRA8888 GL_OES_element_index_uint";

static const uint8_t *n_glGetString(GLenum e) {
  switch (e) {
  case 0x1F00: return (const uint8_t *)"labyrinth2_nx";
  case 0x1F01: return (const uint8_t *)"null renderer";
  case 0x1F02: return (const uint8_t *)"OpenGL ES 3.0 dcr-null";
  case 0x1F03: return (const uint8_t *)g_ext_string;
  case 0x8B8C: return (const uint8_t *)"OpenGL ES GLSL ES 3.00";
  default: return NULL;
  }
}
static const uint8_t *n_glGetStringi(GLenum e, GLuint i) {
  return (e == 0x1F03 && i < (GLuint)ARRAY_SIZE(g_exts)) ? (const uint8_t *)g_exts[i] : NULL;
}

static void n_glGetIntegerv(GLenum e, GLint *v) {
  if (!v)
    return;
  int w, h;
  switch (e) {
  case 0x0D33: case 0x851C: case 0x84E8: *v = 4096; return; /* max texture/cube/renderbuffer */
  case 0x0D3A: dcr_window_size(&w, &h); v[0] = 4096; v[1] = 4096; return; /* MAX_VIEWPORT_DIMS */
  case 0x0BA2: dcr_window_size(&w, &h); v[0] = 0; v[1] = 0; v[2] = w; v[3] = h; return; /* VIEWPORT */
  case 0x8073: *v = 2048; return;        /* MAX_3D_TEXTURE_SIZE */
  case 0x88FF: *v = 256; return;         /* MAX_ARRAY_TEXTURE_LAYERS */
  case 0x8869: *v = 16; return;          /* MAX_VERTEX_ATTRIBS */
  case 0x8872: case 0x8B4C: *v = 16; return; /* texture image units (fs / vs) */
  case 0x8B4D: *v = 32; return;          /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
  case 0x8DFB: *v = 256; return;         /* MAX_VERTEX_UNIFORM_VECTORS */
  case 0x8DFD: *v = 224; return;         /* MAX_FRAGMENT_UNIFORM_VECTORS */
  case 0x8DFC: *v = 15; return;          /* MAX_VARYING_VECTORS */
  case 0x8B4B: *v = 60; return;          /* MAX_VARYING_COMPONENTS */
  case 0x8824: case 0x8CDF: *v = 4; return; /* MAX_DRAW_BUFFERS / MAX_COLOR_ATTACHMENTS */
  case 0x8D57: *v = 4; return;           /* MAX_SAMPLES */
  case 0x821B: *v = 3; return;           /* MAJOR_VERSION */
  case 0x821C: *v = 0; return;           /* MINOR_VERSION */
  case 0x821D: *v = ARRAY_SIZE(g_exts); return; /* NUM_EXTENSIONS */
  case 0x8A30: *v = 16384; return;       /* MAX_UNIFORM_BLOCK_SIZE */
  case 0x8A2F: *v = 24; return;          /* MAX_UNIFORM_BUFFER_BINDINGS */
  case 0x8A2B: case 0x8A2D: *v = 12; return; /* MAX_VERTEX/FRAGMENT_UNIFORM_BLOCKS */
  case 0x8A2E: *v = 24; return;          /* MAX_COMBINED_UNIFORM_BLOCKS */
  case 0x8A34: *v = 256; return;         /* UNIFORM_BUFFER_OFFSET_ALIGNMENT */
  case 0x8D6B: *v = -1; return;          /* MAX_ELEMENT_INDEX */
  case 0x8B4A: *v = 1024; return;        /* MAX_VERTEX_UNIFORM_COMPONENTS */
  case 0x8B49: *v = 896; return;         /* MAX_FRAGMENT_UNIFORM_COMPONENTS */
  case 0x8C8A: case 0x8C8B: case 0x8C80: *v = 4; return; /* transform feedback limits */
  case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: *v = 8; return; /* R/G/B/A bits */
  case 0x0D56: *v = 24; return;          /* DEPTH_BITS */
  case 0x0D57: *v = 8; return;           /* STENCIL_BITS */
  case 0x0D50: *v = 4; return;           /* SUBPIXEL_BITS */
  case 0x0D05: *v = 4; return;           /* PACK_ALIGNMENT */
  case 0x0CF5: *v = 4; return;           /* UNPACK_ALIGNMENT */
  case 0x84FF: *v = 1; return;           /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
  case 0x86A2: case 0x87FE: case 0x8DF9: *v = 0; return; /* no compressed/binary formats */
  case 0x8CA6: case 0x8CAA: case 0x8CA7: case 0x8894: case 0x8895: case 0x85B5: case 0x8B8D:
  case 0x8069: case 0x84E0:
    *v = 0; /* bindings: nothing bound / TEXTURE0 */
    if (e == 0x84E0) *v = 0x84C0;
    return;
  default: {
    static uint32_t seen[64];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++)
      known |= seen[i] == e;
    if (!known && nseen < ARRAY_SIZE(seen)) {
      seen[nseen++] = e;
      debugPrintf("[gl-null] glGetIntegerv(0x%04lx) -> 0\n", (unsigned long)e);
    }
    *v = 0;
  }
  }
}

static void n_glGetShaderiv(GLuint s, GLenum e, GLint *v) {
  if (v) *v = (e == 0x8B81) ? 1 : 0; /* COMPILE_STATUS */
}
static void n_glGetProgramiv(GLuint p, GLenum e, GLint *v) {
  if (!v) return;
  switch (e) {
  case 0x8B82: case 0x8B83: *v = 1; return; /* LINK_STATUS / VALIDATE_STATUS */
  case 0x8B87: case 0x8B8A: case 0x8A35: *v = 1; return; /* max name lengths */
  default: *v = 0; return;
  }
}
static void write_empty_log(GLsizei cap, GLsizei *len, char *log) {
  if (len) *len = 0;
  if (log && cap > 0) log[0] = 0;
}
static void n_glGetShaderInfoLog(GLuint s, GLsizei cap, GLsizei *len, char *log) { write_empty_log(cap, len, log); }
static void n_glGetProgramInfoLog(GLuint p, GLsizei cap, GLsizei *len, char *log) { write_empty_log(cap, len, log); }
static void n_glGetShaderSource(GLuint s, GLsizei cap, GLsizei *len, char *src) { write_empty_log(cap, len, src); }
static void n_glGetObjectLabel(GLenum id, GLuint n, GLsizei cap, GLsizei *len, char *l) { write_empty_log(cap, len, l); }
static void n_glGetProgramBinary(GLuint p, GLsizei cap, GLsizei *len, GLenum *fmt, void *bin) {
  if (len) *len = 0;
  if (fmt) *fmt = 0;
}
static void n_glGetShaderPrecisionFormat(GLenum shader, GLenum type, GLint *range, GLint *prec) {
  int is_float = type <= 0x8DF2; /* LOW/MEDIUM/HIGH_FLOAT */
  if (range) { range[0] = is_float ? 127 : 31; range[1] = is_float ? 127 : 30; }
  if (prec) *prec = is_float ? 23 : 0;
}
static void n_glGetInternalformativ(GLenum t, GLenum f, GLenum pname, GLsizei n, GLint *v) {
  if (!v || n < 1) return;
  if (pname == 0x9380) *v = 1;          /* NUM_SAMPLE_COUNTS */
  else if (pname == 0x80A9) *v = 4;     /* SAMPLES */
  else *v = 0;
}
static void zero_ints(GLint *v) { if (v) *v = 0; }
static void n_glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *v) { zero_ints(v); }
static void n_glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { zero_ints(v); }
static void n_glGetTexParameteriv(GLenum t, GLenum p, GLint *v) { zero_ints(v); }
static void n_glGetVertexAttribiv(GLuint i, GLenum p, GLint *v) { zero_ints(v); }
static void n_glGetUniformiv(GLuint p, GLint l, GLint *v) { zero_ints(v); }
static void n_glGetActiveUniformBlockiv(GLuint p, GLuint b, GLenum e, GLint *v) { zero_ints(v); }
static void n_glGetProgramInterfaceiv(GLuint p, GLenum i, GLenum e, GLint *v) { zero_ints(v); }
static void n_glGetActiveUniformsiv(GLuint p, GLsizei n, const GLuint *idx, GLenum e, GLint *v) {
  for (GLsizei i = 0; v && i < n; i++) v[i] = 0;
}
static void n_glGetUniformIndices(GLuint p, GLsizei n, const char *const *names, GLuint *idx) {
  for (GLsizei i = 0; idx && i < n; i++) idx[i] = 0xFFFFFFFFu;
}
static void n_glGetProgramResourceiv(GLuint p, GLenum i, GLuint x, GLsizei n, const GLenum *props,
                                     GLsizei cap, GLsizei *len, GLint *v) {
  for (GLsizei k = 0; v && k < cap; k++) v[k] = 0;
  if (len) *len = cap < n ? cap : n;
}
static void n_glGetQueryObjectui64v(GLuint q, GLenum p, uint64_t *v) { if (v) *v = 0; }

/* Mapped buffers: the engine writes vertex/index data straight into these, so
 * they must be real memory. One live mapping per buffer target is how GLES
 * works (mapping a mapped buffer is an error), keyed on the target enum. */
static struct { GLenum target; void *p; } g_maps[16];
static void *map_for(GLenum target, size_t len) {
  for (int i = 0; i < ARRAY_SIZE(g_maps); i++)
    if (!g_maps[i].p) {
      g_maps[i].target = target;
      g_maps[i].p = memalign(64, len ? len : 64);
      return g_maps[i].p;
    }
  return NULL;
}
static void *n_glMapBufferRange(GLenum t, GLintptr off, GLsizeiptr len, uint32_t access) { return map_for(t, (size_t)len); }
static void *n_glMapBufferOES(GLenum t, GLenum access) { return map_for(t, 1 << 20); }
static uint8_t n_glUnmapBuffer(GLenum t) {
  for (int i = ARRAY_SIZE(g_maps) - 1; i >= 0; i--)
    if (g_maps[i].p && g_maps[i].target == t) {
      free(g_maps[i].p);
      g_maps[i].p = NULL;
      break;
    }
  return 1;
}

/* ================================== EGL ==================================== */
#define EGL_SUCCESS 0x3000
#define EGL_NONE 0x3038
static int g_display, g_config, g_context, g_window_surface, g_pbuffer_surface;
static void *g_cur_ctx, *g_cur_draw;

EGLBoolean b_eglInitialize(void *dpy, EGLint *major, EGLint *minor) {
  if (major) *major = 1;
  if (minor) *minor = 4;
  return 1;
}
void *b_eglGetDisplay(void *native) { return &g_display; }
EGLBoolean b_eglTerminate(void *dpy) { return 1; }
EGLint b_eglGetError(void) { return EGL_SUCCESS; }

EGLBoolean b_eglChooseConfig(void *dpy, const EGLint *attrs, void **configs, EGLint cap, EGLint *num) {
  if (configs && cap > 0)
    configs[0] = &g_config;
  if (num)
    *num = 1;
  return 1;
}

EGLBoolean b_eglGetConfigAttrib(void *dpy, void *cfg, EGLint attr, EGLint *v) {
  if (!v)
    return 0;
  switch (attr) {
  case 0x3020: *v = 32; break;                    /* BUFFER_SIZE */
  case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = 8; break; /* A/B/G/R */
  case 0x3025: *v = 24; break;                    /* DEPTH_SIZE */
  case 0x3026: *v = 8; break;                     /* STENCIL_SIZE */
  case 0x3027: *v = 0x3038; break;                /* CONFIG_CAVEAT = NONE */
  case 0x3028: *v = 1; break;                     /* CONFIG_ID */
  case 0x302E: *v = 1; break;                     /* NATIVE_VISUAL_ID (RGBA_8888) */
  case 0x3031: case 0x3032: *v = 0; break;        /* SAMPLES / SAMPLE_BUFFERS */
  case 0x3033: *v = 0x0005; break;                /* SURFACE_TYPE = WINDOW|PBUFFER */
  case 0x3040: case 0x3042: *v = 0x0044; break;   /* RENDERABLE_TYPE / CONFORMANT = ES2|ES3 */
  case 0x303F: *v = 0x308E; break;                /* COLOR_BUFFER_TYPE = RGB_BUFFER */
  case 0x302C: case 0x302A: *v = 4096; break;     /* MAX_PBUFFER_WIDTH/HEIGHT */
  default: *v = 0; break;
  }
  return 1;
}

void *b_eglCreateContext(void *dpy, void *cfg, void *share, const EGLint *attrs) {
  int ver = 1;
  for (const EGLint *a = attrs; a && a[0] != EGL_NONE; a += 2)
    if (a[0] == 0x3098) /* CONTEXT_CLIENT_VERSION */
      ver = a[1];
  debugPrintf("[gl-null] eglCreateContext: GLES %d\n", ver);
  return &g_context;
}
EGLBoolean b_eglDestroyContext(void *dpy, void *ctx) { return 1; }
void *b_eglCreateWindowSurface(void *dpy, void *cfg, void *win, const EGLint *attrs) {
  return &g_window_surface;
}
void *b_eglCreatePbufferSurface(void *dpy, void *cfg, const EGLint *attrs) { return &g_pbuffer_surface; }
EGLBoolean b_eglDestroySurface(void *dpy, void *s) { return 1; }

EGLBoolean b_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
  g_cur_ctx = ctx;
  g_cur_draw = draw;
  return 1;
}
void *b_eglGetCurrentContext(void) { return g_cur_ctx; }
void *b_eglGetCurrentSurface(EGLint which) { return g_cur_draw; }

EGLBoolean b_eglQuerySurface(void *dpy, void *s, EGLint attr, EGLint *v) {
  int w, h;
  dcr_window_size(&w, &h);
  if (!v)
    return 0;
  if (attr == 0x3057) *v = w;       /* WIDTH */
  else if (attr == 0x3056) *v = h;  /* HEIGHT */
  else if (attr == 0x3086) *v = 0x3084; /* RENDER_BUFFER = BACK_BUFFER */
  else *v = 0;
  return 1;
}

const char *b_eglQueryString(void *dpy, EGLint name) {
  switch (name) {
  case 0x3053: return "labyrinth2_nx";                                  /* VENDOR */
  case 0x3054: return "1.4 dcr-null";                            /* VERSION */
  case 0x3055: return "EGL_KHR_create_context EGL_KHR_surfaceless_context"; /* EXTENSIONS */
  case 0x308D: return "OpenGL_ES";                               /* CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean b_eglSwapInterval(void *dpy, EGLint interval) { return 1; }
void *b_eglGetCurrentDisplay(void) { return (void *)1; }
EGLBoolean b_eglQueryContext(void *dpy, void *ctx, EGLint attr, EGLint *v) {
  if (v)
    *v = attr == 0x3098 /* EGL_CONTEXT_CLIENT_VERSION */ ? 1 : 0;
  return 1;
}

void (*dcr_frame_hook)(void);
void (*dcr_present_hook)(void);

EGLBoolean b_eglSwapBuffers(void *dpy, void *surface) {
  /* Pace to 60 Hz on the system tick, as a display would. */
  static u64 next;
  const u64 frame = armNsToTicks(16666667);
  u64 now = armGetSystemTick();
  if (next > now)
    svcSleepThread((s64)armTicksToNs(next - now));
  now = armGetSystemTick();
  next = (next + frame > now) ? next + frame : now + frame;
  g_frames++;
  if (g_frames == 1 || (g_frames % 600) == 0)
    debugPrintf("[gl-null] frame %lu\n", (unsigned long)g_frames);
  if (dcr_frame_hook)
    dcr_frame_hook();
  return 1;
}

void *b_eglGetProcAddress(const char *name) { return (void *)dcr_gl_lookup(name); }

/* ================================= lookup ================================== */
#define E(n) {#n, (uintptr_t)n_##n}
#define A(alias, n) {#alias, (uintptr_t)n_##n}
#define G(n) {#n, (uintptr_t)b_##n}
static const struct { const char *name; uintptr_t fn; } g_table[] = {
    E(glGenBuffers), E(glGenTextures), E(glGenFramebuffers), E(glGenRenderbuffers),
    E(glGenVertexArrays), A(glGenVertexArraysOES, glGenVertexArrays), E(glGenQueries),
    A(glGenQueriesEXT, glGenQueries), E(glGenSamplers), E(glGenTransformFeedbacks),
    E(glCreateProgram), E(glCreateShader), E(glFenceSync), E(glClientWaitSync),
    E(glCheckFramebufferStatus), E(glGetUniformLocation), E(glGetAttribLocation),
    E(glGetUniformBlockIndex), E(glIsVertexArray), E(glGetString), E(glGetStringi),
    E(glGetIntegerv), E(glGetShaderiv), E(glGetProgramiv), E(glGetShaderInfoLog),
    E(glGetProgramInfoLog), E(glGetShaderSource), E(glGetObjectLabel),
    A(glGetObjectLabelEXT, glGetObjectLabel), A(glGetObjectLabelKHR, glGetObjectLabel),
    E(glGetProgramBinary), A(glGetProgramBinaryOES, glGetProgramBinary),
    E(glGetShaderPrecisionFormat), E(glGetInternalformativ),
    E(glGetFramebufferAttachmentParameteriv), E(glGetRenderbufferParameteriv),
    E(glGetTexParameteriv), A(glGetTextureParameteriv, glGetTexParameteriv),
    E(glGetVertexAttribiv), E(glGetUniformiv), E(glGetActiveUniformBlockiv),
    E(glGetProgramInterfaceiv), E(glGetActiveUniformsiv), E(glGetUniformIndices),
    E(glGetProgramResourceiv), A(glGetQueryObjectui64vEXT, glGetQueryObjectui64v),
    A(glGetQueryObjectui64vNV, glGetQueryObjectui64v), E(glMapBufferRange),
    A(glMapBufferRangeEXT, glMapBufferRange), E(glMapBufferOES), E(glUnmapBuffer),
    A(glUnmapBufferOES, glUnmapBuffer), A(glUnmapBufferEXT, glUnmapBuffer),
    G(eglInitialize), G(eglGetDisplay), G(eglTerminate), G(eglGetError), G(eglChooseConfig),
    G(eglGetConfigAttrib), G(eglCreateContext), G(eglDestroyContext),
    G(eglCreateWindowSurface), G(eglCreatePbufferSurface), G(eglDestroySurface),
    G(eglMakeCurrent), G(eglGetCurrentContext), G(eglGetCurrentSurface), G(eglQuerySurface),
    G(eglQueryString), G(eglSwapInterval), G(eglSwapBuffers), G(eglGetProcAddress),
    G(eglGetCurrentDisplay), G(eglQueryContext),
};

uintptr_t dcr_gl_lookup(const char *name) {
  if (!name)
    return 0;
  for (int i = 0; i < ARRAY_SIZE(g_table); i++)
    if (!strcmp(g_table[i].name, name))
      return g_table[i].fn;
  if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z')
    return (uintptr_t)gl_noop;
  return 0;
}

#endif /* !DCR_GL_MESA */
