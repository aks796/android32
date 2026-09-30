/* bionic_time.c -- clocks, sleeps and calendar time for the bionic ABI.
 *
 * THE MONOTONIC CLOCK DOES NOT COUNT SUSPENSION
 * ---------------------------------------------
 * An engine's frame clock is CLOCK_MONOTONIC more often than not (Unity's
 * GetTimeSinceStartup, for one, and Mono's Stopwatch / Environment.TickCount).
 * A Switch keeps the system tick running while the console sleeps or the HOME
 * menu is up, so on resume the engine would integrate the entire gap into one
 * frame: physics explodes, tweens snap, timers fire in a burst.
 *
 * Fixing it HERE, at the clock source, fixes it for every consumer at once and
 * keeps the engine's own semantics intact (fixed-step physics, time scales,
 * a maximum frame delta). dcr_time_suspend()/_resume() are called on focus
 * lost / gained; MONOTONIC stands still between them.
 *
 * NOR DOES A FROZEN PROCESS
 * -------------------------
 * For the HOME menu and sleep the system freezes the whole process, and no
 * message may say so beforehand (hardware, 2026-09-30: after 79 minutes of
 * sleep a game's first frame integrated all of it). A thread of this file
 * reads the clock every RT_TIME_WATCH_NS, so while the process runs no two
 * readings are far apart: a gap of more than RT_TIME_FREEZE_NS is time the
 * process did not run, and it is removed on the spot, before the reading is
 * returned, so no clock jumps and steps back.
 *
 * So there are two clocks under one lock:
 *   run      = tick - frozen                  (dcr_run_ns: the watchdog's)
 *   monotonic = run - suspended, standing still while suspended
 * Every path that reads the tick -- a reading, the watch thread, and the
 * suspend / resume stamps -- goes through run_ns_locked(), which finds a
 * freeze and records the latest reading: a focus message handled right after
 * a thaw neither stamps its suspension after an unremoved freeze nor leaves
 * the last reading from before it (to be removed a second time). A
 * suspension is measured in run time, so a freeze inside it is not counted
 * twice. Only the watch thread writes the log, outside the lock.
 *
 * The wall clocks: REALTIME (with gettimeofday and ftime) and time() are the
 * real wall clock, or, per RT_TIME_SHIFT, the wall clock at boot plus the run
 * time -- for an engine whose frame timer is gettimeofday, which then trails
 * the real clock by the time slept.
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
#include "rt_settings.h"
#include "util.h"

/* ---------------------------------------------------------------- settings */
/* Which wall clocks leave the freezes out (dcr_time.h's bits):
 *   0 (dcr, abs, pvz, sonic, flappy): both are the real wall clock;
 *   RT_TIME_SHIFT_REALTIME (lab2): REALTIME / gettimeofday / ftime do (the
 *     engine's frame timers are gettimeofday), time() stays real (a date the
 *     game keeps);
 *   RT_TIME_SHIFT_REALTIME | RT_TIME_SHIFT_TIME (a8r): time() too (the
 *     engine's frame timer is gettimeofday, and its date checks follow it). */
#ifndef RT_TIME_SHIFT
#define RT_TIME_SHIFT 0
#endif
#if (RT_TIME_SHIFT & RT_TIME_SHIFT_TIME) && !(RT_TIME_SHIFT & RT_TIME_SHIFT_REALTIME)
#error "RT_TIME_SHIFT: time() can run on the process's time only if REALTIME does too"
#endif
/* The freeze watch thread; 0 leaves freezes in every clock (no detection
 * without the thread: a long load would look like a freeze). */
#ifndef RT_TIME_FREEZE_WATCH
#define RT_TIME_FREEZE_WATCH 1
#endif
/* Its libnx priority: above the game's threads (59) and the ports' own
 * helpers, so a long load never starves it into a false freeze. 0x2C for
 * every port but lab2 (0x2B: its main thread and sound decoder are 0x2C). */
#ifndef RT_TIME_WATCH_PRIO
#define RT_TIME_WATCH_PRIO 0x2C
#endif
#ifndef RT_TIME_WATCH_CORE
#define RT_TIME_WATCH_CORE -2 /* libnx: the default core */
#endif
#ifndef RT_TIME_WATCH_STACK
#define RT_TIME_WATCH_STACK 0x4000
#endif
/* Its reading period, and the gap between two readings that only a frozen
 * process leaves (every port: 100 ms and 2 s). */
