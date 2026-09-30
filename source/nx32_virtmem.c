/* nx32_virtmem.c -- libnx32's kernel/virtmem.c, fixed for 32-bit processes.
 *
 * Defining every symbol of libnx's virtmem.o here keeps the linker from pulling
 * that object out of libnx.a (a missed symbol would show up as a duplicate-
 * definition link error, not a silent mix of the two).
 *
 * TWO 32-BIT BUGS IN THE UPSTREAM FILE
 * ------------------------------------
 *  1. Region ends are `base + size` in uintptr_t. A 32-bit process's ASLR
 *     region is [0x200000, 0x200000 + 0xFFE00000) = [.., 0x1_0000_0000): the
 *     end wraps to 0, so every containment/overlap test at the top of the
 *     address space is wrong. Region bounds are u64 here.
 *
 *  2. virtmemFindAslr / virtmemFindCodeMemory search the whole ASLR region.
 *     For 32-bit processes the kernel only accepts Shared / Code / AliasCode /
 *     SharedCode / GeneratedCode / Transfered / ThreadLocal mappings inside the
 *     *code* region -- Mesosphere sets m_alias_code_region = m_code_region =
 *     the small map region [0x200000, 0x40000000) for 32-bit address spaces
 *     (KPageTableBase::InitializeForProcess; CanContain checks the alias-code
 *     region for those states). The large-map space above 0x40000000 holds the
 *     alias and heap regions and nothing else we may map into. So for 32-bit
 *     the search region is the code region, which the kernel reports as the
 *     stack region (stack_region_size 0 => stack region = code region). Seen
 *     as MapSharedMemory() = InvalidCurrentMemory during hidInitialize.
 *
 * Measured (Ryujinx 1.1.1098, 32-bit app): alias 0x40000000+1G, heap
 * 0x80000000+1G, aslr 0x200000+0xFFE00000, stack(code) 0x200000+0x3FE00000,
 * total memory 1 GiB.
 *
 * The libnx32 fork (4.12.0) has both fixes in its own virtmem.c (it does not
 * clamp start - guard at 0, which cannot underflow there: start >= 0x200000).
 * RT_OWN_VIRTMEM (below) chooses; this file stays the default until a
 * hardware boot per engine family has run on the fork's.
 *
 * Original: Copyright libnx authors (ISC). Changes MIT.
 */
#include <switch.h>
#include <stdint.h>

#include "rt_settings.h"

/* RT_OWN_VIRTMEM: 1 = this file replaces libnx's virtmem.o (every port today,
 * hardware-proven); 0 = the file is empty and the libnx32 fork's virtmem.c
 * (4.12.0 or later: same fixes) is linked instead. Every code mapping (the
 * loader, code_flush's page, hid/time shared memory) goes through it. */
#ifndef RT_OWN_VIRTMEM
#define RT_OWN_VIRTMEM 1
#endif

#if RT_OWN_VIRTMEM

#define SEQUENTIAL_GUARD_REGION_SIZE 0x1000
#define RANDOM_MAX_ATTEMPTS 0x200

typedef struct {
  u64 start;
  u64 end;   /* exclusive; u64 so 0x1_0000_0000 is representable */
} MemRegion;

struct VirtmemReservation {
  VirtmemReservation *next;
  VirtmemReservation *prev;
  MemRegion region;
};

static Mutex g_VirtmemMutex;
static MemRegion g_AliasRegion;
static MemRegion g_HeapRegion;
static MemRegion g_AslrRegion;
static MemRegion g_StackRegion;
static MemRegion *g_MapRegion;   /* where Shared/Code/etc. may go */
static VirtmemReservation *g_Reservations;

void *__libnx_alloc(size_t size);
void __libnx_free(void *p);

uintptr_t __attribute__((weak)) __libnx_virtmem_rng(void) {
  return (uintptr_t)randomGet64();
}

static Result region_from_info(MemRegion *r, InfoType addr_id, InfoType size_id) {
  u64 base = 0, size = 0;
  Result rc = svcGetInfo(&base, addr_id, CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(rc))
    rc = svcGetInfo(&size, size_id, CUR_PROCESS_HANDLE, 0);
  if (R_SUCCEEDED(rc)) {
    r->start = base;
    r->end = base + size;
  }
  return rc;
}

static inline bool overlaps(const MemRegion *r, u64 start, u64 end) {
  return start < r->end && r->start < end;
}

