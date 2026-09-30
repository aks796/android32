/* rt_audout.c -- the sound output every port shares: audout, three buffers.
 *
 * On Android the game writes PCM to an AudioTrack (or its mixer runs behind
 * OpenSL ES, or a SoundPool) and a blocking write paces it. Here the port
 * turns that into the device's 48 kHz stereo s16 and submits buffers of
 * RT_AUDOUT_FRAMES frames; three are queued, and a submit waits while all
 * three are (rt_audout_submit), which is the same pacing.
 *
 * audout's buffer descriptor is an IPC structure with 64-bit fields for every
 * client, while libnx32's AudioOutBuffer had 32-bit pointers, so append and
 * get-released are issued here with the right layout (the self-test below
 * proved it on hardware, and every port has used it since). libnx32 4.12.0 has
 * the fixed descriptor; the raw calls stay, as they reap all three buffers in
 * one call. MIT.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "rt_audout.h"
#include "util.h"

typedef struct {
  u64 next, buffer, buffer_size, data_size, data_offset;
} AoBuf;
_Static_assert(sizeof(AoBuf) == 0x28, "audout buffer descriptor");

#define NBUF 3
#define FRAME_BYTES (RT_AUDOUT_FRAMES * 4)
#define BUF_BYTES ((FRAME_BYTES + 0xFFF) & ~0xFFF) /* whole pages: 1024 frames = one */
/* the wait for a free buffer: a fraction of one buffer's time (10.7 ms at
 * 512 frames, 21 ms at 1024) */
#define WAIT_NS (RT_AUDOUT_FRAMES <= 512 ? 1000000ll : 2000000ll)

static AoBuf g_bufs[NBUF] __attribute__((aligned(16)));
static int16_t *g_pcm[NBUF];
static int g_queued[NBUF];
static int g_ao_ready;
static u32 g_out_rate = 48000;
static volatile int g_cancel;
static unsigned long g_underruns, g_append_fails, g_dropped, g_submits;

static Result ao_append(AoBuf *b) {
  u64 tag = (u64)(uintptr_t)b;
  const bool auto_ = hosversionAtLeast(3, 0, 0);
  return serviceDispatchIn(audoutGetServiceSession_AudioOut(), auto_ ? 7 : 3, tag,
                           .buffer_attrs = {auto_ ? (SfBufferAttr_HipcAutoSelect | SfBufferAttr_In)
                                                  : (SfBufferAttr_HipcMapAlias | SfBufferAttr_In)},
                           .buffers = {{b, sizeof(*b)}});
}

static Result ao_released(u64 *tags, u32 max, u32 *count) {
  const bool auto_ = hosversionAtLeast(3, 0, 0);
  return serviceDispatchOut(audoutGetServiceSession_AudioOut(), auto_ ? 8 : 5, *count,
                            .buffer_attrs = {auto_ ? (SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out)
                                                   : (SfBufferAttr_HipcMapAlias | SfBufferAttr_Out)},
                            .buffers = {{tags, max * sizeof(u64)}});
}

/* Mark the buffers the audio server has finished with as free again. */
static void reap(void) {
  u64 tags[NBUF] = {0};
  u32 n = 0;
  if (R_SUCCEEDED(ao_released(tags, NBUF, &n)))
    for (u32 k = 0; k < n && k < NBUF; k++)
      for (int i = 0; i < NBUF; i++)
        if (tags[k] == (u64)(uintptr_t)&g_bufs[i])
          g_queued[i] = 0;
}

/* Index of a free buffer (reaping only when none is), or -1. */
static int free_buffer(void) {
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < NBUF; i++)
      if (!g_queued[i])
        return i;
    reap();
  }
  return -1;
}

int rt_audout_open(void) {
  if (g_ao_ready)
    return 0;
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    debugPrintf("[audio] audoutInitialize failed 0x%x\n", rc);
    return -1;
  }
  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) {
    debugPrintf("[audio] audoutStartAudioOut failed 0x%x\n", rc);
    audoutExit();
    return -1;
  }
  g_out_rate = audoutGetSampleRate() ? audoutGetSampleRate() : 48000;
  for (int i = 0; i < NBUF; i++) {
    if (!g_pcm[i])
      g_pcm[i] = memalign(0x1000, BUF_BYTES);
    if (!g_pcm[i])
      return -1;
    memset(g_pcm[i], 0, BUF_BYTES);
    g_bufs[i].buffer = (u64)(uintptr_t)g_pcm[i];
    g_bufs[i].buffer_size = BUF_BYTES;
    g_bufs[i].data_size = FRAME_BYTES;
    g_queued[i] = 0;
  }
  g_ao_ready = 1;
  debugPrintf("[audio] audout open: %u Hz, %u ch; %d x %d-frame buffers\n", (unsigned)g_out_rate,
              (unsigned)audoutGetChannelCount(), NBUF, RT_AUDOUT_FRAMES);
  return 0;
}