#ifndef RT_TIME_WATCH_NS
#define RT_TIME_WATCH_NS 100000000ull
#endif
#ifndef RT_TIME_FREEZE_NS
#define RT_TIME_FREEZE_NS 2000000000ull
#endif

/* ------------------------------------------------------------------ bases */
static Mutex g_susp_lock;       /* everything below, to the realtime bases */
static int g_watch_on;          /* the watch thread runs: freezes are looked for */
static u64 g_last_read;         /* tick-ns of the latest reading */
static u64 g_frozen_ns;         /* total time frozen (run = tick - this) */
static u64 g_suspended_ns;      /* total time spent suspended (run-ns) */
static u64 g_suspend_start;     /* run-ns when the current suspension began, or 0 */
static unsigned g_freezes;      /* freezes seen */
static u64 g_log_frozen_ms;     /* for the watch thread's log: freezes seen since, */
static u64 g_log_resumed_ms;    /* and the suspension just ended, */
static int g_log_resumed;       /* if any */

static s64 g_realtime_base_s;   /* UTC seconds at g_realtime_tick_ns */
static u64 g_realtime_tick_ns;  /* tick-ns at dcr_time_init (= run-ns then) */
static int32_t g_gmtoff;        /* local time offset, seconds */
static char g_tzname[16] = "UTC";

static inline u64 tick_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* A reading, under g_susp_lock: the tick less the time frozen. A gap over
 * RT_TIME_FREEZE_NS since the latest reading, by any thread, was a freeze:
 * it is removed (less the one watch period a running process may leave). */
