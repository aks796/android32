/* android_ndk.c -- the libandroid.so / libjnigraphics.so / libOpenSLES.so
 * imports: ANativeWindow, ALooper, AConfiguration, ASensor, AInputEvent,
 * AndroidBitmap, and OpenSL ES refused.
 *
 *   ANativeWindow  the one window is libnx's default NWindow (rt_window.c).
 *                  The GL layer's eglCreateWindowSurface receives it back
 *                  unchanged.
 *   ALooper        a real looper: an engine may run its UI thread and an app
 *                  thread on loopers with pipes added by ALooper_addFd, and
 *                  post work between them through those pipes -- some of it
 *                  synchronously. So pollOnce watches the registered fds (our
 *                  in-memory pipes, bionic_io.c), runs their callbacks, and
 *                  otherwise sleeps on the pipes' activity futex. An engine
 *                  that only uses a looper to wait and wake (Unity) gets the
 *                  same: ALooper_wake bumps that futex.
 *   AConfiguration what the engine is told about the device (language,
 *                  density, orientation...).
 *   ASensor        no sensors: an empty list, no default sensor.
 *   AInputEvent    the getters read RtInputEvent below, for a port that hands
 *                  the engine key events of its own.
 *   AndroidBitmap  refused.
 *   OpenSL ES      refused (slCreateEngine fails) unless RT_OPENSLES is 1,
 *                  when opensles.c implements it: without it, an engine falls
 *                  back to its Java audio path, which the port plays.
 * MIT.
 */
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "rt_settings.h"
#include "rt_window.h"
#include "util.h"

/* ---------------------------------------------------------------- settings */
/* The main thread plays Android's UI thread, whose looper always exists (Java
 * prepared it): ALooper_forThread there makes one. 0: only threads that
 * called ALooper_prepare have one. dcr 0; lab2, abs, pvz, sonic, flappy, a8r
 * 1. */
#ifndef RT_LOOPER_MAIN_IMPLICIT
#define RT_LOOPER_MAIN_IMPLICIT 1
#endif
/* The locale AConfiguration reports until dcr_config_locale() sets one.
 * pvz "zh"/"CN"; the others "en"/"US". */
#ifndef RT_ACONFIG_LANG
#define RT_ACONFIG_LANG "en"
#endif
#ifndef RT_ACONFIG_COUNTRY
#define RT_ACONFIG_COUNTRY "US"
#endif
/* 1: opensles.c implements OpenSL ES; 0: slCreateEngine is refused here.
 * a8r 1; the others 0. (opensles.c reads it too, with the same default.) */
#ifndef RT_OPENSLES
#define RT_OPENSLES 0
#endif

/* ============================== ANativeWindow ============================== */
void *b_ANativeWindow_fromSurface(void *env, void *surface) {
  NWindow *w = nwindowGetDefault();
  int ww, wh;
  dcr_window_size(&ww, &wh);
  rt_window_set_geom(w, (u32)ww, (u32)wh);
  return w;
}
void b_ANativeWindow_acquire(void *w) {}
void b_ANativeWindow_release(void *w) {}
int32_t b_ANativeWindow_getWidth(void *w) {
  int ww;
  dcr_window_size(&ww, NULL);
  return ww;
}
int32_t b_ANativeWindow_getHeight(void *w) {
  int wh;
  dcr_window_size(NULL, &wh);
  return wh;
}
int32_t b_ANativeWindow_setBuffersGeometry(void *w, int32_t width, int32_t height, int32_t fmt) {
  /* 0x0 (with a format) keeps the window's own size. */
  if (width > 0 && height > 0) {
    debugPrintf("[window] setBuffersGeometry %dx%d fmt %d\n", width, height, fmt);
    rt_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  }
  return 0;
}

/* ================================= ALooper ================================= */
#define MAX_LOOPERS 32
#define MAX_LOOPER_FDS 16
#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT (-3)
#define ALOOPER_POLL_ERROR (-4)
#define ALOOPER_EVENT_INPUT 1
#define ALOOPER_EVENT_HANGUP 4

typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

typedef struct {
  int fd, ident, events;
  ALooper_callbackFunc cb;
  void *data;
} LooperFd;

typedef struct {
  Handle owner;
  Mutex m;
  volatile int wake;
  LooperFd fds[MAX_LOOPER_FDS];
  int nfds;
} Looper;

