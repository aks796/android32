/* opensles.c -- OpenSL ES for the game's sound, played through audout.
 *
 * Gameloft's vox audio engine picks its Android output by API level, and the
 * engine's JNI_OnLoad sets that to 9 (VoxSetAndroidAPILevel(9)): so it always
 * takes vox::DriverAndroid::_InitOSL, as on every phone, never the Java
 * AudioTrack path. What _InitOSL does (disassembled):
 *
 *   slCreateEngine -> Realize -> GetInterface(SL_IID_ENGINE)
 *   CreateOutputMix -> Realize
 *   CreateAudioPlayer(src = buffer queue locator + PCM 44.1 kHz s16 stereo,
 *                     sink = the output mix, 1 interface: the buffer queue)
 *   -> Realize -> GetInterface(SL_IID_PLAY), GetInterface(<the queue>)
 *   -> RegisterCallback(CallbackOSL, driver)
 *   DoCallbackOSL() (mixes 1024 frames, Enqueue), SetPlayState(PLAYING)
 *
 * and from then on vox mixes and enqueues the next 4 KB block inside the
 * buffer-queue callback. So this file is that much of OpenSL ES, honestly
 * implemented: objects with their interfaces (each an SLxxxItf, a pointer to
 * a vtable pointer), a player whose queue an audio thread drains -- each
 * buffer resampled to the device's 48 kHz, queued to audout (which blocks
 * while three 1024-frame buffers are pending: the pacing), then dequeued, and
 * the callback called, as Android's OpenSL does when a buffer has played. The
 * microphone recorder (_InitRecordOSL) is refused.
 *
 * The audout half (buffer descriptors with the IPC layout, the self-test) is
 * the Crossy Road / PvZ ports'. MIT.
 */
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <switch.h>

#include "util.h"

/* =============================================================== audout */
typedef struct {
  u64 next, buffer, buffer_size, data_size, data_offset;
} AoBuf;
_Static_assert(sizeof(AoBuf) == 0x28, "audout buffer descriptor");

#define NBUF 3
#define FRAMES_PER_BUF 1024              /* 1024 * 4 bytes = one 0x1000 page */
#define BUF_BYTES (FRAMES_PER_BUF * 4)

static AoBuf g_bufs[NBUF] __attribute__((aligned(16)));
static int16_t *g_pcm[NBUF];
static int g_queued[NBUF];
static int g_ao_ready;
static u32 g_out_rate = 48000;

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

static void reap(void) {
  u64 tags[NBUF] = {0};
  u32 n = 0;
  if (R_SUCCEEDED(ao_released(tags, NBUF, &n)))
    for (u32 k = 0; k < n && k < NBUF; k++)
      for (int i = 0; i < NBUF; i++)
        if (tags[k] == (u64)(uintptr_t)&g_bufs[i])
          g_queued[i] = 0;
}

static int free_buffer(void) {
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < NBUF; i++)
      if (!g_queued[i])
        return i;
    reap();
  }
  return -1;
}

static int ao_open(void) {
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
    g_pcm[i] = memalign(0x1000, BUF_BYTES);
    if (!g_pcm[i])
      return -1;
    memset(g_pcm[i], 0, BUF_BYTES);
    g_bufs[i].buffer = (u64)(uintptr_t)g_pcm[i];
    g_bufs[i].buffer_size = BUF_BYTES;
    g_bufs[i].data_size = BUF_BYTES;
  }
  g_ao_ready = 1;
  debugPrintf("[audio] audout open: %u Hz, %u ch\n", (unsigned)g_out_rate,
              (unsigned)audoutGetChannelCount());
  return 0;
}

static unsigned long g_underruns, g_append_fails, g_dropped, g_submits;

