/* watchdog.c -- keeps the log reaching the SD card, and reports hangs.
 *
 * During play the log goes to a RAM ring (util.c), written out periodically.
 * If the engine stops presenting frames, the evidence would stay in RAM (the
 * Crossy Road port's first hang: debug.log ended at frame 3). So this thread,
 * created with libnx directly:
 *   - flushes the ring every 5 s whatever the engine is doing;
 *   - when no frame has completed for 10 s while in focus, snapshots every
 *     registered thread -- pause, read registers and the top of its stack,
 *     resume -- and only then logs, with addresses named module+offset.
 *     Nothing is logged while a thread is paused: it may hold the log lock.
 * Reports repeat at 40 s and 100 s of the same hang, then stop. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "dcr_sched.h"
#include "dcr_time.h"
#include "util.h"

const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.c */
int dcr_is_code_addr(uint32_t a);                               /* exc_handler.c */
size_t dcr_readable(uint32_t p, size_t want);                   /* exc_handler.c */
uint64_t dcr_boot_frames(void);                                 /* lab_boot.c */
int dcr_boot_in_focus(void);                                    /* lab_boot.c */
uint32_t dcr_gl_frames(void);                                   /* gl_mesa.c */
uint32_t lab_audio_mixes(void);                                /* lab_audio.c */

/* The kernel's ThreadContext (svcGetThreadContext3); for an AArch32 thread
 * r[0..14] hold r0-r14. Same layout as mono_rt.c. */
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

#define MAX_SNAP 48
#define MAX_RET 12
typedef struct {
  int tid, ok, main;
  char name[16];
  s32 prio;
  u64 cores;
  uint32_t pc, lr, sp, psr;
  uint32_t ret[MAX_RET];
  int nret;
} Snap;

static Snap g_snap[MAX_SNAP];
static int g_nsnap;
static Handle g_main_thread;

static void snap_one(BThread *t, void *arg) {
  if (g_nsnap >= MAX_SNAP || t->handle == INVALID_HANDLE || t->finished)
    return;
  Snap *s = &g_snap[g_nsnap++];
  memset(s, 0, sizeof *s);
  s->tid = t->tid;
  s->main = t->handle == g_main_thread;
  memcpy(s->name, t->name, sizeof s->name);
  s->name[sizeof s->name - 1] = 0;
  s->prio = -1;
  svcGetThreadPriority(&s->prio, t->handle);
  dcr_thread_get_cores(t->handle, NULL, &s->cores);
  /* Paused just long enough to read it. Lock order: the thread list
   * (b_thread_foreach), then the pause lock. */
  b_pause_lock();
  int paused_here = R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused));
  KCtx ctx;
  if (R_SUCCEEDED(get_ctx(&ctx, t->handle))) {
    s->ok = 1;
    s->pc = (uint32_t)ctx.pc;
    s->lr = (uint32_t)ctx.r[14];
    s->sp = (uint32_t)ctx.r[13];
    s->psr = ctx.psr;
    uintptr_t lo = (uintptr_t)t->stack_base, hi = lo + t->stack_size;
    if (lo && s->sp >= lo && s->sp < hi) {
      uintptr_t end = (s->sp & ~3u) + dcr_readable(s->sp & ~3u, 0x3000); /* mapped only */
      for (uintptr_t a = s->sp & ~3u; a + 4 <= hi && a + 4 <= end && s->nret < MAX_RET; a += 4) {
        uint32_t v = *(const volatile uint32_t *)a;
        if (dcr_is_code_addr(v & ~1u))
          s->ret[s->nret++] = v;
      }
    }
  }
  if (paused_here)
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  b_pause_unlock();
}

static void report(uint64_t frames, unsigned secs) {
  /* Activity since the previous report: rising counters mean slow, not stuck. */
  static uint32_t p_au, p_pr;
  uint32_t au = lab_audio_mixes(), pr = dcr_gl_frames();
  g_nsnap = 0;
  b_thread_foreach(snap_one, NULL); /* no logging in here */
  debugPrintf("[watchdog] no frame finished for %u s (last frame %llu), %d threads. Since the "
              "last report: presented +%lu, audio writes +%lu\n",
              secs, (unsigned long long)frames, g_nsnap, (unsigned long)(pr - p_pr),
              (unsigned long)(au - p_au));
  p_au = au, p_pr = pr;
  for (int i = 0; i < g_nsnap; i++) {
    const Snap *s = &g_snap[i];
    char a[64], b[64];
    if (!s->ok) {
      debugPrintf("[watchdog]  tid %d%s: no context\n", s->tid, s->main ? " (main)" : "");
      continue;
    }
    debugPrintf("[watchdog]  tid %d%s %s (prio %d, cores 0x%llx): pc %08lx %s%s | lr %08lx %s | "
                "sp %08lx\n",
                s->tid, s->main ? " (main)" : "", s->name, (int)s->prio,
                (unsigned long long)s->cores, (unsigned long)s->pc, dcr_addr_name(s->pc, a, sizeof a),
                (s->psr & 0x20) ? " T" : "", (unsigned long)s->lr, dcr_addr_name(s->lr & ~1u, b, sizeof b),
                (unsigned long)s->sp);
    char line[512];
    int n = 0;
    for (int k = 0; k < s->nret && n < (int)sizeof line - 64; k++)
      n += snprintf(line + n, sizeof line - n, " %s", dcr_addr_name(s->ret[k] & ~1u, a, sizeof a));
    if (s->nret)
      debugPrintf("[watchdog]    stack:%s\n", line);
  }
  log_flush_ring();
}

/* Time is the process's own (bionic_time.c): a freeze for sleep or the
 * HOME menu is not a hang, though no frame came during it. */
static void watchdog(void *arg) {
  uint64_t last = dcr_boot_frames(), since = dcr_run_ns();
  int reports = 0;
  for (unsigned tick = 1;; tick++) {
    svcSleepThread(1000000000ll);
    if (tick % 5 == 0)
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
      report(f, secs);
      reports++;
    }
  }
}

void dcr_watchdog_start(void) {
  static Thread t;
  g_main_thread = envGetMainThreadHandle();
  if (R_SUCCEEDED(threadCreate(&t, watchdog, NULL, NULL, 0x8000, 0x2B, -2)))
    threadStart(&t);
  else
    debugPrintf("[watchdog] could not start\n");
}
