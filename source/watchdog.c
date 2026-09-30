/* watchdog.c -- keeps the log reaching the SD card, and reports hangs.
 *
 * During play the log goes to a RAM ring (util.c), written out periodically.
 * If the engine stops presenting frames, the evidence would stay in RAM (the
 * Crossy Road port's first hang: debug.log ended at frame 3). So this thread,
 * created with libnx directly (no guest code, and no GC bridge, ever sees or
 * stops it):
 *   - flushes the ring every 5 s whatever the engine is doing;
 *   - when no frame has completed for 10 s while frames are expected
 *     (dcr_boot_in_focus: rt_applet.c), snapshots every registered thread --
 *     pause, read registers and the top of its stack, resume -- and only then
 *     logs, with addresses named module+offset. Nothing is logged while a
 *     thread is paused: it may hold the log lock.
 * Reports repeat at 40 s and 100 s of the same hang, then stop.
 *
 * Time is the process's own, the tick less the freezes (dcr_run_ns,
 * bionic_time.c): a freeze for sleep or the HOME menu is not a hang, though
 * no frame came during it (the raw tick gave three false reports after each
 * wake). If the log lock or the thread list's lock stays taken for a second,
 * the report is made without locks, to svcOutputDebugString only (emergency
 * below). Under an emulator no thread is paused: Ryujinx blocks in the pause
 * for a spinning thread, or crashes. MIT.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_sched.h"
#include "dcr_time.h"
#include "exc_handler.h"
#include "gl_layer.h"
#include "rt_applet.h"
#include "rt_settings.h"
#include "util.h"
#include "watchdog.h"

/* RT_WATCHDOG_PRIO / RT_WATCHDOG_CORE: the watchdog thread's priority and
 * core. It sleeps nearly all the time; a thread spinning at a high priority
 * on the watchdog's core must not silence the one that would report it.
 * Default 0x2B on the process's default core (-2): lab2, abs, pvz, sonic,
 * flappy, a8r. dcr: 0x1C (the highest its NPDM allows) on core 2, away from
 * core 0 where its helper threads start. */
#ifndef RT_WATCHDOG_PRIO
#define RT_WATCHDOG_PRIO 0x2B
#endif
#ifndef RT_WATCHDOG_CORE
#define RT_WATCHDOG_CORE -2
#endif

/* The kernel's ThreadContext (svcGetThreadContext3); for an AArch32 thread
 * r[0..14] hold r0-r14. */
typedef struct {
  uint64_t r[29];
  uint64_t fp, lr, sp, pc;
  uint32_t psr, _pad;
  uint8_t v[32][16];
  uint32_t fpcr, fpsr;
  uint64_t tpidr;
} KCtx;
_Static_assert(sizeof(KCtx) == 0x320, "kernel ThreadContext is 0x320 bytes");

static Result get_ctx(KCtx *ctx, Handle h) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)ctx;
  register uint32_t r1 __asm__("r1") = h;
  /* The 32-bit SVC ABI returns with r1-r3 zeroed: they must be clobbers, or
   * the compiler keeps live values there (the watchdog's first report died on
   * a pointer it had parked in r3). */
  __asm__ volatile("svc 0x33" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
  return r0;
}

__attribute__((weak)) void port_watchdog_emulator_report(unsigned secs) { (void)secs; }

/* ---------------------------------------------------------------- counters */
#define MAX_COUNTERS 8
static struct {
  const char *label;
  uint32_t (*read)(void);
  uint32_t prev; /* at the previous report */
} g_ctr[MAX_COUNTERS];
static int g_nctr;

void rt_watchdog_add_counter(const char *label, uint32_t (*read)(void)) {
  static Mutex lock;
  if (!label || !read)
    return;
  mutexLock(&lock);
  const int n = __atomic_load_n(&g_nctr, __ATOMIC_RELAXED);
  if (n < MAX_COUNTERS) {
    g_ctr[n].label = label;
    g_ctr[n].read = read;
    g_ctr[n].prev = 0;
    __atomic_store_n(&g_nctr, n + 1, __ATOMIC_RELEASE); /* the watchdog sees it filled in */
  }
  mutexUnlock(&lock);
  if (n >= MAX_COUNTERS)
    debugPrintf("[watchdog] counter \"%s\" not added: %d already\n", label, MAX_COUNTERS);
}

