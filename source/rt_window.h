/* rt_window.h -- the one window (libnx's default NWindow) and its size.
 *
 * The rendering size starts at RT_SCREEN_W x RT_SCREEN_H; config.ini
 * [display] resolution changes it (dcr_window_set_size, before the engine
 * starts). The EGL layer calls dcr_window_prepare right before it makes the
 * window surface; the vi layer scales the window to the screen. MIT.
 */
#ifndef RT_WINDOW_H
#define RT_WINDOW_H

#include <switch.h>

void dcr_window_size(int *w, int *h);    /* the rendering size */
void dcr_window_set_size(int w, int h);  /* ignored unless both > 0 */
void dcr_window_prepare(void);           /* the default window at that size */

/* Size, crop and swap interval of w, unless the on-screen boot log still owns
 * the window (android_ndk.c's ANativeWindow calls this). */
void rt_window_set_geom(NWindow *w, u32 width, u32 height);

#endif /* RT_WINDOW_H */