static Looper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;

/* from group B (bionic_io.c, bionic_io.h): the in-memory pipes' activity
 * futex, which ALooper_wake bumps too */
void dcr_fd_activity(void);
void dcr_fd_wait(uint32_t seen, s64 timeout_ns);
uint32_t dcr_fd_seq(void);
int dcr_fd_readable(int fd);


static Looper *looper_for(Handle h, int create) {
  mutexLock(&g_loopers_lock);
  Looper *free_slot = NULL;
  for (int i = 0; i < MAX_LOOPERS; i++) {
    if (g_loopers[i].owner == h) {
      mutexUnlock(&g_loopers_lock);
      return &g_loopers[i];
    }
    if (!g_loopers[i].owner && !free_slot)
      free_slot = &g_loopers[i];
  }
  if (create && free_slot) {
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->owner = h;
  }
  mutexUnlock(&g_loopers_lock);
  return create ? free_slot : NULL;
}

void *b_ALooper_prepare(int opts) { return looper_for(threadGetCurHandle(), 1); }

/* Other threads have a looper only after ALooper_prepare; the main thread's
 * exists from the start when RT_LOOPER_MAIN_IMPLICIT. */
void *b_ALooper_forThread(void) {
  Handle me = threadGetCurHandle();
  return looper_for(me, RT_LOOPER_MAIN_IMPLICIT && me == envGetMainThreadHandle());
}

void b_ALooper_acquire(void *l) {}
void b_ALooper_release(void *l) {}

void b_ALooper_wake(void *l) {
  Looper *L = l;
  if (!L)
    return;
  L->wake = 1;
  dcr_fd_activity();
}

int b_ALooper_addFd(void *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data) {
  Looper *L = l;
  if (!L)
    return -1;
  mutexLock(&L->m);
  int slot = -1;
  for (int i = 0; i < L->nfds; i++)
    if (L->fds[i].fd == fd)
      slot = i;
  if (slot < 0 && L->nfds < MAX_LOOPER_FDS)
    slot = L->nfds++;
  if (slot >= 0)
    L->fds[slot] = (LooperFd){fd, cb ? -2 : ident, events, cb, data};
  mutexUnlock(&L->m);
  debugPrintf("[looper] %p addFd(%d, ident %d, events %d, cb %p)\n", l, fd, ident, events, cb);
  return slot >= 0 ? 1 : -1;
}

int b_ALooper_removeFd(void *l, int fd) {
  Looper *L = l;
  if (!L)
    return -1;
  mutexLock(&L->m);
  int found = 0;
  for (int i = 0; i < L->nfds; i++)
    if (L->fds[i].fd == fd) {
      L->fds[i] = L->fds[--L->nfds];
      found = 1;
      break;
    }
  mutexUnlock(&L->m);
  return found;
}

/* One pass: run the callbacks of ready fds, or report the first ready ident.
 * Returns ALOOPER_POLL_CALLBACK / an ident >= 0, or ALOOPER_POLL_TIMEOUT if
 * nothing was ready. */
static int looper_scan(Looper *L, int *out_fd, int *out_events, void **out_data) {
  int ran = 0;
  for (int i = 0; i < L->nfds; i++) {
    mutexLock(&L->m);
    if (i >= L->nfds) {
      mutexUnlock(&L->m);
      break;
    }
    LooperFd e = L->fds[i];
    mutexUnlock(&L->m);
    int r = dcr_fd_readable(e.fd);
    if (r == 0)
      continue;
    if (r < 0) { /* closed without removeFd: forget it, as epoll does */
      debugPrintf("[looper] fd %d closed while registered: dropped\n", e.fd);
      b_ALooper_removeFd(L, e.fd);
      i--;
      continue;
    }
    int ev = r == 2 ? ALOOPER_EVENT_INPUT | ALOOPER_EVENT_HANGUP : ALOOPER_EVENT_INPUT;
    if (!e.cb) {
      if (out_fd) *out_fd = e.fd;
      if (out_events) *out_events = ev;
      if (out_data) *out_data = e.data;
      return e.ident;
    }
    ran = 1;
    if (e.cb(e.fd, ev, e.data) == 0) /* 0: unregister */
      b_ALooper_removeFd(L, e.fd);
  }
  return ran ? ALOOPER_POLL_CALLBACK : ALOOPER_POLL_TIMEOUT;
}

