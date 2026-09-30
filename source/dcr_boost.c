/* dcr_boost.c -- the CPU at full speed while the engine is stuck in a long frame.
 *
 * Loading (the start-up resource load, a level's first frame) is single frames
 * of hundreds of milliseconds of CPU work, at the Switch's 1020 MHz where the
 * TV boxes and phones this was built for run faster. Horizon has a mode for
 * exactly this, the one retail games use behind their loading screens:
 * appletSetCpuBoostMode(FastLoad) -- CPU 1785 MHz, GPU down to its minimum.
 * The GPU half is why it cannot simply stay on. So it is on only INSIDE a frame
 * that has already taken 50 ms (polled by the UI thread loop, pvz_boot.c), and
 * off again the moment that frame is presented (eglSwapBuffers, gl_mesa.c).
 *
 * The start-up is one long load, so there the boost is simply on: from the
 * moment config.ini is read until the first picture reaches the screen.
 *
 * Long frames are also written to the log, with the time they took, how much
 * of it was boosted, and each thread's CPU time. (From the Crossy Road port.) MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_config.h"
#include "util.h"

#define BOOST_AFTER_MS 50
#define LOG_OVER_MS 120

static Mutex s_mx;
static volatile u64 s_frame_start; /* system tick; 0 between frames */
static volatile u64 s_boost_start;
static int s_on, s_failed, s_logged;
static int s_launch; /* boosted from start-up to the first picture */
static int s_hold;   /* boosted while something asks (dcr_boost_hold) */
static u64 s_launch_start;
static u64 s_boosts, s_boost_ms_total;

static u64 ms_since(u64 tick) { return armTicksToNs(armGetSystemTick() - tick) / 1000000ull; }

/* ---- where a long frame's time went, without pausing anyone: each thread's
 * CPU time (the kernel's per-thread tick count) and the file reads, from the
 * moment the frame passed BOOST_AFTER_MS to its end. The main thread mostly
 * waits in such frames (for Unity's loading thread, the render thread, the
 * GC), so this says which thread the time was really spent on. */
void dcr_io_read_stats(uint64_t *calls, uint64_t *bytes, uint64_t *ticks); /* bionic_io.c */
#define MAXT 64
typedef struct {
  BThread *t;
  int tid, main;
  char name[16];
  u64 ticks;
} TSnap;
static TSnap s_ts[MAXT];
static int s_nts;
static u64 s_io0[3];
static int s_have_snap;

static u64 thread_ticks(Handle h) {
  u64 v = 0;
  if (R_FAILED(svcGetInfo(&v, InfoType_ThreadTickCount, h, TickCountInfo_Total)))
    svcGetInfo(&v, InfoType_ThreadTickCountDeprecated, h, TickCountInfo_Total);
  return v;
}

static void snap_thread(BThread *t, void *arg) {
  if (s_nts >= MAXT || t->handle == INVALID_HANDLE || t->finished)
    return;
  TSnap *e = &s_ts[s_nts++];
  e->t = t;
  e->tid = t->tid;
  e->main = t->handle == envGetMainThreadHandle();
  memcpy(e->name, t->name, sizeof e->name);
  e->name[sizeof e->name - 1] = 0;
  e->ticks = thread_ticks(t->handle);
}

/* the CPU used since the snapshot, by live threads still in it */
typedef struct {
  int idx;
  u64 used;
} Used;
static Used s_used[MAXT];
static int s_nused;

static void diff_thread(BThread *t, void *arg) {
  if (t->handle == INVALID_HANDLE || t->finished)
    return;
  for (int i = 0; i < s_nts; i++)
    if (s_ts[i].t == t && s_ts[i].tid == t->tid) {
      u64 now = thread_ticks(t->handle);
      if (now > s_ts[i].ticks && s_nused < MAXT) {
        s_used[s_nused].idx = i;
        s_used[s_nused].used = now - s_ts[i].ticks;
        s_nused++;
      }
      return;
    }
}

static void cpu_report(char *out, size_t cap) {
  s_nused = 0;
  b_thread_foreach(diff_thread, NULL);
  int n = snprintf(out, cap, "; CPU since:");
  for (int k = 0; k < 4 && n < (int)cap - 48; k++) {
    int best = -1;
    for (int i = 0; i < s_nused; i++)
      if (s_used[i].used && (best < 0 || s_used[i].used > s_used[best].used))
        best = i;
    if (best < 0 || armTicksToNs(s_used[best].used) < 5000000ull)
      break;
    const TSnap *e = &s_ts[s_used[best].idx];
    n += snprintf(out + n, cap - n, " %s %llu ms,", e->main ? "main" : (e->name[0] ? e->name : "?"),
                  (unsigned long long)(armTicksToNs(s_used[best].used) / 1000000ull));
    s_used[best].used = 0;
  }
  u64 io[3];
  dcr_io_read_stats(&io[0], &io[1], &io[2]);
  snprintf(out + n, cap - n, " file reads %llu (%llu KB, %llu ms)", (unsigned long long)(io[0] - s_io0[0]),
           (unsigned long long)((io[1] - s_io0[1]) >> 10),
           (unsigned long long)(armTicksToNs(io[2] - s_io0[2]) / 1000000ull));
}

