/* gl_mesa.c -- the real renderer: Mesa's nouveau driver through its Switch EGL
 * platform, cross-built for AArch32 (mesa32; portlibs32/).
 * PvZ's engine renders with OpenGL ES 1.x (no "renderer" meta-data: GLES1 is
 * libnative_code's default), which Mesa serves through the same EGL; its
 * direct gl* imports resolve here too (so_util.c).
 *
 * The engine's egl* imports land here and pass through to Mesa, with the few
 * adjustments an Android build needs:
 *   - Android-only config/surface attributes (EGL_RECORDABLE_ANDROID, ...) are
 *     dropped; Mesa rejects attributes it does not know;
 *   - Mesa's Switch platform offers RGBA8888 window configs without MSAA, so a
 *     config request that finds nothing is retried without multisampling;
 *   - the native window is libnx's default NWindow (ANativeWindow_fromSurface
 *     returns it), and the on-screen boot log that owned it until now is
 *     closed first.
 * Every gl* name the engine looks up (dlsym or eglGetProcAddress) resolves
 * through Mesa's eglGetProcAddress, which serves core and extension entry
 * points alike (EGL_KHR_get_all_proc_addresses). MIT.
 */
#include "config.h"

#if DCR_GL_MESA
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "gl_layer.h"
#include "util.h"

void dcr_window_prepare(void); /* android_ndk.c */

static uint32_t g_frames;
uint32_t dcr_gl_frames(void) { return g_frames; }

#define EGL_RECORDABLE_ANDROID 0x3142
#define EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147
#define EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID 0x314C

static int log_budget = 200; /* first calls are logged: the hardware bring-up trace */
#define TRACE(...)                   \
  do {                               \
    if (log_budget > 0) {            \
      log_budget--;                  \
      debugPrintf(__VA_ARGS__);      \
    }                                \
  } while (0)

/* Copy an attribute list, dropping what Mesa's Switch platform cannot honour. */
static const EGLint *filter_attrs(const EGLint *in, EGLint *out, int cap, int drop_msaa) {
  if (!in)
    return NULL;
  int n = 0;
  for (; in[0] != EGL_NONE && n + 3 < cap; in += 2) {
    EGLint k = in[0], v = in[1];
    if (k == EGL_RECORDABLE_ANDROID || k == EGL_FRAMEBUFFER_TARGET_ANDROID ||
        k == EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID)
      continue;
    if (drop_msaa && (k == EGL_SAMPLES || k == EGL_SAMPLE_BUFFERS))
      continue;
    if (k == EGL_SURFACE_TYPE)
      v = EGL_WINDOW_BIT; /* the only surface type the platform provides */
    out[n++] = k;
    out[n++] = v;
  }
  out[n] = EGL_NONE;
  return out;
}

EGLDisplay b_eglGetDisplay(EGLNativeDisplayType d) {
  EGLDisplay r = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  TRACE("[egl] eglGetDisplay -> %p\n", r);
  return r;
}

EGLBoolean b_eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min) {
  EGLBoolean r = eglInitialize(d, maj, min);
  TRACE("[egl] eglInitialize -> %d (EGL %d.%d) err 0x%x\n", r, maj ? *maj : 0, min ? *min : 0,
        r ? 0 : eglGetError());
  return r;
}

EGLBoolean b_eglTerminate(EGLDisplay d) { return eglTerminate(d); }
EGLint b_eglGetError(void) { return eglGetError(); }
const char *b_eglQueryString(EGLDisplay d, EGLint name) { return eglQueryString(d, name); }

EGLBoolean b_eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *cfgs, EGLint cap, EGLint *num) {
  EGLint buf[128];
  EGLBoolean r = eglChooseConfig(d, filter_attrs(attrs, buf, 128, 0), cfgs, cap, num);
  if (r && num && *num == 0) {
    r = eglChooseConfig(d, filter_attrs(attrs, buf, 128, 1), cfgs, cap, num);
    TRACE("[egl] no MSAA configs: retried without multisampling -> %d\n", num ? *num : -1);
  }
  TRACE("[egl] eglChooseConfig -> %d, %d config(s)\n", r, num ? *num : -1);
  return r;
}