/* Queue one full buffer of 48 kHz stereo s16; blocks while all are in use. */
static void submit(const int16_t *frames) {
  int i;
  reap();
  int queued = 0;
  for (int k = 0; k < NBUF; k++)
    queued += g_queued[k];
  if (!queued && g_submits > NBUF)
    g_underruns++;
  while ((i = free_buffer()) < 0)
    svcSleepThread(2000000ll);
  memcpy(g_pcm[i], frames, BUF_BYTES);
  armDCacheFlush(g_pcm[i], BUF_BYTES);
  g_bufs[i].data_size = BUF_BYTES;
  g_bufs[i].data_offset = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    Result rc = ao_append(&g_bufs[i]);
    if (R_SUCCEEDED(rc)) {
      g_queued[i] = 1;
      g_submits++;
      return;
    }
    if (g_append_fails++ < 3)
      debugPrintf("[audio] audout append failed 0x%x (retrying)\n", (unsigned)rc);
    svcSleepThread(2000000ll);
    reap();
  }
  g_dropped++;
}

/* For the start screen (a8r_menu_audio.c), before the game has a player:
 * the same audout, 48 kHz stereo s16 in buffers of DCR_AUDIO_FRAMES. */
int dcr_audio_out_open(void) { return ao_open(); }
unsigned dcr_audio_out_rate(void) { return g_out_rate; }
void dcr_audio_out_submit(const int16_t *frames) { submit(frames); }

