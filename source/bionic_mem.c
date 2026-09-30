/* bionic_mem.c -- mmap and friends for the bionic ABI, plus the code-aware
 * memory primitives.
 *
 * There is no virtual memory API for applications beyond the heap, so:
 *   - anonymous mappings are page-aligned heap blocks (zeroed);
 *   - file mappings are heap blocks filled from the file (MAP_PRIVATE
 *     semantics; the fd's position is never moved: b_pread_all);
 *   - a port's code space is asked first (codespace.h, weak defaults in the
 *     loader): a hooking library's small mappings (trampolines) can come from
 *     the port's trampoline pool, which becomes executable when the engine is
 *     sealed;
 *   - PROT_NONE reservations are backed too (logged when large, since 32-bit
 *     address space and the heap region are the budget).
 * mprotect() of code the port manages goes to cs_mprotect (never a real
 * permission change); elsewhere it is accepted and ignored, except that
 * granting EXEC is logged: that code would not be executable on hardware.
 *
 * memcpy / memmove / memset (and their __aeabi_ / _chk forms) check -- one
 * load, while nothing is armed -- whether the destination is sealed code its
 * owner has mprotect()ed writable, and then write through cs_write. That is
 * how a mod's byte patches toggle during play. The runtime fills memory
 * cs_mmap handed out through cs_rw_alias (its writable view, where code has
 * one).
 *
 * b_mmap, b_munmap, b_mremap, b_mprotect, b_mmap_bytes and memcpy / memmove /
 * memset are weak: a port with memory of its own (dcr: a JIT arena with an
 * executable and a writable view) may replace them. MIT.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_io.h"
#include "codespace.h"
#include "error.h"
#include "util.h"

/* =============================== mapping table ============================= */
typedef struct {
  uintptr_t addr;
  size_t len;
  int exec;
} Mapping;

#define MAX_MAPPINGS 4096
static Mapping g_maps[MAX_MAPPINGS];
static int g_nmaps;
static Mutex g_map_lock;
static u64 g_mapped_bytes;

static int map_find(uintptr_t a) {
  for (int i = 0; i < g_nmaps; i++)
    if (a >= g_maps[i].addr && a < g_maps[i].addr + g_maps[i].len)
      return i;
  return -1;
}

static void map_add(uintptr_t a, size_t len, int exec) {
  mutexLock(&g_map_lock);
  if (g_nmaps < MAX_MAPPINGS) {
    g_maps[g_nmaps++] = (Mapping){a, len, exec};
    g_mapped_bytes += len;
  } else {
    static int warned;
    if (!warned++)
      debugPrintf("[mmap] mapping table full; munmap of later mappings will leak\n");
  }
  mutexUnlock(&g_map_lock);
}

__attribute__((weak)) void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, b_off_t off) {
  if (!len) {
    b_set_errno(L_EINVAL);
    return L_MAP_FAILED;
  }
  size_t alen = (len + 0xFFF) & ~0xFFFu;

  /* MAP_FIXED inside something we already handed out: the caller is committing
   * or re-initialising part of its own reservation. */
  if ((flags & L_MAP_FIXED) && addr) {
    mutexLock(&g_map_lock);
    int i = map_find((uintptr_t)addr);
    mutexUnlock(&g_map_lock);
    if (i >= 0) {
      if (flags & L_MAP_ANONYMOUS)
        memset(addr, 0, len);
      else if (fd >= 0)
        b_pread_all(fd, addr, len, off);
      return addr;
    }
    debugPrintf("[mmap] MAP_FIXED at unknown %p (%u KB) refused\n", addr, (unsigned)(len >> 10));
    b_set_errno(L_EINVAL);
    return L_MAP_FAILED;
  }

  /* Memory for code the port manages: a trampoline (executable once the
   * engine is sealed), or a JIT block -- whose file contents, if any, go in
   * through its writable view. */
  if ((flags & L_MAP_ANONYMOUS) || (prot & L_PROT_EXEC)) {
    void *c = cs_mmap(len, prot, __builtin_return_address(0));
    if (c) {
      if (!(flags & L_MAP_ANONYMOUS) && fd >= 0) {
        char *w = cs_rw_alias(c, alen);
        size_t got = b_pread_all(fd, w, len, off);
        if (got < alen)
          memset(w + got, 0, alen - got);
      }
      return c;
    }
  }
  void *p;
  int exec = (prot & L_PROT_EXEC) != 0;
  if (exec) {
    static int warned;
    if (warned++ < 8)
      debugPrintf("[mmap] executable mapping (%u KB) from %p: code there will not execute on "
                  "hardware\n", (unsigned)(alen >> 10), __builtin_return_address(0));
  }
  p = memalign(0x1000, alen);
  if (p && (flags & L_MAP_ANONYMOUS))
    memset(p, 0, alen);
  if (!p) {
    debugPrintf("[mmap] out of memory for %u KB (prot %d)\n", (unsigned)(alen >> 10), prot);
    b_set_errno(L_ENOMEM);
    return L_MAP_FAILED;
  }
  if (!(flags & L_MAP_ANONYMOUS) && fd >= 0) {
    size_t got = b_pread_all(fd, p, len, off);
    if (got < alen)
      memset((char *)p + got, 0, alen - got);
  }
  if (alen >= (16u << 20))
    debugPrintf("[mmap] large mapping %u MB prot=%d flags=0x%x -> %p\n", (unsigned)(alen >> 20),
                prot, flags, p);
  map_add((uintptr_t)p, alen, exec);
  return p;
}

