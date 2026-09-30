/* dcr_boost.h -- the CPU at full speed while the engine is stuck in a long
 * frame, and at start-up (dcr_boost.c). The frame loop brackets each frame
 * (the mesa renderer does it at eglSwapBuffers unless RT_GL_SWAP_ENDS_FRAME
 * is 0), something polls it, and main() starts the launch boost. MIT.
 */
#ifndef DCR_BOOST_H
#define DCR_BOOST_H
#include <stddef.h>
#include <stdint.h>

/* The engine's frame starts / ends (gl_mesa.c's eglSwapBuffers, or the
 * port's frame loop). */
void dcr_boost_frame_begin(void);
void dcr_boost_frame_end(uint64_t frame);
/* Boost a frame that has run past 50 ms. Often: every vsync, or between
 * frames; RT_BOOST_WATCH_THREAD polls every 10 ms on a thread of its own. */
void dcr_boost_poll(void);
/* The game lost the focus (HOME, sleep): no frame is running until it is
 * back, and none of that time is a long frame. */
void dcr_boost_idle(void);

/* main(), once config.ini is read: the start-up boost (and the port's clocks,
 * port_perf_clocks). */
void dcr_boost_launch_begin(void);
/* The end of the start-up boost: the frame loop, after the first frame that
 * reached the screen -- or, with RT_BOOST_LAUNCH_AFTER_PICTURE_MS, these: */
void dcr_boost_launch_end(void);
/* the first frame that reached the screen (the launch goes on) */
void dcr_boost_first_picture(void);
/* the game says its title screen is up: the launch boost may end */
void dcr_launch_ready(void);
/* every frame: ends the launch boost once it is ready or the time is up */
void dcr_boost_launch_tick(void);

/* Keep the boost on while something asks (a video's decoding): 1 on, 0 off
 * (the next frame's end turns it off). */
void dcr_boost_hold(int on);

/* Log lines: the boosted frames so far (only when there is something new),
 * and (call it once a minute) where the CPU went and the SoC's temperature. */
void dcr_boost_report(void);
void dcr_boost_cpu_report(void);

/* ---------------------------------------------------------------- callbacks
 * Weak; the runtime's defaults keep appletSetCpuBoostMode(FastLoad). */

/* The port drives the CPU clock itself (clkrst): set the boost on / off and
 * return 1 (done) or 0 (no boost now); -1 = not the port's, use FastLoad. */
int port_cpu_boost_set(int on);
/* dcr_boost_launch_begin, first thing: the port sets its clocks. */
void port_perf_clocks(void);
/* A long frame passed 50 ms: the port snapshots its own counters... */
void port_boost_snapshot(void);
/* ...and appends them to that frame's log line (startup 0: since the
 * snapshot), or to the start-up's line (startup 1: since the start). Returns
 * the length written into out (cap bytes, NUL-terminated). */
int port_boost_format(char *out, size_t cap, int startup);

#endif