void dcr_audio_selftest(void) {
  if (ao_open() != 0)
    return;
  static int16_t silence[FRAMES_PER_BUF * 2];
  submit(silence);
  submit(silence);
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

/* ================================================================ OpenSL */
typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef uint8_t SLboolean_t; /* SLboolean is a 32-bit value in the ABI: passed as SLuint32 */

#define SL_RESULT_SUCCESS 0x0
#define SL_RESULT_PRECONDITIONS_VIOLATED 0x1
#define SL_RESULT_PARAMETER_INVALID 0x2
#define SL_RESULT_MEMORY_FAILURE 0x3
#define SL_RESULT_BUFFER_INSUFFICIENT 0x7
#define SL_RESULT_CONTENT_UNSUPPORTED 0x9
#define SL_RESULT_FEATURE_UNSUPPORTED 0xC

#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATALOCATOR_BUFFERQUEUE 0x6
#define SL_DATALOCATOR_OUTPUTMIX 0x4
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATAFORMAT_PCM 0x2
#define SL_BYTEORDER_LITTLEENDIAN 0x2

/* SLInterfaceID values: pointers to GUIDs (the SL_IID_* imports hold them). */
typedef struct { uint32_t d1; uint16_t d2, d3, d4; uint8_t n[6]; } SLGuid;
static const SLGuid k_iid_engine = {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_play = {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_bq = {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_asbq = {0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_record = {0xc5657aa0, 0xdddb, 0x11db, 0x82f7, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
const void *b_SL_IID_ENGINE = &k_iid_engine;
const void *b_SL_IID_PLAY = &k_iid_play;
const void *b_SL_IID_BUFFERQUEUE = &k_iid_bq;
const void *b_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &k_iid_asbq;
const void *b_SL_IID_RECORD = &k_iid_record;

static int iid_is(const void *iid, const SLGuid *g) {
  return iid == g || (iid && !memcmp(iid, g, sizeof *g));
}

typedef struct SLObj SLObj;
/* One interface: `self` in every call is the address of such a slot. */
typedef struct {
  const void *vtbl;
  SLObj *o;
} Slot;

enum { K_ENGINE, K_MIX, K_PLAYER };

typedef void (*BqCallback)(const void *caller, void *ctx);

#define QCAP 16

struct SLObj {
  Slot obj;      /* SLObjectItf */
  Slot engine;   /* SLEngineItf (engine) */
  Slot play;     /* SLPlayItf (player) */
  Slot bq;       /* SLBufferQueueItf / SLAndroidSimpleBufferQueueItf (player) */
  int kind, state;
  /* player */
  Mutex lock;
  CondVar cv;
  volatile uint32_t play_state;
  struct { const void *buf; uint32_t size; } q[QCAP];
  uint32_t qcap, head, count, index;
  BqCallback cb;
  void *cb_ctx;
  int channels, bits;
  uint32_t rate;
  volatile int dead;
};

/* ------------------------------------------------------- the audio thread */
static SLObj *volatile g_player;  /* the one player being played */
static volatile int g_paused, g_closing, g_in_callback;
static volatile uint32_t g_writes;
static int16_t g_out[FRAMES_PER_BUF * 2];
static int g_out_n;
static double g_pos;
static int16_t g_prev[2];
static Thread g_thread;
static int g_thread_up;
static unsigned long g_frames_in;
static u64 g_t_first;

uint32_t dcr_audio_writes(void) { return g_writes; }
void dcr_audio_pause(int paused) { g_paused = paused; }
void dcr_audio_close(void) {
  g_closing = 1;
  SLObj *p = g_player;
  if (p) {
    mutexLock(&p->lock);
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
  }
}

static inline void frame_at(const uint8_t *in, int i, int ch, int bits, int16_t out[2]) {
  if (i < 0) {
    out[0] = g_prev[0], out[1] = g_prev[1];
    return;
  }
  if (bits == 16) {
    const int16_t *s = (const int16_t *)in + i * ch;
    out[0] = s[0];
    out[1] = ch == 2 ? s[1] : s[0];
  } else {
    const uint8_t *s = in + i * ch;
    out[0] = (int16_t)(((int)s[0] - 128) << 8);
    out[1] = ch == 2 ? (int16_t)(((int)s[1] - 128) << 8) : out[0];
  }
}

/* Linear resampling to the device rate, continuous across buffers (index -1
 * is the previous buffer's last frame). submit() blocks: the pacing. */
static void play_pcm(const uint8_t *in, uint32_t size, uint32_t rate, int ch, int bits) {
  const int frame_bytes = ch * (bits / 8);
  const int frames = (int)(size / (uint32_t)frame_bytes);
  if (frames <= 0)
    return;
  g_frames_in += (unsigned long)frames;
  const double step = (double)rate / (double)g_out_rate;
  int16_t a[2], b[2];
  while (g_pos < (double)(frames - 1)) {
    int i0 = (int)floor(g_pos);
    double t = g_pos - (double)i0;
    frame_at(in, i0, ch, bits, a);
    frame_at(in, i0 + 1, ch, bits, b);
    g_out[g_out_n * 2] = (int16_t)((double)a[0] + (double)(b[0] - a[0]) * t);
    g_out[g_out_n * 2 + 1] = (int16_t)((double)a[1] + (double)(b[1] - a[1]) * t);
    if (++g_out_n == FRAMES_PER_BUF) {
      submit(g_out);
      g_out_n = 0;
    }
    g_pos += step;
  }
  g_pos -= (double)frames;
  frame_at(in, frames - 1, ch, bits, g_prev);
}

static void audio_thread(void *arg) {
  (void)arg;
  while (!g_closing) {
    SLObj *p = g_player;
    if (!p || g_paused) {
      svcSleepThread(10000000ll);
      continue;
    }
    mutexLock(&p->lock);
    while (!g_closing && !p->dead && (p->play_state != SL_PLAYSTATE_PLAYING || !p->count || g_paused))
      condvarWaitTimeout(&p->cv, &p->lock, 20000000ll);
    if (g_closing || p->dead) {
      mutexUnlock(&p->lock);
      continue;
    }
    const void *buf = p->q[p->head].buf;
    uint32_t size = p->q[p->head].size, rate = p->rate;
    int ch = p->channels, bits = p->bits;
    mutexUnlock(&p->lock);

    if (!g_writes++) {
      g_t_first = armGetSystemTick();
      debugPrintf("[audio] first buffer: %u bytes (%u Hz, %d ch, %d bit)\n", (unsigned)size,
                  (unsigned)rate, ch, bits);
    }
    play_pcm(buf, size, rate, ch, bits);

    mutexLock(&p->lock);
    BqCallback cb = NULL;
    void *ctx = NULL;
    if (p->count) { /* not Clear()ed meanwhile */
      p->head = (p->head + 1) % p->qcap;
      p->count--;
      p->index++;
      cb = p->cb;
      ctx = p->cb_ctx;
    }
    g_in_callback = 1;
    mutexUnlock(&p->lock);
    if (cb && !p->dead)
      cb(&p->bq, ctx); /* vox mixes the next block and enqueues it */
    g_in_callback = 0;

    if (g_writes % 3000 == 0) {
      double secs = (double)armTicksToNs(armGetSystemTick() - g_t_first) / 1e9;
      debugPrintf("[audio] %lu buffers; pulled at %.0f Hz over %.0f s; %lu underruns, %lu failed "
                  "submits (%lu dropped)\n",
                  (unsigned long)g_writes, secs > 0 ? (double)g_frames_in / secs : 0.0, secs,
                  g_underruns, g_append_fails, g_dropped);
    }
  }
}

static void start_thread(void) {
  if (g_thread_up)
    return;
  /* Above the game's threads (priority 59, dcr_sched.c): the mixing callback
   * is short, and a late buffer is an audible click. */
  if (R_FAILED(threadCreate(&g_thread, audio_thread, NULL, NULL, 0x10000, 0x2A, 2)) ||
      R_FAILED(threadStart(&g_thread))) {
    debugPrintf("[audio] could not start the audio thread\n");
    return;
  }
  g_thread_up = 1;
}

/* ---------------------------------------------------------- SLObjectItf */
static SLObj *obj_of(const void *self) { return self ? ((const Slot *)self)->o : NULL; }

static SLresult o_Realize(const void *self, SLuint32 async) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  if (o->state == SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  o->state = SL_OBJECT_STATE_REALIZED;
  if (o->kind == K_ENGINE)
    ao_open();
  if (o->kind == K_PLAYER)
    start_thread();
  return SL_RESULT_SUCCESS;
}
static SLresult o_Resume(const void *self, SLuint32 async) { return SL_RESULT_SUCCESS; }
static SLresult o_GetState(const void *self, SLuint32 *state) {
  SLObj *o = obj_of(self);
  if (!o || !state)
    return SL_RESULT_PARAMETER_INVALID;
  *state = (SLuint32)o->state;
  return SL_RESULT_SUCCESS;
}
static SLresult o_GetInterface(const void *self, const void *iid, void *out) {
  SLObj *o = obj_of(self);
  if (!o || !out)
    return SL_RESULT_PARAMETER_INVALID;
  if (o->state != SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  const void *itf = NULL;
  if (o->kind == K_ENGINE && iid_is(iid, &k_iid_engine))
    itf = &o->engine;
  else if (o->kind == K_PLAYER && iid_is(iid, &k_iid_play))
    itf = &o->play;
  else if (o->kind == K_PLAYER && (iid_is(iid, &k_iid_bq) || iid_is(iid, &k_iid_asbq)))
    itf = &o->bq;
  if (!itf) {
    *(const void **)out = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  *(const void **)out = itf;
  return SL_RESULT_SUCCESS;
}
static SLresult o_RegisterCallback(const void *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void o_AbortAsyncOperation(const void *self) {}
static void o_Destroy(const void *self) {
  SLObj *o = obj_of(self);
  if (!o)
    return;
  if (o->kind == K_PLAYER) {
    mutexLock(&o->lock);
    o->dead = 1;
    o->play_state = SL_PLAYSTATE_STOPPED;
    o->count = 0;
    condvarWakeAll(&o->cv);
    mutexUnlock(&o->lock);
    if (g_player == o) {
      g_player = NULL;
      while (g_in_callback)
        svcSleepThread(1000000ll);
    }
    debugPrintf("[audio] player destroyed\n");
    return; /* kept: the audio thread may still hold a pointer to it */
  }
  free(o);
}
static SLresult o_SetPriority(const void *self, int32_t prio, SLuint32 preempt) { return SL_RESULT_SUCCESS; }
static SLresult o_GetPriority(const void *self, int32_t *prio, SLuint32 *preempt) {
  if (prio)
    *prio = 0;
  if (preempt)
    *preempt = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult o_SetLossOfControlInterfaces(const void *self, int16_t n, const void **ids, SLuint32 on) {
  return SL_RESULT_SUCCESS;
}
static const void *const k_obj_vt[] = {
    o_Realize, o_Resume, o_GetState, o_GetInterface, o_RegisterCallback, o_AbortAsyncOperation,
    o_Destroy, o_SetPriority, o_GetPriority, o_SetLossOfControlInterfaces,
};

/* ------------------------------------------------------------ SLPlayItf */
static SLresult p_SetPlayState(const void *self, SLuint32 st) {
  SLObj *o = obj_of(self);
  if (!o || st < SL_PLAYSTATE_STOPPED || st > SL_PLAYSTATE_PLAYING)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->play_state = st;
  if (st == SL_PLAYSTATE_STOPPED) { /* stopping returns every queued buffer */
    o->count = 0;
    o->index = 0;
  }
  if (st == SL_PLAYSTATE_PLAYING)
    g_player = o;
  condvarWakeAll(&o->cv);
  mutexUnlock(&o->lock);
  static int logged;
  if (logged++ < 6)
    debugPrintf("[audio] SetPlayState(%s)\n",
                st == SL_PLAYSTATE_PLAYING ? "playing" : st == SL_PLAYSTATE_PAUSED ? "paused" : "stopped");
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetPlayState(const void *self, SLuint32 *st) {
  SLObj *o = obj_of(self);
  if (!o || !st)
    return SL_RESULT_PARAMETER_INVALID;
  *st = o->play_state;
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetDuration(const void *self, SLuint32 *ms) {
  if (ms)
    *ms = 0xFFFFFFFFu; /* SL_TIME_UNKNOWN */
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetPosition(const void *self, SLuint32 *ms) {
  SLObj *o = obj_of(self);
  if (ms)
    *ms = o && o->rate ? (SLuint32)((u64)g_frames_in * 1000ull / o->rate) : 0;
  return SL_RESULT_SUCCESS;
}
static SLresult p_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult p_ok(void) { return SL_RESULT_SUCCESS; }
static const void *const k_play_vt[] = {
    p_SetPlayState, p_GetPlayState, p_GetDuration, p_GetPosition,
    p_ok /* RegisterCallback */, p_ok /* SetCallbackEventsMask */, p_ok /* GetCallbackEventsMask */,
    p_unsupported /* SetMarkerPosition */, p_ok /* ClearMarkerPosition */,
    p_unsupported /* GetMarkerPosition */, p_ok /* SetPositionUpdatePeriod */,
    p_unsupported /* GetPositionUpdatePeriod */,
};

/* ------------------------------------------------ SL(Android)BufferQueueItf */
static SLresult q_Enqueue(const void *self, const void *buf, SLuint32 size) {
  SLObj *o = obj_of(self);
  if (!o || !buf || !size)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  if (o->count >= o->qcap) {
    mutexUnlock(&o->lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  uint32_t tail = (o->head + o->count) % o->qcap;
  o->q[tail].buf = buf;
  o->q[tail].size = size;
  o->count++;
  condvarWakeAll(&o->cv);
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_Clear(const void *self) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->count = 0;
  o->index = 0;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_GetState(const void *self, SLuint32 *st) {
  SLObj *o = obj_of(self);
  if (!o || !st)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  st[0] = o->count;
  st[1] = o->index;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_RegisterCallback(const void *self, BqCallback cb, void *ctx) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->cb = cb;
  o->cb_ctx = ctx;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static const void *const k_bq_vt[] = {q_Enqueue, q_Clear, q_GetState, q_RegisterCallback};

/* ---------------------------------------------------------- SLEngineItf */
static SLObj *new_obj(int kind) {
  SLObj *o = calloc(1, sizeof *o);
  if (!o)
    return NULL;
  o->kind = kind;
  o->state = SL_OBJECT_STATE_UNREALIZED;
  o->obj = (Slot){k_obj_vt, o};
  o->engine = (Slot){NULL, o};
  o->play = (Slot){k_play_vt, o};
  o->bq = (Slot){k_bq_vt, o};
  mutexInit(&o->lock);
  condvarInit(&o->cv);
  o->play_state = SL_PLAYSTATE_STOPPED;
  return o;
}

static SLresult e_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult e_CreateOutputMix(const void *self, const void **out, SLuint32 n, const void **ids,
                                  const SLuint32 *req) {
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  SLObj *o = new_obj(K_MIX);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  *out = &o->obj;
  return SL_RESULT_SUCCESS;
}

typedef struct { void *locator, *format; } SLDataSource;
typedef struct {
  SLuint32 type, channels, milli_hz, bits, container, mask, endian;
} SLDataFormat_PCM;

static SLresult e_CreateAudioPlayer(const void *self, const void **out, const SLDataSource *src,
                                    const SLDataSource *snk, SLuint32 n, const void **ids,
                                    const SLuint32 *req) {
  if (!out || !src || !src->locator || !src->format)
    return SL_RESULT_PARAMETER_INVALID;
  const SLuint32 *loc = src->locator;
  const SLDataFormat_PCM *fmt = src->format;
  if ((loc[0] != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && loc[0] != SL_DATALOCATOR_BUFFERQUEUE) ||
      fmt->type != SL_DATAFORMAT_PCM || (fmt->channels != 1 && fmt->channels != 2) ||
      (fmt->bits != 8 && fmt->bits != 16)) {
    debugPrintf("[audio] CreateAudioPlayer: locator 0x%x format %u (%u ch, %u bit) not supported\n",
                (unsigned)loc[0], (unsigned)fmt->type, (unsigned)fmt->channels, (unsigned)fmt->bits);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  for (SLuint32 i = 0; i < n; i++) {
    int known = iid_is(ids[i], &k_iid_bq) || iid_is(ids[i], &k_iid_asbq) || iid_is(ids[i], &k_iid_play);
    if (!known && req && req[i]) {
      debugPrintf("[audio] CreateAudioPlayer: a required interface is not provided\n");
      return SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }
  SLObj *o = new_obj(K_PLAYER);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  o->qcap = loc[1] ? (loc[1] < QCAP ? loc[1] : QCAP) : 2;
  o->channels = (int)fmt->channels;
  o->bits = (int)fmt->bits;
  o->rate = fmt->milli_hz / 1000u;
  if (!o->rate)
    o->rate = 44100;
  *out = &o->obj;
  debugPrintf("[audio] OpenSL player: %u Hz, %d ch, %d bit, %u buffers queued at most -> audout %u Hz\n",
              (unsigned)o->rate, o->channels, o->bits, (unsigned)o->qcap, (unsigned)g_out_rate);
  return SL_RESULT_SUCCESS;
}

static SLresult e_CreateAudioRecorder(void) {
  debugPrintf("[audio] CreateAudioRecorder: no microphone\n");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult e_QueryNumSupportedInterfaces(const void *self, SLuint32 id, SLuint32 *n) {
  if (n)
    *n = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult e_QueryNumSupportedExtensions(const void *self, SLuint32 *n) {
  if (n)
    *n = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult e_IsExtensionSupported(const void *self, const char *name, SLuint32 *yes) {
  if (yes)
    *yes = 0;
  return SL_RESULT_SUCCESS;
}
static const void *const k_engine_vt[] = {
    e_unsupported,         /* CreateLEDDevice */
    e_unsupported,         /* CreateVibraDevice */
    e_CreateAudioPlayer,   e_CreateAudioRecorder,
    e_unsupported,         /* CreateMidiPlayer */
    e_unsupported,         /* CreateListener */
    e_unsupported,         /* Create3DGroup */
    e_CreateOutputMix,
    e_unsupported,         /* CreateMetadataExtractor */
    e_unsupported,         /* CreateExtensionObject */
    e_QueryNumSupportedInterfaces,
    e_unsupported,         /* QuerySupportedInterfaces */
    e_QueryNumSupportedExtensions,
    e_unsupported,         /* QuerySupportedExtension */
    e_IsExtensionSupported,
};

SLresult b_slCreateEngine(const void **out, SLuint32 nopts, const void *opts, SLuint32 n,
                          const void **ids, const SLuint32 *req) {
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  SLObj *o = new_obj(K_ENGINE);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  o->engine.vtbl = k_engine_vt;
  *out = &o->obj;
  debugPrintf("[audio] slCreateEngine: OpenSL ES over audout\n");
  return SL_RESULT_SUCCESS;
}