__attribute__((weak)) int b_munmap(void *addr, size_t len) {
  if (cs_munmap(addr, len))
    return 0;
  mutexLock(&g_map_lock);
  int i = map_find((uintptr_t)addr);
  if (i < 0) {
    mutexUnlock(&g_map_lock);
    return 0; /* not ours / already gone: bionic returns 0 for unmapped ranges */
  }
  Mapping m = g_maps[i];
  if ((uintptr_t)addr != m.addr || ((len + 0xFFF) & ~0xFFFu) < m.len) {
    /* Partial unmap: keep the block (the memory stays valid for the rest). */
    mutexUnlock(&g_map_lock);
    return 0;
  }
  g_maps[i] = g_maps[--g_nmaps];
  g_mapped_bytes -= m.len;
  mutexUnlock(&g_map_lock);
  free(addr);
  return 0;
}

__attribute__((weak)) void *b_mremap(void *old, size_t old_len, size_t new_len, int flags, ...) {
  if (new_len <= old_len)
    return old;
  if (!(flags & L_MREMAP_MAYMOVE)) {
    b_set_errno(L_ENOMEM);
    return L_MAP_FAILED;
  }
  mutexLock(&g_map_lock);
  int i = map_find((uintptr_t)old);
  int exec = i >= 0 ? g_maps[i].exec : 0;
  mutexUnlock(&g_map_lock);
  void *n = b_mmap(NULL, new_len, L_PROT_READ | L_PROT_WRITE | (exec ? L_PROT_EXEC : 0),
                   L_MAP_PRIVATE | L_MAP_ANONYMOUS, -1, 0);
  if (n == L_MAP_FAILED)
    return n;
  memcpy(cs_rw_alias(n, new_len), old, old_len);
  b_munmap(old, old_len);
  return n;
}

__attribute__((weak)) int b_mprotect(void *addr, size_t len, int prot) {
  if (cs_mprotect(addr, len, prot, __builtin_return_address(0)))
    return 0;
  if (prot & L_PROT_EXEC) {
    static int warned;
    if (warned++ < 8)
      debugPrintf("[mmap] mprotect(%p, %u, EXEC) from %p outside the game's code -- code there "
                  "will not execute on hardware\n",
                  addr, (unsigned)len, __builtin_return_address(0));
  }
  return 0;
}

int b_madvise(void *addr, size_t len, int advice) { return 0; }

__attribute__((weak)) u64 b_mmap_bytes(void) { return g_mapped_bytes; }

/* ======================== code-aware memory primitives ===================== */
__attribute__((weak)) void *b_memcpy(void *d, const void *s, size_t n) {
  if (__builtin_expect(g_cs_armed, 0) && cs_write(d, s, n, 0, 0))
    return d;
  return memcpy(d, s, n);
}

__attribute__((weak)) void *b_memmove(void *d, const void *s, size_t n) {
  if (__builtin_expect(g_cs_armed, 0) && cs_write(d, s, n, 0, 1))
    return d;
  return memmove(d, s, n);
}

__attribute__((weak)) void *b_memset(void *d, int c, size_t n) {
  if (__builtin_expect(g_cs_armed, 0) && cs_write(d, NULL, n, c, 2))
    return d;
  return memset(d, c, n);
}

/* FORTIFY forms (for libraries built with _FORTIFY_SOURCE). */
void *b___memcpy_chk(void *d, const void *s, size_t n, size_t dlen) {
  if (n > dlen)
    fatal_error("__memcpy_chk: %u bytes into a %u-byte buffer (from %p)", (unsigned)n,
                (unsigned)dlen, __builtin_return_address(0));
  return b_memcpy(d, s, n);
}
void *b___memmove_chk(void *d, const void *s, size_t n, size_t dlen) {
  if (n > dlen)
    fatal_error("__memmove_chk: %u bytes into a %u-byte buffer (from %p)", (unsigned)n,
                (unsigned)dlen, __builtin_return_address(0));
  return b_memmove(d, s, n);
}
void *b___memset_chk(void *d, int c, size_t n, size_t dlen) {
  if (n > dlen)
    fatal_error("__memset_chk: %u bytes into a %u-byte buffer (from %p)", (unsigned)n,
                (unsigned)dlen, __builtin_return_address(0));
  return b_memset(d, c, n);
}

/* ARM EABI helpers. NB __aeabi_memset's argument order is (dest, n, c). */
void b___aeabi_memcpy(void *d, const void *s, size_t n) { b_memcpy(d, s, n); }
void b___aeabi_memmove(void *d, const void *s, size_t n) { b_memmove(d, s, n); }
void b___aeabi_memset(void *d, size_t n, int c) { b_memset(d, c, n); }
void b___aeabi_memclr(void *d, size_t n) { b_memset(d, 0, n); }
/* The 4/8 variants only promise alignment; same behaviour. */
void b___aeabi_memcpy4(void *d, const void *s, size_t n) __attribute__((alias("b___aeabi_memcpy")));
void b___aeabi_memcpy8(void *d, const void *s, size_t n) __attribute__((alias("b___aeabi_memcpy")));
void b___aeabi_memmove4(void *d, const void *s, size_t n) __attribute__((alias("b___aeabi_memmove")));
void b___aeabi_memmove8(void *d, const void *s, size_t n) __attribute__((alias("b___aeabi_memmove")));
void b___aeabi_memset4(void *d, size_t n, int c) __attribute__((alias("b___aeabi_memset")));
void b___aeabi_memset8(void *d, size_t n, int c) __attribute__((alias("b___aeabi_memset")));
void b___aeabi_memclr4(void *d, size_t n) __attribute__((alias("b___aeabi_memclr")));
void b___aeabi_memclr8(void *d, size_t n) __attribute__((alias("b___aeabi_memclr")));
