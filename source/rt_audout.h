/* rt_audout.h -- the sound output: audout, the device's rate (48 kHz) stereo
 * s16, in buffers of RT_AUDOUT_FRAMES frames, three queued (rt_audout.c).
 * The port converts or mixes the game's sound into those buffers; the
 * blocking submit is what paces its mixer. MIT.
 */
#ifndef RT_AUDOUT_H
#define RT_AUDOUT_H
#include <stdint.h>

#include "rt_settings.h"

/* Frames in one buffer (4 bytes a frame). Three are queued, so this sets the
 * latency: 1024 = 64 ms of sound ahead. Values: flappy 512; the others 1024. */
#ifndef RT_AUDOUT_FRAMES
#define RT_AUDOUT_FRAMES 1024
#endif

/* audout started and the buffers made: 0, or -1 (logged). Later calls
 * return 0 at once. */
int rt_audout_open(void);
/* The device's sample rate (48000 until it is open). */
unsigned rt_audout_rate(void);
/* Queue RT_AUDOUT_FRAMES stereo frames, waiting while every buffer is
 * queued: 0 queued, -1 dropped (not open, cancelled while waiting, or audout
 * refused it five times). One thread submits. */
int rt_audout_submit(const int16_t *frames);
/* 1: a submit that waits for a buffer returns -1 at once (a closing game's
 * sound thread must not hang); 0: submits wait again. */
void rt_audout_cancel(int on);
/* Stop audout and let it go (rt_audout_open starts it again). */
void rt_audout_close(void);
/* Before the game runs: two buffers of silence must come back from the audio
 * server, or the buffer descriptor was not accepted. Logged. */
void rt_audout_selftest(void);

typedef struct {
  unsigned long submits, underruns, append_fails, dropped;
} RtAudoutStats;
void rt_audout_stats(RtAudoutStats *out);

/* ---- a mixer thread, for ports whose game hands over sounds rather than a
 * stream: fill(out, RT_AUDOUT_FRAMES, ud) writes the next buffer, then it is
 * submitted, over and over. prio above the game's threads (0x28 or 0x2A),
 * core as threadCreate's (-2: the default). 0 running, -1 failed (logged). */
typedef void (*rt_audout_fill_fn)(int16_t *out, int frames, void *ud);
int rt_audout_pump_start(rt_audout_fill_fn fill, void *ud, int prio, int core);
/* 1: the pump stops filling (HOME: the voices hold where they are). */
void rt_audout_pause(int paused);
/* The pump thread ended (a waiting submit is cancelled). */
void rt_audout_pump_stop(void);
/* Buffers the pump has filled (the watchdog's "is the sound alive"). */
unsigned long rt_audout_pump_blocks(void);

/* ---------------------------------------------------------------- callback
 * main() runs it at start-up; the runtime's weak default is
 * rt_audout_selftest(). A port whose sound is not audout's defines its own. */
void port_audio_selftest(void);

#endif
