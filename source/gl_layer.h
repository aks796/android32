/* gl_layer.h -- the EGL/GLES provider the engine is linked against.
 *
 * Engines import the egl* entry points directly (DT_NEEDED libEGL) and find
 * their gl* functions at run time through eglGetProcAddress / dlsym, or import
 * them directly. All of it goes through this interface, so the backend is
 * swappable in one place (runtime.mk picks one, DCR_GL_MESA):
 *
 *   gl_mesa.c   Mesa's nouveau driver cross-built for AArch32 (mesa32,
 *               portlibs32/): the real renderer.
 *   gl_null.c   a null driver: correct object/query semantics, no rendering.
 *               Lets the engine and the game's code boot and run their frame
 *               loop on an emulator without a GPU driver.
 *
 * The frame hooks and the port_gl_* callbacks are how a port joins the frame
 * (draws over it, times it, wraps a GL call) without its own copy of the
 * renderer. The capture, self-test, glthread and swap callbacks are the mesa
 * renderer's; the null one keeps the names (captures do nothing). MIT.
 */
#ifndef DCR_GL_LAYER_H
#define DCR_GL_LAYER_H
#include <stdint.h>

/* Address of a gl* / egl* function by name, 0 if the backend has none. */
uintptr_t dcr_gl_lookup(const char *name);

/* Frames presented so far (eglSwapBuffers calls on a window surface). */
uint32_t dcr_gl_frames(void);

/* Called after each presented frame, on the engine's thread (NULL: none). */
extern void (*dcr_frame_hook)(void);
/* Called just before each present, to draw over the frame (NULL: none;
 * mesa only). */
extern void (*dcr_present_hook)(void);

/* EGL on the default window, a GLSL program, a draw and a read-back, then
 * everything torn down: 1 when the stack works (gl_mesa.c; main.c runs it
 * under the emulator or with [debug] gl_selftest). */
int dcr_gl_selftest(void);

/* Save the next presented frame as <root>/capture-NNN.bmp (under the
 * emulator, the first one of the next RT_CAPTURE_TRIES that is not all black). */
void dcr_gl_request_capture(void);
/* The same, saved as <root>/test/<name>.bmp (a test script's picture). */
void dcr_gl_request_capture_named(const char *name);
/* The current back buffer, saved now, on the thread whose context it is. */
void dcr_gl_capture_now(void);

/* Mesa's glthread for a context not yet current (mesa32 972de9c1): its GL
 * calls are recorded on the calling thread and run by a worker. 1 if it
 * started. The arguments are an EGLDisplay and an EGLContext. */
int rt_egl_start_glthread(void *display, void *context);

/* ---------------------------------------------------------------- callbacks
 * Weak; the runtime's defaults do nothing. mesa renderer only. */

/* A gl* function looked up by the engine (dlsym, eglGetProcAddress, the
 * import table): return a wrapper for `real` (keep `real` to call it), or 0
 * for `real` itself. `real` may be 0. */
uintptr_t port_gl_wrap(const char *name, uintptr_t real);
/* Just before eglSwapBuffers hands the frame to the driver (after the
 * present hook and any capture). */
void port_gl_before_swap(void);
/* Just after it returns; `frame` is dcr_gl_frames() counting this one. Runs
 * before the boost's frame end and the frame hook. */
void port_gl_after_swap(uint32_t frame);

#endif
