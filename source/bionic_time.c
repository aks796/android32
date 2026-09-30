/* bionic_time.c -- clocks, sleeps and calendar time for the bionic ABI.
 *
 * THE MONOTONIC CLOCK DOES NOT COUNT SUSPENSION
 * ---------------------------------------------
 * Unity 2017.4's engine clock is GetTimeSinceStartup() -> clock_gettime(
 * CLOCK_MONOTONIC) (libunity+0x2f5e68, verified through its PLT), and Mono's
 * Stopwatch / Environment.TickCount read the same clock. A Switch keeps the
 * system tick running while the console sleeps or the HOME menu is up, so on
 * resume the engine would integrate the entire gap into one frame: physics
 * explodes, tweens snap, timers fire in a burst.
 *
 * Fixing it HERE, at the clock source, fixes it for every consumer at once and
 * keeps the engine's own semantics intact (fixed-step physics, timeScale,
 * maximumDeltaTime, Time.time inside FixedUpdate). dcr_time_suspend()/_resume()
 * are called from the applet focus hook; the gap between them is subtracted
 * from every MONOTONIC/BOOTTIME reading.
 *
 * NOR DOES A FROZEN PROCESS
 * -------------------------
 * For the HOME menu and sleep the system freezes the whole process, and no
 * message may say so beforehand (the Asphalt 8 Retry port on hardware,
 * 2026-09-30: after 79 minutes of sleep the first frame integrated all of
 * it). A thread of this file reads the clock every WATCH_NS, so while the
 * process runs no two readings are far apart: a gap of more than FREEZE_NS
 * is time the process did not run, and it is removed on the spot, before the
 * reading is returned, so no clock jumps and steps back. Labyrinth 2's frame
 * clock is gettimeofday (the engine's millisecond timers, liblabyrinthii
 * +0x1b5ac / +0x1b5f6, read by SurfaceView.render), so REALTIME leaves the
 * freezes out as well, and trails the real wall clock by the time slept.
 * time() stays on the wall clock: the engine calls it once, for a date it
 * keeps in the registry.
 *
 * All structs converted: bionic timespec/timeval are 2x int32, newlib's are
 * 16 bytes with a 64-bit time_t; Linux clock ids are renumbered (bionic.h). MIT.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <time.h>

#include "bionic.h"
#include "dcr_time.h"
#include "util.h"

/* ------------------------------------------------------------------ bases */
static u64 g_suspended_ns;   /* total time spent suspended (run-ns) */
static u64 g_suspend_start;  /* run-ns when the current suspension began, or 0 */
static Mutex g_susp_lock;

/* The process's own time: the system tick less the gaps it was frozen. */
#define FREEZE_NS 2000000000ull /* a gap between readings longer than this: frozen */
#define WATCH_NS 100000000ull   /* the watch thread's reading period */
static u64 g_frozen_ns;         /* total time frozen */
static u64 g_last_read;         /* tick-ns of the latest reading */
static u64 g_freeze_gap;        /* a freeze just seen, for the log (ms), or 0 */
static volatile unsigned g_freezes; /* freezes seen */

static s64 g_realtime_base_s;   /* UTC seconds at g_realtime_tick */
static u64 g_realtime_tick_ns;
static int32_t g_gmtoff;        /* local time offset, seconds */
static char g_tzname[16] = "UTC";

static inline u64 tick_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* The tick less frozen time; under g_susp_lock. A suspension's run-ns do not
 * include a freeze inside it, so no gap is counted twice. */
static u64 run_ns_locked(void) {
  u64 t = tick_ns();
  if (g_last_read && t > g_last_read + FREEZE_NS) {
    u64 gap = t - g_last_read - WATCH_NS;
    g_frozen_ns += gap;
    g_freeze_gap = gap / 1000000ull;
    g_freezes++;
  }
  if (t > g_last_read)
    g_last_read = t;
  return t - g_frozen_ns;
}

u64 dcr_run_ns(void) {
  mutexLock(&g_susp_lock);
  u64 r = run_ns_locked();
  mutexUnlock(&g_susp_lock);
  return r;
}