int b_ALooper_pollOnce(int timeout_ms, int *fd, int *events, void **data) {
  Looper *L = looper_for(threadGetCurHandle(), 1);
  if (fd) *fd = 0;
  if (events) *events = 0;
  if (data) *data = NULL;
  if (!L)
    return ALOOPER_POLL_ERROR;
  u64 deadline = timeout_ms > 0 ? armGetSystemTick() + armNsToTicks((u64)timeout_ms * 1000000ull) : 0;
  for (;;) {
    uint32_t seen = dcr_fd_seq();
    if (L->wake) {
      L->wake = 0;
      return ALOOPER_POLL_WAKE;
    }
    int r = looper_scan(L, fd, events, data);
    if (r != ALOOPER_POLL_TIMEOUT || timeout_ms == 0)
      return r;
    u64 now = armGetSystemTick();
    if (timeout_ms > 0 && now >= deadline)
      return ALOOPER_POLL_TIMEOUT;
    dcr_fd_wait(seen, timeout_ms < 0 ? -1 : (s64)armTicksToNs(deadline - now));
  }
}

int b_ALooper_pollAll(int timeout_ms, int *fd, int *events, void **data) {
  for (;;) {
    int r = b_ALooper_pollOnce(timeout_ms, fd, events, data);
    if (r != ALOOPER_POLL_CALLBACK)
      return r;
  }
}

/* The UI thread's loop (the port's frame loop, on the main thread): run due
 * callbacks, waiting at most timeout_ms. */
void dcr_looper_run_main(int timeout_ms) {
  Looper *L = looper_for(envGetMainThreadHandle(), 1);
  if (L && L->owner == threadGetCurHandle())
    b_ALooper_pollOnce(timeout_ms, NULL, NULL, NULL);
}

/* ============================== AConfiguration ============================= */
typedef struct {
  char lang[2], country[2];
  int32_t density;
} AConfig;

static char g_lang[3] = RT_ACONFIG_LANG, g_country[3] = RT_ACONFIG_COUNTRY;
static int32_t g_density = 213; /* tvdpi at 720p */

/* Set from config.ini before the engine starts (the port's boot). */
void dcr_config_locale(const char *lang, const char *country, int density) {
  if (lang && strlen(lang) == 2)
    memcpy(g_lang, lang, 3);
  if (country && strlen(country) == 2)
    memcpy(g_country, country, 3);
  if (density > 0)
    g_density = density;
}

void *b_AConfiguration_new(void) {
  AConfig *c = calloc(1, sizeof *c);
  if (c) {
    memcpy(c->lang, g_lang, 2);
    memcpy(c->country, g_country, 2);
    c->density = g_density;
  }
  return c;
}
void b_AConfiguration_delete(void *c) { free(c); }
void b_AConfiguration_fromAssetManager(void *c, void *am) {
  AConfig *k = c;
  if (k) {
    memcpy(k->lang, g_lang, 2);
    memcpy(k->country, g_country, 2);
    k->density = g_density;
  }
}
void b_AConfiguration_getLanguage(void *c, char *out) {
  if (out) memcpy(out, c ? ((AConfig *)c)->lang : g_lang, 2);
}
void b_AConfiguration_getCountry(void *c, char *out) {
  if (out) memcpy(out, c ? ((AConfig *)c)->country : g_country, 2);
}
int32_t b_AConfiguration_getDensity(void *c) { return c ? ((AConfig *)c)->density : g_density; }
int32_t b_AConfiguration_getMcc(void *c) { return 0; }
int32_t b_AConfiguration_getMnc(void *c) { return 0; }
int32_t b_AConfiguration_getOrientation(void *c) { return 2; }   /* LAND */
int32_t b_AConfiguration_getTouchscreen(void *c) { return 3; }   /* FINGER */
int32_t b_AConfiguration_getKeyboard(void *c) { return 1; }      /* NOKEYS */
int32_t b_AConfiguration_getNavigation(void *c) { return 2; }    /* DPAD */
int32_t b_AConfiguration_getKeysHidden(void *c) { return 1; }    /* YES */
int32_t b_AConfiguration_getNavHidden(void *c) { return 1; }     /* NO */
int32_t b_AConfiguration_getSdkVersion(void *c) { return 28; }
int32_t b_AConfiguration_getScreenSize(void *c) { return 3; }    /* LARGE */
int32_t b_AConfiguration_getScreenLong(void *c) { return 2; }    /* YES (16:9) */
int32_t b_AConfiguration_getUiModeType(void *c) { return 1; }    /* NORMAL (a phone) */
int32_t b_AConfiguration_getUiModeNight(void *c) { return 1; }   /* NO */

