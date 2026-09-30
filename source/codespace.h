/* codespace.h -- run-time code writes by the game's own modules (lab_loader.c: nothing to do for this game).
 *
 * The bionic memory shims (bionic_mem.c) ask here first: an mmap/mprotect/
 * munmap that concerns code, and a memcpy/memmove/memset whose destination is
 * sealed code that its owner mprotect()ed writable, are handled here. MIT. */
#ifndef DCR_CODESPACE_H
#define DCR_CODESPACE_H
#include <stddef.h>

/* mmap: a chunk of the trampoline pool while the hook phase is on, else NULL. */
void *cs_mmap(size_t len, int prot, const void *caller);
/* munmap / mprotect: 1 when the range is code this file manages (the call is
 * then done, and returns 0 to the game), 0 to let the shim handle it. */
int cs_munmap(void *addr, size_t len);
int cs_mprotect(void *addr, size_t len, int prot, const void *caller);

/* Non-zero while any sealed-code range is armed for writing. Checked before
 * cs_write so the hot memcpy path costs one load. */
extern volatile int g_cs_armed;
/* kind: 0 memcpy, 1 memmove, 2 memset(c). 1 = done (through so_patch_code). */
int cs_write(void *dst, const void *src, size_t n, int c, int kind);

#endif
