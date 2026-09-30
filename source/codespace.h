/* codespace.h -- run-time code writes by the game's own modules.
 *
 * The bionic memory shims (bionic_mem.c) ask here first: an mmap/mprotect/
 * munmap that concerns code, and a memcpy/memmove/memset whose destination is
 * sealed code that its owner mprotect()ed writable, are handled here. The
 * runtime's defaults (codespace.c, all weak) manage no code of their own; a
 * port whose engine writes code at run time overrides them (the PvZ port:
 * a trampoline pool for its mod's hooks and writes into sealed text through
 * so_patch_code; the Crossy Road port: its Mono JIT arena). MIT. */
#ifndef DCR_CODESPACE_H
#define DCR_CODESPACE_H
#include <stddef.h>

/* mmap: memory for code the port manages (a trampoline pool chunk, a JIT
 * arena block), or NULL to let the shim allocate. Default: NULL. */
void *cs_mmap(size_t len, int prot, const void *caller);
/* munmap / mprotect: 1 when the range is code this file manages (the call is
 * then done, and returns 0 to the game), 0 to let the shim handle it.
 * Defaults: munmap 0; mprotect 1 for a loaded module's own pages (text stays
 * RX and data RW whatever the engine asks: nothing to do, nothing to warn
 * about), else 0. */
int cs_munmap(void *addr, size_t len);
int cs_mprotect(void *addr, size_t len, int prot, const void *caller);

/* Non-zero while any sealed-code range is armed for writing. Checked before
 * cs_write so the hot memcpy path costs one load. Default: 0 (weak data). */
extern volatile int g_cs_armed;
/* kind: 0 memcpy, 1 memmove, 2 memset(c). 1 = done (through so_patch_code,
 * or another view of the code). Default: 0. */
int cs_write(void *dst, const void *src, size_t n, int c, int kind);

/* Where to write `len` bytes that belong at `addr` (an address cs_mmap
 * returned, e.g. to fill a file-backed or MAP_FIXED executable mapping):
 * addr itself unless the port's code has a separate writable view (the
 * Crossy Road JIT arena's RW alias). Default: addr. */
void *cs_rw_alias(void *addr, size_t len);

#endif