unsigned rt_audout_rate(void) { return g_out_rate; }

void rt_audout_cancel(int on) { g_cancel = on; }

int rt_audout_submit(const int16_t *frames) {
  if (!g_ao_ready)
    return -1;
  int i;
  reap();
  int queued = 0;
  for (int k = 0; k < NBUF; k++)
    queued += g_queued[k];
  if (!queued && g_submits > NBUF)
    g_underruns++; /* audout had nothing left to play */
  while ((i = free_buffer()) < 0 && !g_cancel)
    svcSleepThread(WAIT_NS);
  if (i < 0)
    return -1;
  memcpy(g_pcm[i], frames, FRAME_BYTES);
  armDCacheFlush(g_pcm[i], FRAME_BYTES);
  g_bufs[i].data_size = FRAME_BYTES;
  g_bufs[i].data_offset = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    Result rc = ao_append(&g_bufs[i]);
    if (R_SUCCEEDED(rc)) {
      g_queued[i] = 1;
      g_submits++;
      return 0;
    }
    if (g_append_fails++ < 3)
      debugPrintf("[audio] audout append failed 0x%x (retrying)\n", (unsigned)rc);
    svcSleepThread(2000000ll);
    reap();
  }
  g_dropped++;
  return -1;
}

void rt_audout_close(void) {
  if (!g_ao_ready)
    return;
  audoutStopAudioOut();
  audoutExit();
  g_ao_ready = 0;
  for (int i = 0; i < NBUF; i++)
    g_queued[i] = 0;
  debugPrintf("[audio] audout closed after %lu buffers (%lu underruns, %lu dropped)\n", g_submits,
              g_underruns, g_dropped);
}

void rt_audout_stats(RtAudoutStats *out) {
  out->submits = g_submits;
  out->underruns = g_underruns;
  out->append_fails = g_append_fails;
  out->dropped = g_dropped;
}

void rt_audout_selftest(void) {
  if (rt_audout_open() != 0)
    return;
  static int16_t silence[RT_AUDOUT_FRAMES * 2];
  rt_audout_submit(silence);
  rt_audout_submit(silence);
  u64 t0 = armGetSystemTick();
  int back = 0;
  while (armTicksToNs(armGetSystemTick() - t0) < 500000000ull) {
    reap();
    back = 0;
    for (int i = 0; i < NBUF; i++)
      back += !g_queued[i];
    if (back == NBUF)
      break;
    svcSleepThread(5000000ll);
  }
  debugPrintf("[audio] self-test: %s (%d/%d buffers returned in %llu ms)\n",
              back == NBUF ? "OK" : "FAILED -- buffer descriptor not accepted", back, NBUF,
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
}

__attribute__((weak)) void port_audio_selftest(void) { rt_audout_selftest(); }

/* ------------------------------------------------------------ the pump
 * A mixer on a thread of its own, above the game's threads, so a buffer is
 * always ready before audout runs dry. */
static Thread g_pump;
static int g_pump_up;
static volatile int g_pump_stop, g_paused;
static volatile unsigned long g_blocks;
static rt_audout_fill_fn g_fill;
static void *g_fill_ud;

static void pump(void *arg) {
  (void)arg;
  static int16_t block[RT_AUDOUT_FRAMES * 2];
  while (!g_pump_stop) {
    if (g_paused) {
      svcSleepThread(10000000ll);
      continue;
    }
    g_fill(block, RT_AUDOUT_FRAMES, g_fill_ud);
    g_blocks++;
    rt_audout_submit(block); /* waits while every buffer is queued: the pacing */
  }
}

int rt_audout_pump_start(rt_audout_fill_fn fill, void *ud, int prio, int core) {
  if (g_pump_up)
    return 0;
  if (!fill || rt_audout_open() != 0)
    return -1;
  g_fill = fill, g_fill_ud = ud;
  g_pump_stop = 0;
  Result rc = threadCreate(&g_pump, pump, NULL, NULL, 0x10000, prio, core);
  if (R_SUCCEEDED(rc)) {
    rc = threadStart(&g_pump);
    if (R_FAILED(rc))
      threadClose(&g_pump);
  }
  if (R_FAILED(rc)) {
    debugPrintf("[audio] mixer thread: 0x%x -- no sound\n", (unsigned)rc);
    return -1;
  }
  g_pump_up = 1;
  debugPrintf("[audio] mixer thread running (%u Hz, priority 0x%x)\n", (unsigned)g_out_rate, (unsigned)prio);
  return 0;
}

void rt_audout_pause(int paused) { g_paused = paused; }

void rt_audout_pump_stop(void) {
  if (!g_pump_up)
    return;
  g_pump_stop = 1;
  rt_audout_cancel(1);
  threadWaitForExit(&g_pump);
  threadClose(&g_pump);
  rt_audout_cancel(0);
  g_pump_up = 0;
}

unsigned long rt_audout_pump_blocks(void) { return g_blocks; }
