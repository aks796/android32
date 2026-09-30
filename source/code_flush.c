/* code_flush.c -- instruction-cache maintenance for a 32-bit process.
 *
 * (The cache half of the Crossy Road port's jit_arena.c; this port has no JIT.)
 *
 * A 32-bit (AArch32) EL0 thread cannot run cache maintenance at all -- there
 * is no EL0 form of DCCMVAU / ICIMVAU in AArch32, which is why 32-bit Linux
 * has the cacheflush syscall -- and libnx32 defines armICacheInvalidate as
 * (void)0. Code written at run time (Homura's hooks, byte patches toggled from
 * its menu) could otherwise run as whatever the I-cache fetched there before.
 *
 * The kernel invalidates EVERY core's instruction cache (IC IALLUIS plus an
 * instruction barrier on each core) whenever a Code / AliasCode page gains or
 * loses execute permission (KPageTableBase::SetProcessMemoryPermission). So a
 * dedicated page -- mapped with svcMapProcessCodeMemory, never executed -- is
 * flipped R <-> RX once per invalidation. The data side is cleaned first with
 * svcFlushProcessDataCache on the written range, since the I-cache refills from
 * L2/memory, not from the L1 data cache. Proven on hardware in the Crossy Road
 * port (jit self-test 3, 2026-09-23). MIT.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "code_flush.h"
#include "selfproc.h"
#include "util.h"

#define PAGE 0x1000u

static Mutex g_ic_lock;
static uintptr_t g_ic_page; /* 0: not set up yet, 1: unavailable */
static int g_ic_x;          /* the page's current permission includes X */

static int ic_page_init(void) {
  Handle self = dcr_self_process();
  void *src = memalign(PAGE, PAGE); /* donated to the mapping: never freed */
  if (self == INVALID_HANDLE || !src)
    return -1;
  memset(src, 0, PAGE);
  virtmemLock();
  void *dst = virtmemFindCodeMemory(PAGE, PAGE);
  VirtmemReservation *rv = dst ? virtmemAddReservation(dst, PAGE) : NULL;
  virtmemUnlock();
  if (!rv)
    return -1;
  Result rc = svcMapProcessCodeMemory(self, (u64)(uintptr_t)dst, (u64)(uintptr_t)src, PAGE);
  if (R_SUCCEEDED(rc))
    rc = svcSetProcessMemoryPermission(self, (u64)(uintptr_t)dst, PAGE, Perm_R);
  if (R_FAILED(rc)) {
    debugPrintf("[code] I-cache flip page: 0x%x -- patched code may run stale bytes\n", rc);
    return -1;
  }
  g_ic_page = (uintptr_t)dst;
  g_ic_x = 0;
  debugPrintf("[code] I-cache maintenance: flip page at %p\n", dst);
  return 0;
}

void dcr_icache_invalidate(void) {
  if (dcr_is_emulator())
    return; /* the emulator's code cache tracks guest writes itself */
  mutexLock(&g_ic_lock);
  if (!g_ic_page && ic_page_init() != 0)
    g_ic_page = 1;
  if (g_ic_page > 1) {
    g_ic_x ^= 1;
    Result rc = svcSetProcessMemoryPermission(CUR_PROCESS_HANDLE, (u64)g_ic_page, PAGE,
                                              g_ic_x ? Perm_Rx : Perm_R);
    if (R_FAILED(rc)) {
      static int warned;
      if (!warned++)
        debugPrintf("[code] I-cache flip failed: 0x%x\n", rc);
      g_ic_x ^= 1;
    }
  }
  mutexUnlock(&g_ic_lock);
}

void dcr_code_flush(void *code, size_t size) {
  if (!size)
    return;
  Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u64)(uintptr_t)code, size);
  if (R_FAILED(rc)) {
    static int warned;
    if (!warned++)
      debugPrintf("[code] data-cache flush of %p+0x%x failed: 0x%x\n", code, (unsigned)size, rc);
  }
  dcr_icache_invalidate();
}
