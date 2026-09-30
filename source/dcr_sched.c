/* dcr_sched.c -- which cores guest threads run on, and how they share them.
 *
 * Android's scheduler is preemptive: a thread that spins waiting for another
 * is time-sliced, and the thread it waits for still runs. Unity relies on
 * that -- its AssetBundle decoder polls its input with no sleep at all
 * (ArchiveStorageConverter::ConversionThreadFunc +0x938b64 ->
 * DecompressAndStore -> ReadHeaderFromMemoryFile: MemoryFile::Size() < 13,
 * try again) while the thread that feeds it finishes the stream. Horizon
 * is not preemptive at the priorities games use: a runnable thread keeps its
 * core until it blocks, and an equal-priority thread queued behind it on that
 * core does not run. Two rules make Horizon behave like Linux for the guest:
 *
 * 1. CORES 0-2. The kernel creates a thread bound to its ideal core alone
 *    (for us the process default, core 0), and the main thread likewise.
 *    svcSetThreadCoreMask takes a 64-bit mask, which the 32-bit ABI passes in
 *    r2:r3 (libnx32's own svcGetThreadCoreMask stub reads it back from r2:r3);
 *    libnx32 declares the argument u32, so r3 carried whatever the caller left
 *    there -- the thread handle, from mutexUnlock -- the kernel refused the
 *    mask (InvalidCoreId) and every guest thread stayed on core 0. The SVCs
 *    are issued here with the high word zeroed, and the result is read back.
 *    (libnx32's svcGetThreadCoreMask stub also unbalances the stack.)
 *
 * 2. PRIORITY 59. Mesosphere's DPC manager rotates the priority-59 queue of
 *    cores 0-2 (63 on core 3) every 10 ms, migrating same-priority threads
 *    between cores as it goes (kern_k_dpc_manager.cpp ->
 *    KScheduler::RotateScheduledQueue). That is the kernel's time slicing, and
 *    it exists at that priority only. So guest threads -- the main thread and
 *    every pthread_create -- run at 59. Our own service threads (audio 0x2A,
 *    vsync and watchdog 0x2B) stay above them: short, and latency-bound.
 *
 * Hardware 2026-09-24 (third run in a row stuck on the Disney screen): the
 * decoder thread spun on core 0 for ever while the main thread -- runnable,
 * in AssetBundle.LoadFromMemory -> FeedStream -> ProcessData -> Thread::Run,
 * about to FinalizeStream the data the decoder was polling for -- queued
 * behind it on the same core at the same priority. (Names: Unity 2017.4.17f1
 * symbols, matched onto this build's code.)
 * MIT.
 */
#include <switch.h>

#include "dcr_sched.h"
#include "rt_settings.h"
#include "util.h"

/* RT_SCHED_SELFTEST: 1 runs the start-up self-test below on hardware (it is
 * always skipped under an emulator); 0 skips it. Every port: 1. */
#ifndef RT_SCHED_SELFTEST
#define RT_SCHED_SELFTEST 1
#endif