/* ------------------------------------------------------------- snapshots */
#define MAX_SNAP 48
#define MAX_RET 40
typedef struct {
  char name[16];
  uint64_t cpu_ms; /* CPU time the thread has run, all told */
  int tid, ok, main, gc_paused;
  s32 prio;
  u64 cores;
  uint32_t pc, lr, sp, psr;
  uint32_t ret[MAX_RET];
  int nret;
} Snap;

static Snap g_snap[MAX_SNAP];
static int g_nsnap;
static Handle g_main_thread;

/* A word that could be a return address: a code address right after a call. */
static int is_return_addr(uint32_t v) {
  if (!dcr_is_code_addr(v & ~1u) || dcr_readable((v & ~3u) - 4, 4) != 4)
    return 0;
  if (!(v & 1)) {
    if (v & 3)
      return 0;
    uint32_t w = *(const volatile uint32_t *)(v - 4);
    return ((w & 0x0F000000u) == 0x0B000000u && (w >> 28) != 0xF) /* bl */
        || (w & 0x0FFFFFF0u) == 0x012FFF30u                         /* blx reg */
        || (w >> 25) == 0x7Du;                                      /* blx imm */
  }
  const uint16_t *h = (const uint16_t *)(uintptr_t)((v & ~1u) - 4);
  return ((h[0] & 0xF800u) == 0xF000u && (h[1] & 0xC000u) == 0xC000u) /* bl/blx T1/T2 */
      || (h[1] & 0xFF87u) == 0x4780u;                                  /* blx reg */
}

/* The code addresses on the stack from sp up (mapped words only). */
static int scan_from_sp(const BThread *t, uint32_t sp, uint32_t *ret, int max) {
  int n = 0;
  uintptr_t lo = (uintptr_t)t->stack_base, hi = lo + t->stack_size;
  if (lo && sp >= lo && sp < hi) {
    uintptr_t end = (sp & ~3u) + dcr_readable(sp & ~3u, 0x3000); /* mapped only */
    for (uintptr_t a = sp & ~3u; a + 4 <= hi && a + 4 <= end && n < max; a += 4) {
      uint32_t v = *(const volatile uint32_t *)a;
      if (dcr_is_code_addr(v & ~1u))
        ret[n++] = v;
    }
  }
  return n;
}

static void snap_one(BThread *t, void *arg) {
  if (g_nsnap >= MAX_SNAP || t->handle == INVALID_HANDLE || t->finished)
    return;
  Snap *s = &g_snap[g_nsnap++];
  memset(s, 0, sizeof *s);
  s->tid = t->tid;
  s->main = t->handle == g_main_thread;
  memcpy(s->name, t->name, sizeof s->name);
  s->name[sizeof s->name - 1] = 0;
  u64 ticks = 0;
  if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, t->handle, TickCountInfo_Total)))
    svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, t->handle, TickCountInfo_Total);
  s->cpu_ms = armTicksToNs(ticks) / 1000000ull;
  s->gc_paused = t->gc_paused;
  s->prio = -1;
  svcGetThreadPriority(&s->prio, t->handle);
  dcr_thread_get_cores(t->handle, NULL, &s->cores);
  /* A thread a GC bridge has paused stays paused; everything else is paused
   * just long enough to read it. Lock order everywhere: the thread list
   * (b_thread_foreach), then the pause lock. */
  b_pause_lock();
  if (dcr_is_emulator()) {
    /* No pause (Ryujinx 1.1.1098 blocks in it for a spinning thread, or
     * crashes the host): the return addresses on the thread's whole stack,
     * innermost first -- live frames and stale ones alike, the live ones
     * nearer the top. */
    uintptr_t lo = (uintptr_t)t->stack_base, hi = lo + t->stack_size;
    if (lo) {
      uintptr_t start = hi;
      while (start > lo && dcr_readable(start - 0x1000, 0x1000) == 0x1000 && hi - start < 0x40000)
        start -= 0x1000;
      for (uintptr_t a = start; a + 4 <= hi && s->nret < MAX_RET; a += 4) {
        uint32_t v = *(const volatile uint32_t *)a;
        if (is_return_addr(v) && (!s->nret || s->ret[s->nret - 1] != v))
          s->ret[s->nret++] = v;
      }
      s->ok = 2;
    }
    b_pause_unlock();
    return;
  }
  int paused_here = !t->gc_paused && R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused));
  KCtx ctx;
  if (R_SUCCEEDED(get_ctx(&ctx, t->handle))) {
    s->ok = 1;
    s->pc = (uint32_t)ctx.pc;
    s->lr = (uint32_t)ctx.r[14];
    s->sp = (uint32_t)ctx.r[13];
    s->psr = ctx.psr;
    s->nret = scan_from_sp(t, s->sp, s->ret, MAX_RET);
  }
  if (paused_here)
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  b_pause_unlock();
}