unsigned dcr_time_freezes(void) { return g_freezes; }

/* The watch thread: a reading every WATCH_NS, so that only a frozen process
 * leaves a gap of FREEZE_NS. A libnx thread at priority 0x2B, above the main
 * thread's and the sound decoder's (0x2C, core 0) and the engine's threads,
 * so a long load does not starve it into a false freeze. It also logs the
 * freeze. */
static void freeze_watch(void *arg) {
  (void)arg;
  for (;;) {
    svcSleepThread((s64)WATCH_NS);
    mutexLock(&g_susp_lock);
    run_ns_locked();
    u64 ms = g_freeze_gap;
    g_freeze_gap = 0;
    mutexUnlock(&g_susp_lock);
    if (ms)
      debugPrintf("[time] the process was frozen for %llu ms (sleep or the HOME menu): the game's clocks "
                  "did not count it\n", (unsigned long long)ms);
  }
}

/* Called once at boot (after the time service may or may not have come up). */
void dcr_time_init(void) {
  u64 now_utc = 0;
  g_realtime_tick_ns = tick_ns(); /* nothing frozen yet: run time = tick */
  static Thread watch;
  if (R_SUCCEEDED(threadCreate(&watch, freeze_watch, NULL, NULL, 0x4000, 0x2B, -2)))
    threadStart(&watch);
  else
    debugPrintf("[time] no freeze watch thread\n");
  if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &now_utc))) {
    g_realtime_base_s = (s64)now_utc;
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo ai;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(now_utc, &ct, &ai))) {
      g_gmtoff = ai.offset;
      memcpy(g_tzname, ai.timezoneName, sizeof ai.timezoneName < sizeof g_tzname
                                            ? sizeof ai.timezoneName : sizeof g_tzname - 1);
      g_tzname[sizeof g_tzname - 1] = 0;
    }
  } else {
    /* No time service (seen under emulation): start from a fixed, sane date
     * (2020-01-01) so date arithmetic in the game never sees 1970. */
    g_realtime_base_s = 1577836800;
  }
  debugPrintf("[time] realtime base %lld, local offset %d s (%s)\n",
              (long long)g_realtime_base_s, (int)g_gmtoff, g_tzname);
}

void dcr_time_suspend(void) {
  mutexLock(&g_susp_lock);
  if (!g_suspend_start)
    g_suspend_start = run_ns_locked();
  mutexUnlock(&g_susp_lock);
}

void dcr_time_resume(void) {
  mutexLock(&g_susp_lock);
  if (g_suspend_start) {
    u64 gap = run_ns_locked() - g_suspend_start;
    g_suspended_ns += gap;
    g_suspend_start = 0;
    debugPrintf("[time] resumed after %llu ms; monotonic clocks did not count it\n",
                (unsigned long long)(gap / 1000000ull));
  }
  mutexUnlock(&g_susp_lock);
}

/* Monotonic nanoseconds with suspension and freezes removed. While
 * suspended, time stands still at the moment the suspension began. */
u64 dcr_monotonic_ns(void) {
  mutexLock(&g_susp_lock);
  u64 t = g_suspend_start ? g_suspend_start : run_ns_locked();
  u64 r = t - g_suspended_ns;
  mutexUnlock(&g_susp_lock);
  return r;
}

/* REALTIME (clock_gettime, gettimeofday): the wall clock at boot plus the
 * process's run time, freezes left out. */
static void realtime_now(s64 *sec, int32_t *nsec) {
  u64 d = dcr_run_ns() - g_realtime_tick_ns;
  *sec = g_realtime_base_s + (s64)(d / 1000000000ull);
  *nsec = (int32_t)(d % 1000000000ull);
}

/* time(): the real wall clock, sleep included. */
static s64 wall_now(void) {
  return g_realtime_base_s + (s64)((tick_ns() - g_realtime_tick_ns) / 1000000000ull);
}