EGLBoolean b_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v) {
  if (a == EGL_RECORDABLE_ANDROID || a == EGL_FRAMEBUFFER_TARGET_ANDROID) {
    if (v) *v = EGL_FALSE;
    return EGL_TRUE;
  }
  return eglGetConfigAttrib(d, c, a, v);
}

EGLContext b_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs) {
  EGLContext r = eglCreateContext(d, c, share, attrs);
  EGLint ver = 1;
  for (const EGLint *a = attrs; a && a[0] != EGL_NONE; a += 2)
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
      ver = a[1];
  TRACE("[egl] eglCreateContext(GLES %d) -> %p err 0x%x\n", ver, r, r ? 0 : eglGetError());
  return r;
}

EGLBoolean b_eglDestroyContext(EGLDisplay d, EGLContext c) { return eglDestroyContext(d, c); }

EGLSurface b_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *attrs) {
  if (log_console_active())
    debugPrintf("[egl] handing the screen from the boot log to the game\n");
  log_console_close(); /* for good: see util.c */
  dcr_window_prepare();
  EGLint buf[64];
  EGLSurface r = eglCreateWindowSurface(d, c, (EGLNativeWindowType)nwindowGetDefault(),
                                        filter_attrs(attrs, buf, 64, 0));
  TRACE("[egl] eglCreateWindowSurface -> %p err 0x%x\n", r, r ? 0 : eglGetError());
  return r;
}

EGLSurface b_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attrs) {
  /* Mesa's Switch platform has window surfaces only. */
  debugPrintf("[egl] eglCreatePbufferSurface requested: not supported by the platform\n");
  return EGL_NO_SURFACE;
}

EGLBoolean b_eglDestroySurface(EGLDisplay d, EGLSurface s) { return eglDestroySurface(d, s); }

EGLBoolean b_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
  EGLBoolean r = eglMakeCurrent(d, dr, rd, c);
  TRACE("[egl] eglMakeCurrent(%p, %p) -> %d err 0x%x\n", dr, c, r, r ? 0 : eglGetError());
  if (r && c != EGL_NO_CONTEXT) {
    static int once;
    if (!once++) {
      const char *(*gs)(unsigned) = (const char *(*)(unsigned))eglGetProcAddress("glGetString");
      if (gs)
        debugPrintf("[gl] %s | %s | %s\n", gs(0x1F00), gs(0x1F01), gs(0x1F02));
    }
  }
  return r;
}

EGLContext b_eglGetCurrentContext(void) { return eglGetCurrentContext(); }
EGLSurface b_eglGetCurrentSurface(EGLint w) { return eglGetCurrentSurface(w); }
EGLBoolean b_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) {
  EGLBoolean r = eglQuerySurface(d, s, a, v);
  /* devkitPro's Switch EGL driver sizes a window surface's buffers from the
   * NWindow but never stores that size in the EGL surface, so EGL_WIDTH /
   * EGL_HEIGHT read 0 -- and Unity takes its screen size from them (hardware
   * 2026-09-23: "Camera rect 0 0 0 0", only the clear colour shown). Every
   * window surface here is on the default NWindow: answer with its size. */
  if (r && v && (a == EGL_WIDTH || a == EGL_HEIGHT) && *v == 0) {
    u32 w = 0, h = 0;
    if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &w, &h)) && w && h) {
      *v = (EGLint)(a == EGL_WIDTH ? w : h);
      static int logged;
      if (logged++ < 2)
        debugPrintf("[egl] eglQuerySurface(%s) = 0 from the driver -> %d (window size)\n",
                    a == EGL_WIDTH ? "EGL_WIDTH" : "EGL_HEIGHT", *v);
    }
  }
  return r;
}
EGLBoolean b_eglSwapInterval(EGLDisplay d, EGLint i) {
  static int logged;
  EGLBoolean r = eglSwapInterval(d, i);
  if (logged++ < 4)
    debugPrintf("[egl] eglSwapInterval(%d) -> %d\n", (int)i, (int)r);
  return r;
}

/* ------------------------------------------------------- frame capture
 * Minus (dcr_input.c) asks for the next frame the game presents: it is read
 * back just before the swap and saved as <root>/capture-NNN.bmp -- exactly
 * what the game drew, before the display scales and composes it -- and the
 * colours of its top rows and right-hand columns are logged. */