/* ================================= sensors ================================= */
/* No sensors are reported: tilt, where a game has it, is the port's own. */
static int g_sensor_manager;
static int g_sensor_queue;

void *b_ASensorManager_getInstance(void) { return &g_sensor_manager; }
int b_ASensorManager_getSensorList(void *m, const void ***list) {
  static const void *empty[1];
  if (list)
    *list = empty;
  return 0;
}
const void *b_ASensorManager_getDefaultSensor(void *m, int type) { return NULL; }
void *b_ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data) {
  return &g_sensor_queue;
}
int b_ASensorManager_destroyEventQueue(void *m, void *q) { return 0; }
int b_ASensorEventQueue_enableSensor(void *q, const void *s) { return -1; }
int b_ASensorEventQueue_disableSensor(void *q, const void *s) { return 0; }
int b_ASensorEventQueue_setEventRate(void *q, const void *s, int32_t us) { return 0; }
int b_ASensorEventQueue_hasEvents(void *q) { return 0; }
int b_ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { return 0; }
const char *b_ASensor_getName(const void *s) { return ""; }
const char *b_ASensor_getVendor(const void *s) { return ""; }
int b_ASensor_getType(const void *s) { return 0; }
float b_ASensor_getResolution(const void *s) { return 0.0f; }
int b_ASensor_getMinDelay(const void *s) { return 0; }

/* =============================== input events ============================== */
/* An AInputEvent* the engine is handed is one of these, made by the port. */
typedef struct {
  int32_t type;      /* AINPUT_EVENT_TYPE_KEY = 1 */
  int32_t device_id;
  int32_t action;    /* AKEY_EVENT_ACTION_DOWN = 0, UP = 1 */
  int32_t key_code;
  int32_t meta_state;
} RtInputEvent;

int32_t b_AInputEvent_getType(const RtInputEvent *e) { return e ? e->type : 0; }
int32_t b_AInputEvent_getDeviceId(const RtInputEvent *e) { return e ? e->device_id : 0; }
int32_t b_AKeyEvent_getAction(const RtInputEvent *e) { return e ? e->action : 0; }
int32_t b_AKeyEvent_getKeyCode(const RtInputEvent *e) { return e ? e->key_code : 0; }
int32_t b_AKeyEvent_getMetaState(const RtInputEvent *e) { return e ? e->meta_state : 0; }

/* =============================== AndroidBitmap ============================== */
int b_AndroidBitmap_getInfo(void *env, void *bmp, void *info) { return -1; }
int b_AndroidBitmap_lockPixels(void *env, void *bmp, void **addr) { return -1; }
int b_AndroidBitmap_unlockPixels(void *env, void *bmp) { return -1; }

/* ================================ OpenSL ES ================================ */
#if !RT_OPENSLES
/* SLInterfaceID values are pointers to 16-byte GUIDs; a caller only passes
 * them back to the engine object, which never exists. The same five
 * SL_IID_* opensles.c defines, so the import table does not depend on
 * RT_OPENSLES. */
static const uint32_t k_iid_engine[4] = {1}, k_iid_play[4] = {2}, k_iid_asbq[4] = {3};
static const uint32_t k_iid_bq[4] = {4}, k_iid_record[4] = {5};
const void *b_SL_IID_ENGINE = k_iid_engine;
const void *b_SL_IID_PLAY = k_iid_play;
const void *b_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = k_iid_asbq;
const void *b_SL_IID_BUFFERQUEUE = k_iid_bq;
const void *b_SL_IID_RECORD = k_iid_record;

uint32_t b_slCreateEngine(void **engine, uint32_t n, const void *opts, uint32_t ni,
                          const void *ids, const uint32_t *req) {
  if (engine)
    *engine = NULL;
  debugPrintf("[audio] slCreateEngine refused: sound goes through the game's Java audio\n");
  return 0x0C; /* SL_RESULT_FEATURE_UNSUPPORTED */
}
#endif