/* --------------------------------------------------------------- clocks */
int b_clock_gettime(int clk, struct b_timespec *ts) {
  if (!ts) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  switch (clk) {
  case L_CLOCK_REALTIME:
  case L_CLOCK_REALTIME_COARSE: {
    s64 s;
    int32_t ns;
    realtime_now(&s, &ns);
    ts->tv_sec = (int32_t)s;
    ts->tv_nsec = ns;
    return 0;
  }
  case L_CLOCK_MONOTONIC:
  case L_CLOCK_MONOTONIC_RAW:
  case L_CLOCK_MONOTONIC_COARSE:
  case L_CLOCK_BOOTTIME:
  case L_CLOCK_PROCESS_CPUTIME_ID:
  case L_CLOCK_THREAD_CPUTIME_ID: {
    u64 n = dcr_monotonic_ns();
    ts->tv_sec = (int32_t)(n / 1000000000ull);
    ts->tv_nsec = (int32_t)(n % 1000000000ull);
    return 0;
  }
  default:
    if (clk < 0) { /* bionic's per-thread/per-process CPU clock encodings */
      u64 n = dcr_monotonic_ns();
      ts->tv_sec = (int32_t)(n / 1000000000ull);
      ts->tv_nsec = (int32_t)(n % 1000000000ull);
      return 0;
    }
    b_set_errno(L_EINVAL);
    return -1;
  }
}

int b_clock_getres(int clk, struct b_timespec *res) {
  if (res) {
    res->tv_sec = 0;
    res->tv_nsec = 52; /* 19.2 MHz system tick */
  }
  return 0;
}

int b_gettimeofday(struct b_timeval *tv, void *tz) {
  if (tv) {
    s64 s;
    int32_t ns;
    realtime_now(&s, &ns);
    tv->tv_sec = (int32_t)s;
    tv->tv_usec = ns / 1000;
  }
  return 0;
}

b_time_t b_time(b_time_t *t) {
  s64 s = wall_now();
  if (t)
    *t = (b_time_t)s;
  return (b_time_t)s;
}

b_clock_t b_clock(void) {
  /* CLOCKS_PER_SEC is 1000000 on bionic */
  return (b_clock_t)(dcr_monotonic_ns() / 1000ull);
}

double b_difftime(b_time_t a, b_time_t b) { return (double)a - (double)b; }

