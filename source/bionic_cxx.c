/* bionic_cxx.c -- the C++ ABI of Android's minimal libstdc++.so: operator
 * new / delete, the thread-safe static-initialisation guards and the
 * pure-virtual trap.
 *
 * Android's "system" STL (NDK r5-r8) is tiny: operator new is malloc and
 * never throws (a NULL comes back; engines built against it check nothing,
 * as on a phone), and the nothrow forms are the same. Engines built with a
 * full STL bring their own new/delete and never import these.
 *
 * The guards follow the ARM C++ ABI (3.2.3): a 32-bit word whose bit 0 says
 * "constructed"; the compiler tests that bit inline and calls acquire only
 * while it is 0. 0x100 marks an initialisation in progress (as bionic's
 * does), and its waiters sleep on one process-wide condition variable. No
 * lock is held while the constructor runs, so a constructor that starts a
 * thread which reaches another guard -- or the same one -- cannot deadlock
 * against it (a recursive lock held through the construction could). MIT.
 */
#include <stdint.h>
#include <stdlib.h>
#include <switch.h>

#include "error.h"
#include "util.h"

/* ---------------------------------------------------------- new / delete */
void *b__Znwj(size_t n) { return malloc(n ? n : 1); }
void *b__Znaj(size_t n) { return malloc(n ? n : 1); }
void b__ZdlPv(void *p) { free(p); }
void b__ZdaPv(void *p) { free(p); }

/* std::nothrow (an empty object: only its address is used) */
const uint8_t b__ZSt7nothrow[4];

void *b__ZnwjRKSt9nothrow_t(size_t n, const void *nt) { return malloc(n ? n : 1); }
void *b__ZnajRKSt9nothrow_t(size_t n, const void *nt) { return malloc(n ? n : 1); }
void b__ZdlPvRKSt9nothrow_t(void *p, const void *nt) { free(p); }
void b__ZdaPvRKSt9nothrow_t(void *p, const void *nt) { free(p); }

/* ---------------------------------------------------- static-local guards */
#define GUARD_DONE 1u
#define GUARD_PENDING 0x100u

static Mutex g_guard_lock;
static CondVar g_guard_cv;

int b___cxa_guard_acquire(volatile uint32_t *g) {
  if (__atomic_load_n(g, __ATOMIC_ACQUIRE) & GUARD_DONE)
    return 0;
  mutexLock(&g_guard_lock);
  for (;;) {
    uint32_t v = *g;
    if (v & GUARD_DONE) {
      mutexUnlock(&g_guard_lock);
      return 0;
    }
    if (!(v & GUARD_PENDING)) {
      *g = v | GUARD_PENDING;
      mutexUnlock(&g_guard_lock);
      return 1; /* this thread constructs, then releases (or aborts) */
    }
    condvarWait(&g_guard_cv, &g_guard_lock);
  }
}

void b___cxa_guard_release(volatile uint32_t *g) {
  mutexLock(&g_guard_lock);
  __atomic_store_n(g, GUARD_DONE, __ATOMIC_RELEASE);
  condvarWakeAll(&g_guard_cv);
  mutexUnlock(&g_guard_lock);
}

/* The constructor threw: the next caller tries again. */
void b___cxa_guard_abort(volatile uint32_t *g) {
  mutexLock(&g_guard_lock);
  __atomic_store_n(g, 0, __ATOMIC_RELEASE);
  condvarWakeAll(&g_guard_cv);
  mutexUnlock(&g_guard_lock);
}

/* ------------------------------------------------------ pure virtual call */
/* A pure virtual method called: a destroyed object's (or a half-built one's)
 * vtable. On Android the process aborts with this message. */
void b___cxa_pure_virtual(void) {
  void *from = __builtin_return_address(0);
  debugPrintf("[abi] pure virtual method called (from %p)\n", from);
  log_flush_ring();
  fatal_error("The game called a pure virtual method (from %p): an internal error of the engine.", from);
}
