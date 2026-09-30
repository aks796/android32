/* codespace.c -- the runtime's defaults for the codespace hooks (codespace.h).
 *
 * All weak: most engines write no code at run time, so there is nothing to
 * manage; a port that has some defines the strong versions (any subset: each
 * one it leaves out keeps the default here). cs_mprotect accepts a loaded
 * module's own pages without a change or a warning (the Labyrinth 2 / Angry
 * Birds Space / Sonic behaviour; the others returned 0, which only added
 * "EXEC outside the game's code" log lines for those pages -- same result
 * for the game either way). MIT.
 */
#include <stddef.h>

#include "codespace.h"
#include "so_util.h"

__attribute__((weak)) volatile int g_cs_armed;

__attribute__((weak)) void *cs_mmap(size_t len, int prot, const void *caller) {
  (void)len, (void)prot, (void)caller;
  return NULL;
}

__attribute__((weak)) int cs_munmap(void *addr, size_t len) {
  (void)addr, (void)len;
  return 0;
}

__attribute__((weak)) int cs_mprotect(void *addr, size_t len, int prot, const void *caller) {
  (void)len, (void)prot, (void)caller;
  /* The engine's own pages: never a real change (text stays RX, data RW). */
  return so_find_module_by_addr(addr) != NULL;
}

__attribute__((weak)) int cs_write(void *dst, const void *src, size_t n, int c, int kind) {
  (void)dst, (void)src, (void)n, (void)c, (void)kind;
  return 0;
}

__attribute__((weak)) void *cs_rw_alias(void *addr, size_t len) {
  (void)len;
  return addr;
}