/* ------------------------------------------------ emergency (lock-free) */
/* When the log lock or the thread list's lock is still taken after a second,
 * the normal report would block on it too (a hang that took the log with it:
 * nothing at all after the last line). This path takes no lock: it prints
 * straight to svcOutputDebugString (an emulator's log; a debugger) -- not to
 * a file: newlib's stdio locks may be held by a paused thread too -- then
 * dumps every thread. */
static void raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void raw(const char *fmt, ...) {
  char buf[600];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n >= (int)sizeof buf)
    n = sizeof buf - 1;
  svcOutputDebugString(buf, (size_t)n);
}

static void raw_one(BThread *t, void *arg) {
  if (t->handle == INVALID_HANDLE || t->finished)
    return;
  /* Under the emulator the context is read without a pause (above). */
  int paused_here = !t->gc_paused && !dcr_is_emulator() &&
                    R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused));
  KCtx ctx;
  uint32_t ret[MAX_RET];
  int nret = 0, ok = R_SUCCEEDED(get_ctx(&ctx, t->handle));
  if (ok)
    nret = scan_from_sp(t, (uint32_t)ctx.r[13], ret, MAX_RET);
  if (paused_here)
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  if (!ok) {
    raw("[watchdog!] tid %d handle 0x%x: no context", t->tid, (unsigned)t->handle);
    return;
  }
  char a[64], b[64], line[400];
  int n = 0;
  for (int k = 0; k < nret && n < (int)sizeof line - 64; k++)
    n += snprintf(line + n, sizeof line - n, " %s", dcr_addr_name(ret[k] & ~1u, a, sizeof a));
  line[n] = 0;
  raw("[watchdog!] tid %d handle 0x%x%s%s: pc %s lr %s | stack:%s", t->tid, (unsigned)t->handle,
      t->handle == g_main_thread ? " (main)" : "", t->gc_paused ? " (GC-paused)" : "",
      dcr_addr_name((uint32_t)ctx.pc, a, sizeof a), dcr_addr_name((uint32_t)ctx.r[14] & ~1u, b, sizeof b), line);
}

/* 1 when a lock stayed taken for a second: the emergency report was made. */
static int emergency(unsigned secs) {
  for (int i = 0; i < 10; i++) {
    uint32_t r, p;
    b_lock_words(&r, &p);
    if (!log_lock_word() && !r && !p)
      return 0; /* all came free: the normal report can run */
    svcSleepThread(100000000ll);
  }
  uint32_t r, p;
  b_lock_words(&r, &p);
  raw("[watchdog!] no frame for %u s AND a lock is stuck: log lock 0x%08lx, thread list 0x%08lx, "
      "pause 0x%08lx (owner handle | 0x40000000 waiters)", secs, (unsigned long)log_lock_word(),
      (unsigned long)r, (unsigned long)p);
  b_thread_foreach_nolock(raw_one, NULL);
  return 1;
}

