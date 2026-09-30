/* gl_layer.h -- the EGL/GLES provider the engine is linked against.
 *
 * libunity imports the egl* entry points directly (DT_NEEDED libEGL) and finds
 * every gl* function at run time through eglGetProcAddress / dlsym. Both go
 * through this interface, so the backend is swappable in one place:
 *
 *   gl_null.c   a null driver: correct object/query semantics, no rendering.
 *               Lets the engine, Mono and the game scripts boot and run their
 *               frame loop on an emulator before the 32-bit mesa build exists.
 *   gl_mesa.c   (later) mesa32 (Mesa/nouveau) cross-built for AArch32.
 * MIT.
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
/* Called just before each present, to draw over the frame (NULL: none). */
extern void (*dcr_present_hook)(void);

#endif