static u64 run_ns_locked(void) {
  u64 t = tick_ns();
  if (g_watch_on && g_last_read && t > g_last_read + RT_TIME_FREEZE_NS) {
    u64 gap = t - g_last_read - RT_TIME_WATCH_NS;
    g_frozen_ns += gap;
    g_log_frozen_ms += gap / 1000000ull;
    __atomic_store_n(&g_freezes, g_freezes + 1, __ATOMIC_RELEASE);
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

unsigned dcr_time_freezes(void) { return __atomic_load_n(&g_freezes, __ATOMIC_ACQUIRE); }

void dcr_time_suspend(void) {
  mutexLock(&g_susp_lock);
  u64 now = run_ns_locked(); /* first: see the top of this file */
  if (!g_suspend_start)
    g_suspend_start = now;
  mutexUnlock(&g_susp_lock);
}

void dcr_time_resume(void) {
  mutexLock(&g_susp_lock);
  u64 now = run_ns_locked();
  if (g_suspend_start) {
    u64 gap = now - g_suspend_start;
    g_suspended_ns += gap;
    g_suspend_start = 0;
    g_log_resumed_ms += gap / 1000000ull;
    g_log_resumed = 1;
  }
  mutexUnlock(&g_susp_lock);
}

/* Monotonic nanoseconds with suspension and freezes removed. While suspended,
 * time stands still at the moment the suspension began (the reading is still
 * taken, for the freeze check). */
u64 dcr_monotonic_ns(void) {
  mutexLock(&g_susp_lock);
  u64 now = run_ns_locked();
  u64 r = (g_suspend_start ? g_suspend_start : now) - g_suspended_ns;
  mutexUnlock(&g_susp_lock);
  return r;
}

#if RT_TIME_FREEZE_WATCH
/* The watch thread: a reading every RT_TIME_WATCH_NS, so that only a frozen
 * process leaves a gap of RT_TIME_FREEZE_NS. A libnx thread outside the
 * game's thread list, so neither the game nor a garbage collector stops it.
 * It writes the log lines for the freezes and resumes the clocks saw. */
static void freeze_watch(void *arg) {
  (void)arg;
  for (;;) {
    svcSleepThread((s64)RT_TIME_WATCH_NS);
    mutexLock(&g_susp_lock);
    run_ns_locked();
    u64 frozen_ms = g_log_frozen_ms, resumed_ms = g_log_resumed_ms;
    int resumed = g_log_resumed;
    g_log_frozen_ms = g_log_resumed_ms = 0;
    g_log_resumed = 0;
    mutexUnlock(&g_susp_lock);
    if (frozen_ms)
      debugPrintf("[time] the process was frozen for %llu ms (sleep or the HOME menu): the game's clocks "
                  "did not count it\n", (unsigned long long)frozen_ms);
    if (resumed)
      debugPrintf("[time] resumed after %llu ms; monotonic clocks did not count it\n",
                  (unsigned long long)resumed_ms);
  }
}
#endif

static void start_watch(void) {
#if RT_TIME_FREEZE_WATCH
  static Thread watch;
  Result rc = threadCreate(&watch, freeze_watch, NULL, NULL, RT_TIME_WATCH_STACK, RT_TIME_WATCH_PRIO,
                           RT_TIME_WATCH_CORE);
  if (R_FAILED(rc)) {
    debugPrintf("[time] no freeze watch thread (0x%x): a freeze shows as one long frame\n", rc);
    return;
  }
  /* Readings before now were not watched: the gap since the last of them
   * (the whole boot, maybe) is not a freeze. */
  mutexLock(&g_susp_lock);
  g_last_read = tick_ns();
  g_watch_on = 1;
  mutexUnlock(&g_susp_lock);
  rc = threadStart(&watch);
  if (R_FAILED(rc)) {
    mutexLock(&g_susp_lock);
    g_watch_on = 0;
    mutexUnlock(&g_susp_lock);
    threadClose(&watch);
    debugPrintf("[time] no freeze watch thread (start 0x%x): a freeze shows as one long frame\n", rc);
  }
#endif
}

/* Called once at boot (after the time service may or may not have come up). */
void dcr_time_init(void) {
  u64 now_utc = 0;
  g_realtime_tick_ns = tick_ns(); /* nothing frozen yet: run time = tick */
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
  start_watch();
}

#if !(RT_TIME_SHIFT & RT_TIME_SHIFT_TIME)
/* The real wall clock, sleep and freezes included. */
static void wall_now(s64 *sec, int32_t *nsec) {
  u64 d = tick_ns() - g_realtime_tick_ns;
  *sec = g_realtime_base_s + (s64)(d / 1000000000ull);
  *nsec = (int32_t)(d % 1000000000ull);
}
#endif

#if RT_TIME_SHIFT & RT_TIME_SHIFT_REALTIME
/* The wall clock at boot plus the process's run time: freezes left out. */
static void run_realtime(s64 *sec, int32_t *nsec) {
  u64 d = dcr_run_ns() - g_realtime_tick_ns;
  *sec = g_realtime_base_s + (s64)(d / 1000000000ull);
  *nsec = (int32_t)(d % 1000000000ull);
}
#endif

/* REALTIME, gettimeofday, ftime: one clock (RT_TIME_SHIFT). */
static void realtime_now(s64 *sec, int32_t *nsec) {
#if RT_TIME_SHIFT & RT_TIME_SHIFT_REALTIME
  run_realtime(sec, nsec);
#else
  wall_now(sec, nsec);
#endif
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
  s64 s;
  int32_t ns;
#if RT_TIME_SHIFT & RT_TIME_SHIFT_TIME
  run_realtime(&s, &ns);
#else
  wall_now(&s, &ns);
#endif
  if (t)
    *t = (b_time_t)s;
  return (b_time_t)s;
}

b_clock_t b_clock(void) {
  /* CLOCKS_PER_SEC is 1000000 on bionic */
  return (b_clock_t)(dcr_monotonic_ns() / 1000ull);
}

double b_difftime(b_time_t a, b_time_t b) { return (double)a - (double)b; }

/* times(): in clock ticks of sysconf(_SC_CLK_TCK) = 100, as bionic's. */
struct b_tms {
  b_clock_t tms_utime, tms_stime, tms_cutime, tms_cstime;
};
b_clock_t b_times(struct b_tms *t) {
  b_clock_t ticks = (b_clock_t)(dcr_monotonic_ns() / 10000000ull);
  if (t) {
    t->tms_utime = ticks;
    t->tms_stime = 0;
    t->tms_cutime = t->tms_cstime = 0;
  }
  return ticks;
}

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

/* A sampling profiler's timer (Mono's); there are no signals to deliver it with. */
int b_setitimer(int which, const struct b_itimerval *nv, struct b_itimerval *ov) {
  if (ov)
    memset(ov, 0, sizeof *ov);
  return 0;
}