/* ------------------------------------------------------------------ report */
static void report(uint64_t frames, unsigned secs) {
  if (dcr_is_emulator())
    port_watchdog_emulator_report(secs);
  if (emergency(secs))
    return;
  /* Activity since the previous report: rising counters mean slow, not stuck. */
  static uint32_t p_pr;
  char since[400];
  uint32_t pr = dcr_gl_frames();
  int m = snprintf(since, sizeof since, "presented +%lu", (unsigned long)(pr - p_pr));
  p_pr = pr;
  const int nctr = __atomic_load_n(&g_nctr, __ATOMIC_ACQUIRE);
  for (int i = 0; i < nctr && m < (int)sizeof since - 64; i++) {
    uint32_t v = g_ctr[i].read();
    m += snprintf(since + m, sizeof since - m, ", %s +%lu", g_ctr[i].label,
                  (unsigned long)(v - g_ctr[i].prev));
    g_ctr[i].prev = v;
  }
  g_nsnap = 0;
  b_thread_foreach(snap_one, NULL); /* no logging in here */
  debugPrintf("[watchdog] no frame finished for %u s (last frame %llu), %d threads. Since the "
              "last report: %s\n", secs, (unsigned long long)frames, g_nsnap, since);
  for (int i = 0; i < g_nsnap; i++) {
    const Snap *s = &g_snap[i];
    char a[64], b[64], line[1400];
    int n = 0;
    for (int k = 0; k < s->nret && n < (int)sizeof line - 64; k++)
      n += snprintf(line + n, sizeof line - n, " %s", dcr_addr_name(s->ret[k] & ~1u, a, sizeof a));
    line[n] = 0;
    if (s->ok == 2) { /* emulator: the stack scan only */
      debugPrintf("[watchdog]  tid %d%s \"%s\" CPU %llu ms; return addresses on its stack, innermost "
                  "first (live and stale frames):%s\n", s->tid, s->main ? " (main)" : "", s->name,
                  (unsigned long long)s->cpu_ms, line);
      continue;
    }
    if (!s->ok) {
      debugPrintf("[watchdog]  tid %d%s \"%s\": no context; CPU %llu ms so far\n", s->tid,
                  s->main ? " (main)" : "", s->name, (unsigned long long)s->cpu_ms);
      continue;
    }
    debugPrintf("[watchdog]  tid %d%s%s %s (prio %d, cores 0x%llx, CPU %llu ms): pc %08lx %s%s | "
                "lr %08lx %s | sp %08lx\n",
                s->tid, s->main ? " (main)" : "", s->gc_paused ? " (GC-paused)" : "", s->name,
                (int)s->prio, (unsigned long long)s->cores, (unsigned long long)s->cpu_ms,
                (unsigned long)s->pc, dcr_addr_name(s->pc, a, sizeof a), (s->psr & 0x20) ? " T" : "",
                (unsigned long)s->lr, dcr_addr_name(s->lr & ~1u, b, sizeof b), (unsigned long)s->sp);
    if (s->nret)
      debugPrintf("[watchdog]    stack:%s\n", line);
  }
  log_flush_ring();
}

static void watchdog(void *arg) {
  uint64_t last = dcr_boot_frames(), since = dcr_run_ns();
  int reports = 0;
  for (unsigned tick = 1;; tick++) {
    svcSleepThread(1000000000ll);
    /* not while the log lock is taken: a hang that holds it would hold this
     * thread too, before it could make the emergency report */
    if (tick % 5 == 0 && !log_lock_word())
      log_flush_ring();
    uint64_t f = dcr_boot_frames();
    uint64_t now = dcr_run_ns();
    if (f != last || !dcr_boot_in_focus()) {
      last = f;
      since = now;
      reports = 0;
      continue;
    }
    unsigned secs = (unsigned)((now - since) / 1000000000ull);
    static const unsigned at[] = {10, 40, 100};
    if (reports < 3 && secs >= at[reports]) {
      if (!reports) /* proof of life that takes no lock */
        svcOutputDebugString("[watchdog] 10 s without a frame: reporting", 41);
      report(f, secs);
      reports++;
    }
  }
}

void dcr_watchdog_start(void) {
  static Thread t;
  g_main_thread = envGetMainThreadHandle();
  if (R_SUCCEEDED(threadCreate(&t, watchdog, NULL, NULL, 0x8000, RT_WATCHDOG_PRIO, RT_WATCHDOG_CORE)))
    threadStart(&t);
  else
    debugPrintf("[watchdog] could not start\n");
}