static int set_boost(int on) {
  Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  if (R_FAILED(rc) && on && !s_failed) {
    s_failed = 1;
    debugPrintf("[boost] appletSetCpuBoostMode unavailable (0x%x): long frames run at normal clocks\n",
                (unsigned)rc);
  }
  return R_SUCCEEDED(rc);
}

/* Frame loop: around nativeRender. */
void dcr_boost_frame_begin(void) { s_frame_start = armGetSystemTick(); }

void dcr_boost_frame_end(u64 frame) {
  u64 start = s_frame_start;
  mutexLock(&s_mx);
  s_frame_start = 0;
  u64 boosted = 0;
  if (s_on && !s_hold) {
    boosted = ms_since(s_boost_start);
    set_boost(0);
    s_on = 0;
    s_boost_ms_total += boosted;
  }
  int launch = s_launch;
  int have = s_have_snap;
  s_have_snap = 0;
  u64 took = start ? ms_since(start) : 0;
  static char cpu[400];
  cpu[0] = 0;
  if (have && took >= LOG_OVER_MS && s_logged < 400)
    cpu_report(cpu, sizeof cpu);
  mutexUnlock(&s_mx);
  if (took >= LOG_OVER_MS && s_logged < 400) {
    s_logged++;
    if (launch)
      debugPrintf("[frame] frame %llu took %llu ms (start-up: all of it CPU-boosted)%s\n",
                  (unsigned long long)frame, (unsigned long long)took, cpu);
    else if (boosted)
      debugPrintf("[frame] frame %llu took %llu ms, the last %llu ms of it CPU-boosted%s\n",
                  (unsigned long long)frame, (unsigned long long)took, (unsigned long long)boosted, cpu);
    else
      debugPrintf("[frame] frame %llu took %llu ms%s\n", (unsigned long long)frame,
                  (unsigned long long)took, cpu);
  }
}

/* Every vsync pulse: boost a frame that has run past BOOST_AFTER_MS. */
void dcr_boost_poll(void) {
  u64 start = s_frame_start;
  if (!start || s_have_snap || ms_since(start) < BOOST_AFTER_MS)
    return;
  mutexLock(&s_mx);
  if (s_frame_start == start && !s_have_snap) {
    s_nts = 0;
    b_thread_foreach(snap_thread, NULL);
    dcr_io_read_stats(&s_io0[0], &s_io0[1], &s_io0[2]);
    s_have_snap = 1;
    if (!s_launch && !s_on && !s_failed && dcr_config()->boost && set_boost(1)) {
      s_on = 1;
      s_boost_start = armGetSystemTick();
      s_boosts++;
    }
  }
  mutexUnlock(&s_mx);
}

/* main(), once config.ini is read. */
void dcr_boost_launch_begin(void) {
  if (!dcr_config()->boost)
    return;
  mutexLock(&s_mx);
  if (set_boost(1)) {
    s_launch = 1;
    s_launch_start = armGetSystemTick();
  }
  mutexUnlock(&s_mx);
}

/* The frame loop, after the first frame that reached the screen. */
void dcr_boost_launch_end(void) {
  mutexLock(&s_mx);
  int was = s_launch;
  if (was) {
    s_launch = 0;
    if (s_hold) { /* still wanted: it stays on */
      s_on = 1;
      s_boost_start = armGetSystemTick();
    } else {
      set_boost(0);
    }
  }
  mutexUnlock(&s_mx);
  if (was)
    debugPrintf("[boost] start-up: %llu ms at 1785 MHz, until the first picture\n",
                (unsigned long long)ms_since(s_launch_start));
}

/* The intro video (pvz_video.c): its decoding wants the fast CPU while it
 * plays, when the GPU only draws one picture a frame. Off: the next frame's
 * end turns it off. */
void dcr_boost_hold(int on) {
  if (!dcr_config()->boost)
    return;
  mutexLock(&s_mx);
  if (on && !s_hold) {
    s_hold = 1;
    if (!s_on && !s_launch && !s_failed && set_boost(1)) {
      s_on = 1;
      s_boost_start = armGetSystemTick();
      s_boosts++;
    }
  } else if (!on) {
    s_hold = 0;
  }
  mutexUnlock(&s_mx);
}

void dcr_boost_report(void) {
  if (s_boosts)
    debugPrintf("[boost] %llu long frames boosted so far, %llu ms at 1785 MHz in total\n",
                (unsigned long long)s_boosts, (unsigned long long)s_boost_ms_total);
}