static volatile int g_capture_req;
static char g_capture_name[64]; /* lab_test.c: <root>/test/<name>.bmp */
void dcr_gl_request_capture(void) { g_capture_req = 1; }
void dcr_gl_request_capture_named(const char *name) {
  snprintf(g_capture_name, sizeof g_capture_name, "%s", name);
  g_capture_req = 1;
}
const char *dcr_game_root(void); /* main.c */
void dcr_window_size(int *w, int *h);

static void put_le(uint8_t *p, uint32_t v, int n) {
  for (int i = 0; i < n; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

/* 1 when saved; with skip_black, an all-black read (some frames read back
 * nothing in the emulator: the Asphalt 8 port's finding) is dropped and 0
 * returned, to try the next frame */
static int capture_frame(int skip_black) {
  int w, h;
  dcr_window_size(&w, &h);
  uint8_t *px = malloc((size_t)w * h * 4);
  if (!px)
    return 1;
  GLint read_fb = 0, pack_buf = 0, pack_align = 4, pack_row = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
  glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf);
  glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
  glPixelStorei(GL_PACK_ROW_LENGTH, pack_row);

  if (skip_black) {
    size_t lit = 0;
    for (size_t i = 0; i < (size_t)w * h * 4 && !lit; i += 4 * 97)
      lit = px[i] | px[i + 1] | px[i + 2];
    if (!lit) {
      free(px);
      return 0;
    }
  }
  /* GL rows run bottom-up, as a BMP's do: row h-1 is the top of the screen. */
  for (int k = 0; k < 12 && !g_capture_name[0]; k++) {
    const uint8_t *c = px + ((size_t)(h - 1 - k) * w + w / 2) * 4;
    const uint8_t *l = px + ((size_t)(h - 1 - k) * w + 8) * 4;
    debugPrintf("[capture] top row %2d: centre %02x%02x%02x, left %02x%02x%02x\n", k, c[0], c[1],
                c[2], l[0], l[1], l[2]);
  }
  for (int k = 0; k < 6 && !g_capture_name[0]; k++) {
    const uint8_t *c = px + ((size_t)(h * 3 / 4) * w + (w - 1 - k)) * 4;
    debugPrintf("[capture] right column %d (at 1/4 height): %02x%02x%02x\n", k, c[0], c[1], c[2]);
  }

  char path[300];
  FILE *f = NULL;
  if (g_capture_name[0]) {
    snprintf(path, sizeof path, "%s/test/%s.bmp", dcr_game_root(), g_capture_name);
    g_capture_name[0] = 0;
    f = fopen(path, "wb");
  }
  for (int n = 1; n < 1000 && !f; n++) {
    snprintf(path, sizeof path, "%s/capture-%03d.bmp", dcr_game_root(), n);
    FILE *probe = fopen(path, "rb");
    if (probe) {
      fclose(probe);
      continue;
    }
    f = fopen(path, "wb");
  }
  if (f) {
    const uint32_t row = (uint32_t)w * 3, size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    put_le(hdr + 2, size, 4);
    put_le(hdr + 10, 54, 4);
    put_le(hdr + 14, 40, 4);
    put_le(hdr + 18, (uint32_t)w, 4);
    put_le(hdr + 22, (uint32_t)h, 4);
    put_le(hdr + 26, 1, 2);
    put_le(hdr + 28, 24, 2);
    put_le(hdr + 34, row * (uint32_t)h, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *line = malloc(row);
    for (int y = 0; line && y < h; y++) {
      const uint8_t *s = px + (size_t)y * w * 4;
      for (int x = 0; x < w; x++) {
        line[x * 3 + 0] = s[x * 4 + 2];
        line[x * 3 + 1] = s[x * 4 + 1];
        line[x * 3 + 2] = s[x * 4 + 0];
      }
      fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    debugPrintf("[capture] frame saved to %s (%dx%d)\n", path, w, h);
  } else {
    debugPrintf("[capture] could not create a capture file in %s\n", dcr_game_root());
  }
  free(px);
  return 1;
}

void (*dcr_frame_hook)(void);
void (*dcr_present_hook)(void);

void dcr_boost_frame_begin(void);        /* dcr_boost.c */
void dcr_boost_frame_end(uint64_t frame);

/* A presented frame is where the engine's frame ends (its loop is in
 * libnative_code: GameRender, then this), and the next one begins. */
EGLBoolean b_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  if (dcr_present_hook)
    dcr_present_hook();
  if (g_capture_req) { /* under the emulator: up to 30 frames for one not all black */
    if (capture_frame(dcr_is_emulator() && g_capture_req < 30) || ++g_capture_req > 30)
      g_capture_req = 0;
  }
  EGLBoolean r = eglSwapBuffers(d, s);
  g_frames++;
  if (g_frames == 1 || !r)
    debugPrintf("[egl] eglSwapBuffers #%lu -> %d\n", (unsigned long)g_frames, r);
  dcr_boost_frame_end(g_frames);
  dcr_boost_frame_begin();
  if (dcr_frame_hook)
    dcr_frame_hook();
  return r;
}

EGLDisplay b_eglGetCurrentDisplay(void) { return eglGetCurrentDisplay(); }
EGLBoolean b_eglQueryContext(EGLDisplay d, EGLContext c, EGLint a, EGLint *v) {
  return eglQueryContext(d, c, a, v);
}

__eglMustCastToProperFunctionPointerType b_eglGetProcAddress(const char *name) {
  return (__eglMustCastToProperFunctionPointerType)dcr_gl_lookup(name);
}

#define G(n) {#n, (uintptr_t)b_##n}
static const struct { const char *name; uintptr_t fn; } g_egl[] = {
    G(eglGetDisplay), G(eglInitialize), G(eglTerminate), G(eglGetError), G(eglQueryString),
    G(eglChooseConfig), G(eglGetConfigAttrib), G(eglCreateContext), G(eglDestroyContext),
    G(eglCreateWindowSurface), G(eglCreatePbufferSurface), G(eglDestroySurface),
    G(eglMakeCurrent), G(eglGetCurrentContext), G(eglGetCurrentSurface), G(eglQuerySurface),
    G(eglSwapInterval), G(eglSwapBuffers), G(eglGetProcAddress), G(eglGetCurrentDisplay),
    G(eglQueryContext),
};

uintptr_t dcr_gl_lookup(const char *name) {
  if (!name)
    return 0;
  for (unsigned i = 0; i < sizeof g_egl / sizeof g_egl[0]; i++)
    if (!strcmp(g_egl[i].name, name))
      return g_egl[i].fn;
  if (name[0] == 'g' && name[1] == 'l')
    return (uintptr_t)eglGetProcAddress(name);
  if (name[0] == 'e' && name[1] == 'g' && name[2] == 'l')
    return (uintptr_t)eglGetProcAddress(name); /* EGL extensions Mesa implements */
  return 0;
}

/* ------------------------------------------------------------ self-test
 * Proves the graphics stack before the game touches it: EGL on the default
 * window, a GLSL program (Mesa's GLSL compiler + nouveau's code generator), a
 * draw, a read-back of the drawn pixel, and presents. Then everything is torn
 * down so the game starts from a clean EGL. */
static GLuint compile(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = "";
    glGetShaderInfoLog(sh, sizeof log, NULL, log);
    debugPrintf("[gl-test] shader compile failed: %s\n", log);
  }
  return sh;
}

/* Textures the way Mesa picks their formats from an UNSIZED internal format:
 * a GL_RGBA/GL_UNSIGNED_BYTE upload, and sampling an incomplete texture
 * (Mesa substitutes its fallback texture, itself GL_RGBA/GL_UNSIGNED_BYTE).
 * Both go through st_choose_matching_format, which crashed when
 * mesa_format was a 16-bit short enum (hardware 2026-09-24, first frame after
 * the age gate; fixed in mesa32 099a02a3, "AArch32: don't depend on
 * int-sized enums"). */
static void draw_textured(GLuint prog, GLuint tex, uint8_t px[4]) {
  static const GLfloat full[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(glGetUniformLocation(prog, "t"), 0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glReadPixels(DCR_FORCE_SCREEN_W / 2, DCR_FORCE_SCREEN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static int texture_test(EGLDisplay d, EGLSurface surf) {
  GLuint prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER,
                               "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER,
                               "precision mediump float; uniform sampler2D t;"
                               "void main() { gl_FragColor = texture2D(t, vec2(0.5)); }"));
  glBindAttribLocation(prog, 0, "p");
  glLinkProgram(prog);
  static const uint8_t texel[4] = {0x20, 0x80, 0xe0, 0xff};
  GLuint tex[2];
  glGenTextures(2, tex);
  /* complete: one level, no mipmap filter */
  glBindTexture(GL_TEXTURE_2D, tex[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  /* incomplete: the default filter wants mipmaps it does not have */
  glBindTexture(GL_TEXTURE_2D, tex[1]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  if (dcr_is_emulator()) {
    /* Drawing needs nv50_sampler_state_create, whose fixed-point vcvt
     * Ryujinx cannot execute; the uploads above are the format choice. */
    debugPrintf("[gl-test] textures: unsized RGBA uploads accepted (%s); drawing them skipped "
                "under the emulator\n",
                glGetError() == GL_NO_ERROR ? "no GL error" : "GL ERROR");
    glDeleteTextures(2, tex);
    glDeleteProgram(prog);
    return 1;
  }
  uint8_t a[4] = {0}, b[4] = {0};
  draw_textured(prog, tex[0], a);
  draw_textured(prog, tex[1], b);
  eglSwapBuffers(d, surf);
  int ok = a[0] == 0x20 && a[1] == 0x80 && a[2] == 0xe0 && b[0] == 0 && b[1] == 0 && b[2] == 0;
  debugPrintf("[gl-test] textures: unsized RGBA upload -> %02x%02x%02x (want 2080e0), incomplete "
              "texture -> Mesa's fallback %02x%02x%02x (want 000000): %s\n",
              a[0], a[1], a[2], b[0], b[1], b[2], ok ? "OK" : "FAILED");
  glDeleteTextures(2, tex);
  glDeleteProgram(prog);
  return ok;
}

int dcr_gl_selftest(void) {
  /* The window goes to EGL for good (see log_console_close): the rest of the
   * boot log is on the SD card only, and the test frame stays on screen until
   * the game's first frame replaces it. */
  if (log_console_active())
    debugPrintf("[gl-test] the screen now belongs to EGL: the rest of the boot log "
                "is in debug.log only\n");
  log_console_close();
  dcr_window_prepare();
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj = 0, min = 0, n = 0;
  int ok = 0;
  if (!eglInitialize(d, &maj, &min)) {
    debugPrintf("[gl-test] eglInitialize failed 0x%x\n", eglGetError());
    goto out;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint cfg_attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                                     EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                     EGL_NONE};
  EGLConfig cfg;
  if (!eglChooseConfig(d, cfg_attrs, &cfg, 1, &n) || n < 1) {
    debugPrintf("[gl-test] no config (0x%x)\n", eglGetError());
    goto term;
  }
  EGLSurface surf = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (!surf || !ctx || !eglMakeCurrent(d, surf, surf, ctx)) {
    debugPrintf("[gl-test] surface %p / context %p / make-current failed (0x%x)\n", surf, ctx,
                eglGetError());
    goto term;
  }
  debugPrintf("[gl-test] %s | %s | %s\n", (const char *)glGetString(GL_VENDOR),
              (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

  GLuint prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER,
                               "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER,
                               "precision mediump float; uniform vec4 c;"
                               "void main() { gl_FragColor = c; }"));
  glBindAttribLocation(prog, 0, "p");
  glLinkProgram(prog);
  GLint linked = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  static const GLfloat tri[] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f};
  uint8_t px[4] = {0};
  for (int f = 0; f < 60; f++) {
    glViewport(0, 0, DCR_FORCE_SCREEN_W, DCR_FORCE_SCREEN_H);
    glClearColor(0.05f, 0.1f + 0.3f * (float)f / 60.0f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniform4f(glGetUniformLocation(prog, "c"), 1.0f, 0.8f, 0.0f, 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (f == 59)
      glReadPixels(DCR_FORCE_SCREEN_W / 2, DCR_FORCE_SCREEN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    eglSwapBuffers(d, surf);
  }
  ok = linked && px[0] > 200 && px[1] > 150 && px[2] < 60;
  debugPrintf("[gl-test] program %s, centre pixel %02x%02x%02x: %s\n", linked ? "linked" : "NOT linked",
              px[0], px[1], px[2], ok ? "OK (triangle drawn)" : "FAILED");
  glDeleteProgram(prog);
  if (ok)
    ok = texture_test(d, surf);
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(d, ctx);
  eglDestroySurface(d, surf);
term:
  eglTerminate(d);
out:
  return ok;
}

#endif /* DCR_GL_MESA */