/* svcSetThreadCoreMask (0x0F): r0 handle, r1 ideal core, r2:r3 mask. */
Result dcr_thread_set_cores(Handle h, s32 ideal, u64 mask) {
  register uint32_t r0 __asm__("r0") = h;
  register uint32_t r1 __asm__("r1") = (uint32_t)ideal;
  register uint32_t r2 __asm__("r2") = (uint32_t)mask;
  register uint32_t r3 __asm__("r3") = (uint32_t)(mask >> 32);
  __asm__ volatile("svc 0x0F" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r12", "lr", "memory");
  return r0;
}

/* svcGetThreadCoreMask (0x0E): handle in r2; out r1 ideal core, r2:r3 mask. */
Result dcr_thread_get_cores(Handle h, s32 *ideal, u64 *mask) {
  register uint32_t r0 __asm__("r0") = 0;
  register uint32_t r1 __asm__("r1") = 0;
  register uint32_t r2 __asm__("r2") = h;
  register uint32_t r3 __asm__("r3") = 0;
  __asm__ volatile("svc 0x0E" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r12", "lr", "memory");
  if (ideal)
    *ideal = (s32)r1;
  if (mask)
    *mask = R_SUCCEEDED(r0) ? ((u64)r3 << 32 | r2) : 0;
  return r0;
}

/* libnx's own svcSetThreadCoreMask calls (its pthread_create, which Mesa's
 * worker threads come from) come here too (runtime.mk: --wrap). The image's
 * libnx32 passed the mask's high word as garbage; the libnx32 fork the ports
 * build against takes a u64 (c6c53d20), so this is now the same call, kept
 * as the route proven on hardware (Sonic, Asphalt 8). The flag and this
 * symbol ship together: the flag without it fails the link. */
Result __wrap_svcSetThreadCoreMask(Handle h, s32 ideal, u64 mask);
Result __wrap_svcSetThreadCoreMask(Handle h, s32 ideal, u64 mask) { return dcr_thread_set_cores(h, ideal, mask); }

/* New guest threads start on cores 0, 1, 2 in turn; the kernel moves them
 * from there as load demands. */
s32 dcr_sched_next_core(void) {
  static uint32_t next;
  return (s32)(__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % 3);
}

void dcr_sched_guest(Handle h) {
  Result rc = dcr_thread_set_cores(h, -3 /* IdealCoreNoUpdate */, DCR_GUEST_CORES);
  if (R_FAILED(rc)) {
    static int warned;
    if (!warned++)
      debugPrintf("[sched] svcSetThreadCoreMask(0x%x) failed 0x%x: thread stays on one core\n",
                  (unsigned)DCR_GUEST_CORES, rc);
  }
}

/* ------------------------------- self-test ---------------------------------
 * Guest-priority threads spin while the main thread, at the same priority,
 * sleeps 10 ms at a time; each time it wakes it needs a core.
 *   A: spinners on cores 0 and 1 (the main thread's own core is taken). The
 *      kernel moves the main thread to the free core -- if its core mask
 *      allows it. This is the Disney-screen hang, in miniature.
 *   B: a third spinner takes the last core. Now only the kernel's 10 ms
 *      rotation of priority 59 lets the main thread back in.
 * Without either, the main thread would never run again: a 0x2B thread stops
 * the spinners after 1.5 s, and the test reports what happened. */
#define SPINNERS 3
static volatile int st_stop, st_done;
static volatile uint32_t st_cores, st_running;

static void st_spin(void *arg) {
  __atomic_add_fetch(&st_running, 1, __ATOMIC_RELAXED);
  while (!st_stop)
    __atomic_or_fetch(&st_cores, 1u << (svcGetCurrentProcessorNumber() & 31), __ATOMIC_RELAXED);
}

static void st_backstop(void *arg) {
  for (int i = 0; i < 150 && !st_done; i++)
    svcSleepThread(10000000ll);
  st_stop = 1;
}

static u64 ticks_ms(u64 t) { return armTicksToNs(t) / 1000000ull; }

static int st_start(Thread *t, int core) {
  if (R_FAILED(threadCreate(t, st_spin, NULL, NULL, 0x4000, DCR_GUEST_PRIO, core)))
    return 0;
  dcr_sched_guest(t->handle);
  if (R_FAILED(threadStart(t))) {
    threadClose(t);
    return 0;
  }
  return 1;
}

/* Worst of five 10 ms sleeps, or -1 if the main thread only got a core back
 * when the backstop stopped the spinners. */
static int st_measure(void) {
  u64 worst = 0;
  for (int i = 0; i < 5; i++) {
    u64 t0 = armGetSystemTick();
    svcSleepThread(10000000ll);
    u64 ms = ticks_ms(armGetSystemTick() - t0);
    if (st_stop)
      return -1;
    if (ms > worst)
      worst = ms;
  }
  return (int)worst;
}

static void sched_selftest(void) {
  Thread spin[SPINNERS], backstop;
  int n = 0, a = -1, b = -1;
  if (R_FAILED(threadCreate(&backstop, st_backstop, NULL, NULL, 0x4000, 0x2B, -2)) ||
      R_FAILED(threadStart(&backstop))) {
    debugPrintf("[sched] self-test skipped: no backstop thread\n");
    return;
  }
  while (n < 2 && st_start(&spin[n], n))
    n++;
  if (n == 2) {
    svcSleepThread(20000000ll); /* the spinners get going */
    a = st_measure();
    if (a >= 0 && st_start(&spin[n], 2)) {
      n++;
      svcSleepThread(20000000ll);
      b = st_measure();
    }
  }
  st_stop = 1;
  st_done = 1;
  for (int i = 0; i < n; i++) {
    threadWaitForExit(&spin[i]);
    threadClose(&spin[i]);
  }
  threadWaitForExit(&backstop);
  threadClose(&backstop);

  char cores[16];
  int c = 0;
  for (int i = 0; i < 4; i++)
    if (st_cores & (1u << i)) {
      if (c)
        cores[c++] = ',';
      cores[c++] = (char)('0' + i);
    }
  cores[c] = 0;
  debugPrintf("[sched] self-test: %d spinning threads at priority %d ran on cores {%s}\n",
              (int)st_running, DCR_GUEST_PRIO, cores);
  if (a < 0)
    debugPrintf("[sched]   A (a core free): main thread STARVED -- it cannot leave its core; "
                "the game will hang on its first spin-wait\n");
  else
    debugPrintf("[sched]   A (a core free): main thread's 10 ms sleeps took up to %d ms -> it "
                "moved to the free core: OK\n", a);
  if (a >= 0) {
    if (b < 0)
      debugPrintf("[sched]   B (every core busy): main thread starved until the backstop -- no "
                  "time slicing%s\n",
                  dcr_is_emulator() ? " (expected under Ryujinx)" : "");
    else
      debugPrintf("[sched]   B (every core busy): up to %d ms -> kernel time slicing %s\n", b,
                  b <= 60 ? "OK" : "slow, but it ran");
  }
}

void dcr_sched_init(void) {
  s32 prio_before = -1, ideal = -1;
  u64 mask_before = 0, mask_after = 0;
  svcGetThreadPriority(&prio_before, CUR_THREAD_HANDLE);
  dcr_thread_get_cores(CUR_THREAD_HANDLE, &ideal, &mask_before);

  Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, DCR_GUEST_PRIO);
  Result rm = dcr_thread_set_cores(CUR_THREAD_HANDLE, -3, DCR_GUEST_CORES);

  s32 prio_after = -1;
  svcGetThreadPriority(&prio_after, CUR_THREAD_HANDLE);
  dcr_thread_get_cores(CUR_THREAD_HANDLE, &ideal, &mask_after);
  debugPrintf("[sched] main thread: priority %d -> %d%s, cores 0x%llx -> 0x%llx%s (ideal %d)\n",
              (int)prio_before, (int)prio_after, R_FAILED(rp) ? " (set FAILED)" : "",
              (unsigned long long)mask_before, (unsigned long long)mask_after,
              R_FAILED(rm) ? " (set FAILED)" : "", (int)ideal);
  if (R_FAILED(rp) || R_FAILED(rm))
    debugPrintf("[sched]   priority rc 0x%x, core mask rc 0x%x\n", rp, rm);

  /* Ryujinx time-slices priority 59 only intermittently, and after the
   * spinners its GPU emulation no longer presents the GL self-test's frame. */
  if (dcr_is_emulator())
    debugPrintf("[sched] self-test skipped under the emulator\n");
  else if (RT_SCHED_SELFTEST)
    sched_selftest();
}