/* --------------------------------------------------------------- sleeps */
int b_nanosleep(const struct b_timespec *req, struct b_timespec *rem) {
  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  s64 ns = (s64)req->tv_sec * 1000000000ll + req->tv_nsec;
  svcSleepThread(ns > 0 ? ns : 0);
  if (rem)
    rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

int b_usleep(unsigned int us) {
  svcSleepThread((s64)us * 1000);
  return 0;
}

unsigned int b_sleep(unsigned int s) {
  svcSleepThread((s64)s * 1000000000ll);
  return 0;
}

/* ------------------------------------------------------------- calendar */
static void to_btm(struct b_tm *out, const struct tm *in, long gmtoff, const char *zone) {
  out->tm_sec = in->tm_sec;
  out->tm_min = in->tm_min;
  out->tm_hour = in->tm_hour;
  out->tm_mday = in->tm_mday;
  out->tm_mon = in->tm_mon;
  out->tm_year = in->tm_year;
  out->tm_wday = in->tm_wday;
  out->tm_yday = in->tm_yday;
  out->tm_isdst = 0;
  out->tm_gmtoff = gmtoff;
  out->tm_zone = zone;
}

static void from_btm(struct tm *out, const struct b_tm *in) {
  memset(out, 0, sizeof *out);
  out->tm_sec = in->tm_sec;
  out->tm_min = in->tm_min;
  out->tm_hour = in->tm_hour;
  out->tm_mday = in->tm_mday;
  out->tm_mon = in->tm_mon;
  out->tm_year = in->tm_year;
  out->tm_wday = in->tm_wday;
  out->tm_yday = in->tm_yday;
  out->tm_isdst = in->tm_isdst;
}

static __thread struct b_tm t_tm_gm, t_tm_local;

struct b_tm *b_gmtime(const b_time_t *t) {
  if (!t)
    return NULL;
  time_t tt = (time_t)*t;
  struct tm r;
  gmtime_r(&tt, &r);
  to_btm(&t_tm_gm, &r, 0, "UTC");
  return &t_tm_gm;
}

struct b_tm *b_localtime(const b_time_t *t) {
  if (!t)
    return NULL;
  time_t tt = (time_t)*t + g_gmtoff;
  struct tm r;
  gmtime_r(&tt, &r);
  to_btm(&t_tm_local, &r, g_gmtoff, g_tzname);
  return &t_tm_local;
}

struct b_tm *b_gmtime_r(const b_time_t *t, struct b_tm *out) {
  if (!t || !out)
    return NULL;
  time_t tt = (time_t)*t;
  struct tm r;
  gmtime_r(&tt, &r);
  to_btm(out, &r, 0, "UTC");
  return out;
}

struct b_tm *b_localtime_r(const b_time_t *t, struct b_tm *out) {
  if (!t || !out)
    return NULL;
  time_t tt = (time_t)*t + g_gmtoff;
  struct tm r;
  gmtime_r(&tt, &r);
  to_btm(out, &r, g_gmtoff, g_tzname);
  return out;
}

char *b_asctime(const struct b_tm *btm) {
  static __thread char buf[64];
  static const char day[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char mon[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!btm)
    return NULL;
  snprintf(buf, sizeof buf, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
           day[(unsigned)btm->tm_wday % 7], mon[(unsigned)btm->tm_mon % 12], btm->tm_mday,
           btm->tm_hour, btm->tm_min, btm->tm_sec, 1900 + btm->tm_year);
  return buf;
}

/* struct timeb (32-bit bionic): time_t time; unsigned short millitm;
 * short timezone, dstflag */
struct b_timeb {
  b_time_t time;
  unsigned short millitm;
  short timezone, dstflag;
};
int b_ftime(struct b_timeb *tb) {
  struct b_timeval tv;
  b_gettimeofday(&tv, NULL);
  if (tb) {
    tb->time = tv.tv_sec;
    tb->millitm = (unsigned short)(tv.tv_usec / 1000);
    tb->timezone = (short)(-g_gmtoff / 60);
    tb->dstflag = 0;
  }
  return 0;
}

size_t b_strftime(char *s, size_t max, const char *fmt, const struct b_tm *btm);
size_t b_strftime_l(char *s, size_t max, const char *fmt, const struct b_tm *btm, void *loc) {
  return b_strftime(s, max, fmt, btm);
}

b_time_t b_mktime(struct b_tm *btm) {
  struct tm t;
  from_btm(&t, btm);
  time_t r = mktime(&t); /* newlib: no TZ set, so this is UTC */
  if (r == (time_t)-1)
    return -1;
  r -= g_gmtoff;       /* the input was local time */
  struct tm n;
  time_t local = r + g_gmtoff;
  gmtime_r(&local, &n);
  to_btm(btm, &n, g_gmtoff, g_tzname);
  return (b_time_t)r;
}

size_t b_strftime(char *s, size_t max, const char *fmt, const struct b_tm *btm) {
  struct tm t;
  from_btm(&t, btm);
  return strftime(s, max, fmt, &t);
}

size_t b_wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct b_tm *btm) {
  /* Narrow through strftime; the engine only formats ASCII here. */
  char nf[256], out[512];
  size_t i = 0;
  for (; fmt && fmt[i] && i < sizeof nf - 1; i++)
    nf[i] = (char)fmt[i];
  nf[i] = 0;
  size_t n = b_strftime(out, sizeof out, nf, btm);
  if (!s || !max)
    return 0;
  if (n >= max)
    n = max - 1;
  for (i = 0; i < n; i++)
    s[i] = (unsigned char)out[i];
  s[n] = 0;
  return n;
}

int b_utime(const char *path, const struct b_utimbuf *t) { return 0; }

/* Mono's sampling profiler timer; there are no signals to deliver it with. */
int b_setitimer(int which, const struct b_itimerval *nv, struct b_itimerval *ov) {
  if (ov)
    memset(ov, 0, sizeof *ov);
  return 0;
}