/* Anything mapped in [start-guard, end+guard)? */
static bool is_mapped(u64 start, u64 end, u64 guard) {
  start = start > guard ? start - guard : 0;
  end += guard;
  MemoryInfo mi;
  u32 pi;
  Result rc = svcQueryMemory(&mi, &pi, (u32)start);
  if (R_FAILED(rc))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadQueryMemory));
  const u64 memend = (u64)mi.addr + mi.size;
  return mi.type != MemType_Unmapped || end > memend;
}

static bool is_reserved(u64 start, u64 end, u64 guard) {
  start = start > guard ? start - guard : 0;
  end += guard;
  for (VirtmemReservation *rv = g_Reservations; rv; rv = rv->next)
    if (overlaps(&rv->region, start, end))
      return true;
  return false;
}

static void *find_random(const MemRegion *r, size_t size, size_t guard) {
  size = (size + 0xFFF) & ~0xFFF;
  guard = (guard + 0xFFF) & ~0xFFF;
  const u64 region_size = r->end - r->start;
  if ((u64)size > region_size)
    return NULL;
  const u64 max_page = (region_size - size) >> 12;
  for (unsigned i = 0; i < RANDOM_MAX_ATTEMPTS; i++) {
    const u64 addr = r->start + (((u64)__libnx_virtmem_rng() % (max_page + 1)) << 12);
    const u64 end = addr + size;
    if (overlaps(&g_AliasRegion, addr, end) || overlaps(&g_HeapRegion, addr, end))
      continue;
    if (is_mapped(addr, end, guard) || is_reserved(addr, end, guard))
      continue;
    return (void *)(uintptr_t)addr;
  }
  return NULL;
}

void virtmemSetup(void) {
  if (R_FAILED(region_from_info(&g_AliasRegion, InfoType_AliasRegionAddress,
                                InfoType_AliasRegionSize)))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_WeirdKernel));

  u64 alias_extra = 0;
  if (R_SUCCEEDED(svcGetInfo(&alias_extra, InfoType_AliasRegionExtraSize, CUR_PROCESS_HANDLE, 0)))
    g_AliasRegion.end -= alias_extra;

  if (R_FAILED(region_from_info(&g_HeapRegion, InfoType_HeapRegionAddress,
                                InfoType_HeapRegionSize)))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGetInfo_Heap));

  if (R_FAILED(region_from_info(&g_AslrRegion, InfoType_AslrRegionAddress,
                                InfoType_AslrRegionSize)) ||
      R_FAILED(region_from_info(&g_StackRegion, InfoType_StackRegionAddress,
                                InfoType_StackRegionSize)))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_WeirdKernel));

  /* 32-bit: code-region-only mapping states live in the stack(=code) region. */
  g_MapRegion = &g_StackRegion;
}

void virtmemLock(void) { mutexLock(&g_VirtmemMutex); }
void virtmemUnlock(void) { mutexUnlock(&g_VirtmemMutex); }

void *virtmemFindAslr(size_t size, size_t guard_size) {
  if (!mutexIsLockedByCurrentThread(&g_VirtmemMutex))
    return NULL;
  return find_random(g_MapRegion, size, guard_size);
}

void *virtmemFindStack(size_t size, size_t guard_size) {
  if (!mutexIsLockedByCurrentThread(&g_VirtmemMutex))
    return NULL;
  return find_random(&g_StackRegion, size, guard_size);
}

void *virtmemFindCodeMemory(size_t size, size_t guard_size) {
  if (!mutexIsLockedByCurrentThread(&g_VirtmemMutex))
    return NULL;
  return find_random(g_MapRegion, size, guard_size);
}

VirtmemReservation *virtmemAddReservation(void *mem, size_t size) {
  if (!mutexIsLockedByCurrentThread(&g_VirtmemMutex))
    return NULL;
  VirtmemReservation *rv = (VirtmemReservation *)__libnx_alloc(sizeof(*rv));
  if (rv) {
    rv->region.start = (uintptr_t)mem;
    rv->region.end = rv->region.start + size;
    rv->next = g_Reservations;
    rv->prev = NULL;
    g_Reservations = rv;
    if (rv->next)
      rv->next->prev = rv;
  }
  return rv;
}

void virtmemRemoveReservation(VirtmemReservation *rv) {
  if (!mutexIsLockedByCurrentThread(&g_VirtmemMutex))
    return;
  if (rv->next)
    rv->next->prev = rv->prev;
  if (rv->prev)
    rv->prev->next = rv->next;
  else
    g_Reservations = rv->next;
  __libnx_free(rv);
}
#else
typedef int nx32_virtmem_not_used; /* (not an empty translation unit) */
#endif /* RT_OWN_VIRTMEM */
