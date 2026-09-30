/* bionic_pthread.c -- threads, mutexes, condvars, semaphores, keys and once for
 * the 32-bit bionic ABI, on libnx32.
 *
 * EVERY BIONIC SYNC OBJECT IS ONE 32-BIT WORD on armeabi-v7a. The game's static
 * mutexes sit in its .bss holding bionic's initialisers, so the word is decoded:
 *
 *   0       PTHREAD_MUTEX_INITIALIZER            (lazily becomes a pointer)
 *   0x4000  PTHREAD_RECURSIVE_MUTEX_INITIALIZER  (")
 *   0x8000  PTHREAD_ERRORCHECK_MUTEX_INITIALIZER (")
 *   other   a pointer to our BMutex (magic-checked)
 *
 * Heap addresses cannot be told apart by range (a 32-bit process's heap region
 * is placed by the kernel), so "pointer" means "not one of the three
 * initialisers". Lazy creation is a CAS on the word; the loser frees its copy.
 *
 * CONDVARS are the word itself, used as bionic uses it: a signal counter
 * (PTHREAD_COND_INITIALIZER is 0), on the kernel address arbiter -- see
 * "condvars" below for why not libnx's CondVar. A port that signals an
 * engine's condvar itself (a vsync pump) goes through these same functions
 * -- one implementation per word, never two.
 *
 * THREADS are libnx threads. Each gets a BThread record (pthread_t is its
 * address) holding the kernel handle, a small integer tid (gettid/tkill), its
 * stack bounds (pthread_getattr_np -- Mono's Boehm GC scans stacks from there),
 * key values and the cleanup stack. Threads we did not create (the main thread,
 * libnx helpers) are registered lazily the first time they ask. A port may tag
 * each new thread with its creator's context (port_thread_tag_*).
 *
 * Settings: RT_LOG_THREAD_CREATE, RT_COOP_SAMPLE (below). MIT.
 */
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "dcr_sched.h"
#include "rt_settings.h"
#include "util.h"

/* Log each new thread: its size, its start function and the code addresses
 * on its creator's stack (the first 48). a8r: 1; others 0. */
#ifndef RT_LOG_THREAD_CREATE
#define RT_LOG_THREAD_CREATE 0
#endif
/* Cooperative sampling for the watchdog (bionic_pthread.h): one load per
 * mutex lock and getspecific. Every port: 1. */
#ifndef RT_COOP_SAMPLE
#define RT_COOP_SAMPLE 1
#endif

const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.c */
int dcr_is_code_addr(uint32_t a);
size_t dcr_readable(uint32_t p, size_t want);

#define B_KEYS_MAX 256
#define BMUTEX_MAGIC 0x4d555458u /* 'MUTX' */
#define BSEM_MAGIC 0x53454d41u   /* 'SEMA' */
#define DEFAULT_STACK (1024 * 1024)
#define MIN_STACK (128 * 1024)

/* =============================== registry ================================= */
static Mutex g_reg_lock;
static BThread *g_threads;
static int g_next_tid = 1000;
static __thread BThread *t_self;

static struct {
  int used;
  void (*dtor)(void *);
} g_keys[B_KEYS_MAX];
static Mutex g_keys_lock;

/* tid -> thread, readable WITHOUT a lock. The GC bridge looks threads up in
 * the middle of a stop-the-world, when any other thread may already be paused
 * -- possibly while holding g_reg_lock. A slot is written before the thread
 * can be signalled and cleared before its record can be freed. */
#define TID_SLOTS 4096
static BThread *volatile g_by_tid[TID_SLOTS];

static void reg_add(BThread *t) {
  mutexLock(&g_reg_lock);
  t->tid = g_next_tid++;
  t->next = g_threads;
  g_threads = t;
  __atomic_store_n(&g_by_tid[(unsigned)t->tid % TID_SLOTS], t, __ATOMIC_RELEASE);
  mutexUnlock(&g_reg_lock);
}

static void reg_remove(BThread *t) {
  mutexLock(&g_reg_lock);
  BThread *expected = t;
  __atomic_compare_exchange_n(&g_by_tid[(unsigned)t->tid % TID_SLOTS], &expected, NULL, 0,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  for (BThread **pp = &g_threads; *pp; pp = &(*pp)->next)
    if (*pp == t) {
      *pp = t->next;
      break;
    }
  mutexUnlock(&g_reg_lock);
}

BThread *b_thread_by_tid(int tid) {
  BThread *f = __atomic_load_n(&g_by_tid[(unsigned)tid % TID_SLOTS], __ATOMIC_ACQUIRE);
  if (f && f->tid == tid)
    return f;
  mutexLock(&g_reg_lock);
  BThread *t = g_threads;
  while (t && t->tid != tid)
    t = t->next;
  mutexUnlock(&g_reg_lock);
  return t;
}

void b_thread_foreach(void (*fn)(BThread *, void *), void *arg) {
  mutexLock(&g_reg_lock);
  for (BThread *t = g_threads; t; t = t->next)
    fn(t, arg);
  mutexUnlock(&g_reg_lock);
}

/* Pausing a thread (svcSetThreadActivity) is shared by the GC bridge and the
 * watchdog; this lock keeps either from resuming a thread the other paused. */
static Mutex g_pause_lock;
void b_pause_lock(void) { mutexLock(&g_pause_lock); }
void b_pause_unlock(void) { mutexUnlock(&g_pause_lock); }

/* For the watchdog's emergency report (a lock that never comes free): the
 * raw lock words (0 = free; else the owner's handle, bit 30 = waiters), and a
 * walk of the threads that takes no lock (the tid slots, as the GC does). */
void b_lock_words(uint32_t *reg, uint32_t *pause) {
  *reg = __atomic_load_n((uint32_t *)&g_reg_lock, __ATOMIC_RELAXED);
  *pause = __atomic_load_n((uint32_t *)&g_pause_lock, __ATOMIC_RELAXED);
}
void b_thread_foreach_nolock(void (*fn)(BThread *, void *), void *arg) {
  for (unsigned i = 0; i < TID_SLOTS; i++) {
    BThread *t = __atomic_load_n(&g_by_tid[i], __ATOMIC_ACQUIRE);
    if (t && t->magic == BTHREAD_MAGIC)
      fn(t, arg);
  }
}

BThread *b_thread_self(void) {
  if (t_self)
    return t_self;
  BThread *t = calloc(1, sizeof *t);
  if (!t)
    return NULL;
  t->magic = BTHREAD_MAGIC;
  t->handle = threadGetCurHandle();
  Thread *lt = threadGetSelf();
  if (lt && lt->stack_mirror) {
    t->stack_base = lt->stack_mirror;
    t->stack_size = lt->stack_sz;
  } else {
    /* The main thread (and any thread libnx did not set up): the stack is the
     * memory region containing our own frame. */
    MemoryInfo mi;
    u32 pi;
    volatile int here;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (uintptr_t)&here))) {
      t->stack_base = (void *)(uintptr_t)mi.addr;
      t->stack_size = (size_t)mi.size;
    }
  }
  t->detached = 1; /* never joined */
  reg_add(t);
  t_self = t;
  return t;
}

int b_gettid(void) {
  BThread *t = b_thread_self();
  return t ? t->tid : 1;
}

/* ================================ create ================================== */
static void run_key_dtors(BThread *t) {
  for (int round = 0; round < 4; round++) {
    int any = 0;
    for (int k = 0; k < B_KEYS_MAX; k++) {
      void *v = t->keys[k];
      if (v && g_keys[k].used && g_keys[k].dtor) {
        t->keys[k] = NULL;
        g_keys[k].dtor(v);
        any = 1;
      }
    }
    if (!any)
      break;
  }
}

/* Detached threads cannot free their own stacks; they are reaped here, from
 * other threads, once their handle has signalled. */
static BThread *g_zombies;
static Mutex g_zombie_lock;

static void reap_zombies(void) {
  mutexLock(&g_zombie_lock);
  for (BThread **pp = &g_zombies; *pp;) {
    BThread *z = *pp;
    if (R_SUCCEEDED(waitSingle(waiterForThread(&z->thr), 0))) {
      *pp = z->next_zombie;
      threadClose(&z->thr);
      free(z);
    } else {
      pp = &z->next_zombie;
    }
  }
  mutexUnlock(&g_zombie_lock);
}

static void thread_finish(BThread *t) {
  run_key_dtors(t);
  t->finished = 1;
  reg_remove(t);
  if (t->detached) {
    mutexLock(&g_zombie_lock);
    t->next_zombie = g_zombies;
    g_zombies = t;
    mutexUnlock(&g_zombie_lock);
  }
}

/* Thread tags (bionic_pthread.h); the defaults tag nothing. */
__attribute__((weak)) uintptr_t port_thread_tag_for_new(void) { return 0; }
__attribute__((weak)) void port_thread_tag_enter(uintptr_t tag) {}

static void thread_entry(void *arg) {
  BThread *t = arg;
  t_self = t;
  port_thread_tag_enter(t->port_tag); /* a creator's threads are its own */
  Thread *lt = threadGetSelf();
  if (lt) {
    t->stack_base = lt->stack_mirror;
    t->stack_size = lt->stack_sz;
  }
  t->ret = t->start(t->arg);
  thread_finish(t);
}

#if RT_LOG_THREAD_CREATE
/* A new thread's creator is the useful part of the log, and ARM code keeps
 * no frame chain: the code addresses on the creating stack stand in. */
static void log_new_thread(size_t ss, void *(*start)(void *), void *from, void *frame) {
  static int logged;
  if (logged++ >= 48)
    return;
  char a[160], b[160];
  debugPrintf("[pthread] new thread %u KB: %s (from %s)\n", (unsigned)(ss >> 10),
              dcr_addr_name((uint32_t)(uintptr_t)start, a, sizeof a),
              dcr_addr_name((uint32_t)(uintptr_t)from, b, sizeof b));
  uint32_t sp = (uint32_t)(uintptr_t)frame & ~3u;
  size_t n = dcr_readable(sp, 0x600);
  int found = 0;
  for (uint32_t p = sp; p + 4 <= sp + n && found < 6; p += 4) {
    uint32_t v = *(const volatile uint32_t *)(uintptr_t)p;
    if (v > 0x01000000u && dcr_is_code_addr(v & ~1u)) {
      debugPrintf("[pthread]    caller? %s\n", dcr_addr_name(v, a, sizeof a));
      found++;
    }
  }
}
#endif

/* A guest thread at priority `prio`, starting on core `core` (a port's
 * render thread, say); b_pthread_create's are DCR_GUEST_PRIO, cores in turn
 * (dcr_sched.c). */
int b_pthread_create_on(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *),
                        void *arg, int prio, int core) {
  reap_zombies();
  BThread *t = calloc(1, sizeof *t);
  if (!t)
    return L_EAGAIN;
  t->magic = BTHREAD_MAGIC;
  t->start = start;
  t->arg = arg;
  t->owned = 1;
  t->port_tag = port_thread_tag_for_new();
  t->detached = attr && (attr->flags & B_PTHREAD_ATTR_FLAG_DETACHED);

  size_t ss = attr && attr->stack_size ? attr->stack_size : DEFAULT_STACK;
  if (ss < MIN_STACK)
    ss = MIN_STACK;
  ss = (ss + 0xFFF) & ~0xFFFu;

  Result rc = threadCreate(&t->thr, thread_entry, t, NULL, ss, prio, core);
  if (R_FAILED(rc)) {
    debugPrintf("[pthread] threadCreate(%u KB) failed 0x%x\n", (unsigned)(ss >> 10), rc);
    free(t);
    return L_EAGAIN;
  }
  t->handle = t->thr.handle;
#if RT_LOG_THREAD_CREATE
  log_new_thread(ss, start, __builtin_return_address(0), __builtin_frame_address(0));
#endif
  reg_add(t);
  dcr_sched_guest(t->handle);
  if (out)
    *out = (b_pthread_t)t;
  rc = threadStart(&t->thr);
  if (R_FAILED(rc)) {
    debugPrintf("[pthread] threadStart failed 0x%x\n", rc);
    reg_remove(t);
    threadClose(&t->thr);
    free(t);
    return L_EAGAIN;
  }
  return 0;
}

int b_pthread_create(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *),
                     void *arg) {
  /* Guest priority and cores 0-2: see dcr_sched.c. */
  return b_pthread_create_on(out, attr, start, arg, DCR_GUEST_PRIO, dcr_sched_next_core());
}

void NORETURN b_pthread_exit(void *ret) {
  BThread *t = b_thread_self();
  if (t && t->owned) {
    t->ret = ret;
    thread_finish(t);
    threadExit();
  }
  debugPrintf("[pthread] pthread_exit on a thread we did not create -- exiting it\n");
  svcExitThread();
  __builtin_unreachable();
}

int b_pthread_join(b_pthread_t th, void **ret) {
  BThread *t = (BThread *)th;
  if (!t || t->magic != BTHREAD_MAGIC || !t->owned)
    return L_ESRCH;
  if (t->detached)
    return L_EINVAL;
  if (t == t_self)
    return L_EDEADLK;
  threadWaitForExit(&t->thr);
  threadClose(&t->thr);
  if (ret)
    *ret = t->ret;
  t->magic = 0;
  free(t);
  return 0;
}

int b_pthread_detach(b_pthread_t th) {
  BThread *t = (BThread *)th;
  if (!t || t->magic != BTHREAD_MAGIC)
    return L_ESRCH;
  if (!t->owned) {
    t->detached = 1;
    return 0;
  }
  mutexLock(&g_zombie_lock);
  t->detached = 1;
  if (t->finished) { /* it already ended as joinable: make it a zombie now */
    t->next_zombie = g_zombies;
    g_zombies = t;
  }
  mutexUnlock(&g_zombie_lock);
  return 0;
}

b_pthread_t b_pthread_self(void) { return (b_pthread_t)b_thread_self(); }
int b_pthread_equal(b_pthread_t a, b_pthread_t b) { return a == b; }

/* ============================== attributes ================================ */
int b_pthread_attr_init(b_pthread_attr_t *a) {
  memset(a, 0, sizeof *a);
  a->stack_size = DEFAULT_STACK;
  a->guard_size = 0x1000;
  return 0;
}
int b_pthread_attr_destroy(b_pthread_attr_t *a) { return 0; }
int b_pthread_attr_setstacksize(b_pthread_attr_t *a, size_t s) {
  if (s < 0x4000)
    return L_EINVAL;
  a->stack_size = s;
  return 0;
}
int b_pthread_attr_setdetachstate(b_pthread_attr_t *a, int st) {
  if (st == B_PTHREAD_CREATE_DETACHED)
    a->flags |= B_PTHREAD_ATTR_FLAG_DETACHED;
  else
    a->flags &= ~B_PTHREAD_ATTR_FLAG_DETACHED;
  return 0;
}
int b_pthread_attr_getdetachstate(const b_pthread_attr_t *a, int *st) {
  *st = (a->flags & B_PTHREAD_ATTR_FLAG_DETACHED) ? B_PTHREAD_CREATE_DETACHED
                                                  : B_PTHREAD_CREATE_JOINABLE;
  return 0;
}
int b_pthread_attr_setschedparam(b_pthread_attr_t *a, const struct b_sched_param *p) {
  if (p)
    a->sched_priority = p->sched_priority;
  return 0;
}
int b_pthread_attr_getstacksize(const b_pthread_attr_t *a, size_t *s) {
  if (s)
    *s = a && a->stack_size ? a->stack_size : 1024 * 1024;
  return 0;
}
int b_pthread_attr_getschedparam(const b_pthread_attr_t *a, struct b_sched_param *p) {
  if (p)
    p->sched_priority = a ? a->sched_priority : 0;
  return 0;
}
int b_pthread_attr_getschedpolicy(const b_pthread_attr_t *a, int *policy) {
  if (policy)
    *policy = a ? a->sched_policy : 0;
  return 0;
}
int b_pthread_attr_getstack(const b_pthread_attr_t *a, void **base, size_t *size) {
  if (base)
    *base = a->stack_base;
  if (size)
    *size = a->stack_size;
  return 0;
}

/* Stack bounds of a live thread. stack_base is the LOWEST address (bionic);
 * Boehm adds the size to find the cold end it scans towards. */
int b_pthread_getattr_np(b_pthread_t th, b_pthread_attr_t *a) {
  BThread *t = (BThread *)th;
  if (!t || t->magic != BTHREAD_MAGIC)
    return L_ESRCH;
  b_pthread_attr_init(a);
  a->stack_base = t->stack_base;
  a->stack_size = t->stack_size;
  if (t->detached)
    a->flags |= B_PTHREAD_ATTR_FLAG_DETACHED;
  if (!t->stack_base)
    debugPrintf("[pthread] getattr_np: no stack bounds for tid %d\n", t->tid);
  return 0;
}

/* ================================ mutexes ================================= */
static BMutex *mx_get(b_pthread_mutex_t *pm) {
  int32_t v = __atomic_load_n(&pm->value, __ATOMIC_ACQUIRE);
  if (v != B_MUTEX_INIT_NORMAL && v != B_MUTEX_INIT_RECURSIVE && v != B_MUTEX_INIT_ERRORCHECK) {
    BMutex *m = (BMutex *)(uintptr_t)v;
    if (m->magic == BMUTEX_MAGIC)
      return m;
    static int warned;
    if (!warned++)
      debugPrintf("[pthread] mutex %p holds 0x%08lx, not a mutex -- reinitialising\n",
                  (void *)pm, (unsigned long)v);
    v = 0;
    __atomic_store_n(&pm->value, 0, __ATOMIC_RELEASE);
  }
  BMutex *m = calloc(1, sizeof *m);
  if (!m)
    return NULL;
  m->magic = BMUTEX_MAGIC;
  m->type = v == B_MUTEX_INIT_RECURSIVE ? B_PTHREAD_MUTEX_RECURSIVE
          : v == B_MUTEX_INIT_ERRORCHECK ? B_PTHREAD_MUTEX_ERRORCHECK
          : B_PTHREAD_MUTEX_NORMAL;
  int32_t expect = v;
  if (__atomic_compare_exchange_n(&pm->value, &expect, (int32_t)(uintptr_t)m, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return m;
  free(m);
  return (BMutex *)(uintptr_t)__atomic_load_n(&pm->value, __ATOMIC_ACQUIRE);
}

int b_pthread_mutexattr_init(b_pthread_mutexattr_t *a) { *a = B_PTHREAD_MUTEX_NORMAL; return 0; }
int b_pthread_mutexattr_destroy(b_pthread_mutexattr_t *a) { return 0; }
int b_pthread_mutexattr_settype(b_pthread_mutexattr_t *a, int type) {
  if (type < 0 || type > 2)
    return L_EINVAL;
  *a = type;
  return 0;
}

int b_pthread_mutex_init(b_pthread_mutex_t *pm, const b_pthread_mutexattr_t *a) {
  BMutex *m = calloc(1, sizeof *m);
  if (!m)
    return L_ENOMEM;
  m->magic = BMUTEX_MAGIC;
  m->type = a ? (int)(*a & 3) : B_PTHREAD_MUTEX_NORMAL;
  __atomic_store_n(&pm->value, (int32_t)(uintptr_t)m, __ATOMIC_RELEASE);
  return 0;
}

int b_pthread_mutex_destroy(b_pthread_mutex_t *pm) {
  int32_t v = __atomic_exchange_n(&pm->value, 0, __ATOMIC_ACQ_REL);
  if (v != B_MUTEX_INIT_NORMAL && v != B_MUTEX_INIT_RECURSIVE && v != B_MUTEX_INIT_ERRORCHECK) {
    BMutex *m = (BMutex *)(uintptr_t)v;
    if (m->magic == BMUTEX_MAGIC) {
      m->magic = 0;
      free(m);
    }
  }
  return 0;
}

static inline uint32_t self_tag(void) { return (uint32_t)(uintptr_t)b_thread_self(); }

/* Cooperative sample (the watchdog, under emulation): a hung thread that
 * keeps calling these shims records where it is -- its caller and its stack
 * pointer -- when the watchdog asks. An emulator cannot pause a spinning
 * thread for its registers. */
volatile Handle g_sample_want;       /* the thread to sample, or 0 */
volatile uint32_t g_sample_sp, g_sample_lr, g_sample_n;
#if RT_COOP_SAMPLE
#define COOP_SAMPLE()                                                                       \
  do {                                                                                      \
    if (__builtin_expect(g_sample_want != 0, 0) && g_sample_want == threadGetCurHandle()) { \
      g_sample_sp = (uint32_t)(uintptr_t)__builtin_frame_address(0);                      \
      g_sample_lr = (uint32_t)(uintptr_t)__builtin_return_address(0);                     \
      g_sample_n++;                                                                         \
      g_sample_want = 0;                                                                    \
    }                                                                                       \
  } while (0)
#else
#define COOP_SAMPLE() do {} while (0)
#endif

int b_pthread_mutex_lock(b_pthread_mutex_t *pm) {
  COOP_SAMPLE();
  BMutex *m = mx_get(pm);
  if (!m)
    return L_EINVAL;
  if (m->type != B_PTHREAD_MUTEX_NORMAL) {
    uint32_t me = self_tag();
    if (m->owner == me) {
      if (m->type == B_PTHREAD_MUTEX_ERRORCHECK)
        return L_EDEADLK;
      m->count++;
      return 0;
    }
    mutexLock(&m->m);
    m->owner = me;
    m->count = 1;
    return 0;
  }
  mutexLock(&m->m);
  return 0;
}

int b_pthread_mutex_trylock(b_pthread_mutex_t *pm) {
  BMutex *m = mx_get(pm);
  if (!m)
    return L_EINVAL;
  if (m->type != B_PTHREAD_MUTEX_NORMAL) {
    uint32_t me = self_tag();
    if (m->owner == me) {
      if (m->type == B_PTHREAD_MUTEX_ERRORCHECK)
        return L_EBUSY;
      m->count++;
      return 0;
    }
    if (!mutexTryLock(&m->m))
      return L_EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
  }
  return mutexTryLock(&m->m) ? 0 : L_EBUSY;
}

int b_pthread_mutex_unlock(b_pthread_mutex_t *pm) {
  BMutex *m = mx_get(pm);
  if (!m)
    return L_EINVAL;
  if (m->type != B_PTHREAD_MUTEX_NORMAL) {
    if (m->owner != self_tag())
      return L_EPERM;
    if (--m->count > 0)
      return 0;
    m->owner = 0;
  }
  mutexUnlock(&m->m);
  return 0;
}

/* ================================ condvars ================================
 * bionic keeps a sequence counter in the condvar word: signal/broadcast bump
 * it and futex-wake; a wait snapshots it BEFORE releasing the mutex and sleeps
 * only while it is unchanged. A signal that lands between the caller's
 * predicate check and the sleep is therefore not lost.
 *
 * libnx's CondVar (the kernel's process-wide key) has no such memory: a
 * signal with no registered waiter does nothing. Unity depends on the bionic
 * behaviour -- Thread::Run (libunity+0x2e7bf0) waits for a new thread with
 * `while (!started) pthread_cond_wait()`, and the new thread (+0x2e7d30) sets
 * `started` and signals WITHOUT taking the mutex. A new thread starts at once
 * on another core (dcr_sched.c), so that signal can land between the
 * creator's check and its kernel wait -- and on the process-wide key it would
 * be lost, and the creator would sleep for ever.
 *
 * So the word is a counter here too, on svcWaitForAddress(WaitIfEqual) /
 * svcSignalToAddress -- the kernel's futex. The snapshot is the first thing a
 * wait does, which leaves exactly bionic's window (the caller's own predicate
 * check to our snapshot); as a backstop for that, each kernel wait lasts at
 * most COND_SLICE_NS and then returns as a spurious wakeup, which POSIX
 * allows and every predicate loop absorbs. */
#define COND_SLICE_NS 250000000ll

/* svcWaitForAddress (0x34). AArch32 register layout, per Mesosphere's
 * codegen (64-bit arguments take the next even pair, later halves the lowest
 * free registers -- as libnx32's other stubs confirm):
 *   Atmosphere >= 1.8.0 (value is int64 since 19.0.0):
 *       r0 = address, r1 = type, r2:r3 = value, r4:r5 = timeout
 *   the older int32 value (Ryujinx 1.1.1098 still declares it so):
 *       r0 = address, r1 = type, r2 = value,    r3:r4 = timeout
 * libnx32 has no stub. The self-test decides: on hardware (Atmosphere for
 * 21.x, 2026-09-23) as on Ryujinx, a 30 ms wait took 30 ms only with the
 * int32 layout. */
/* So the int32 layout is the default, on hardware as under emulation;
 * dcr_pthread_selftest() verifies it at start-up and switches to the int64
 * layout if timed waits come out wrong. */
static int g_arb_old_abi = -1; /* -1: not chosen yet */

static Result arb_wait_if_equal(volatile uint32_t *addr, uint32_t value, s64 timeout_ns) {
  if (g_arb_old_abi < 0)
    g_arb_old_abi = 1;
  const int old_abi = g_arb_old_abi;
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)addr;
  register uint32_t r1 __asm__("r1") = ArbitrationType_WaitIfEqual;
  register uint32_t r2 __asm__("r2") = value;
  register uint32_t r3 __asm__("r3") = old_abi ? (uint32_t)timeout_ns : 0;
  register uint32_t r4 __asm__("r4") = old_abi ? (uint32_t)((uint64_t)timeout_ns >> 32) : (uint32_t)timeout_ns;
  register uint32_t r5 __asm__("r5") = old_abi ? 0 : (uint32_t)((uint64_t)timeout_ns >> 32);
  __asm__ volatile("svc 0x34"
                   : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5)
                   :
                   : "r12", "lr", "memory");
  return r0;
}

/* svcSignalToAddress (0x35): r0 = address, r1 = type, r2 = value, r3 = count
 * (<= 0: every waiter). */
static void arb_signal(volatile uint32_t *addr, int32_t count) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)addr;
  register uint32_t r1 __asm__("r1") = SignalType_Signal;
  register uint32_t r2 __asm__("r2") = 0;
  register uint32_t r3 __asm__("r3") = (uint32_t)count;
  __asm__ volatile("svc 0x35" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r12", "lr", "memory");
}

/* Linux futex(FUTEX_WAIT/FUTEX_WAKE) on the same arbiter, for syscall()
 * (libc++'s std::atomic::wait/notify) and the fd-activity word (bionic_io.c).
 * A wait lasts at most one
 * COND_SLICE_NS slice -- a spurious wakeup, which every futex caller loops on.
 * Returns 0, or a negative Linux errno. */
int dcr_futex_wait(volatile uint32_t *addr, uint32_t val, s64 timeout_ns) {
  if (timeout_ns < 0 || timeout_ns > COND_SLICE_NS)
    timeout_ns = COND_SLICE_NS;
  Result rc = arb_wait_if_equal(addr, val, timeout_ns);
  if (R_SUCCEEDED(rc))
    return 0;
  if (R_VALUE(rc) == KERNELRESULT(InvalidState))
    return -L_EAGAIN; /* *addr != val */
  if (R_VALUE(rc) == KERNELRESULT(TimedOut))
    return -L_ETIMEDOUT;
  return 0;
}

int dcr_futex_wake(volatile uint32_t *addr, int count) {
  arb_signal(addr, count <= 0 || count == 0x7fffffff ? -1 : count);
  return 0; /* the woken count is not reported by the kernel; callers ignore it */
}

int b_pthread_cond_init(b_pthread_cond_t *c, const void *attr) {
  __atomic_store_n(&c->value, 0, __ATOMIC_RELEASE);
  return 0;
}
int b_pthread_cond_destroy(b_pthread_cond_t *c) { return 0; }
int b_pthread_cond_signal(b_pthread_cond_t *c) {
  __atomic_add_fetch(&c->value, 1, __ATOMIC_SEQ_CST);
  arb_signal((volatile uint32_t *)&c->value, 1);
  return 0;
}
int b_pthread_cond_broadcast(b_pthread_cond_t *c) {
  __atomic_add_fetch(&c->value, 1, __ATOMIC_SEQ_CST);
  arb_signal((volatile uint32_t *)&c->value, -1);
  return 0;
}

/* Wait with the (possibly recursive) mutex released completely. */
static int cond_wait_ns(b_pthread_cond_t *c, b_pthread_mutex_t *pm, u64 timeout_ns) {
  const uint32_t seq = __atomic_load_n(&c->value, __ATOMIC_SEQ_CST); /* first: see above */
  BMutex *m = mx_get(pm);
  if (!m)
    return L_EINVAL;
  uint32_t owner = m->owner;
  int count = m->count;
  if (m->type != B_PTHREAD_MUTEX_NORMAL) {
    m->owner = 0;
    m->count = 0;
  }
  mutexUnlock(&m->m);
  const s64 slice = timeout_ns < (u64)COND_SLICE_NS ? (s64)timeout_ns : COND_SLICE_NS;
  Result rc = arb_wait_if_equal((volatile uint32_t *)&c->value, seq, slice);
  mutexLock(&m->m);
  if (m->type != B_PTHREAD_MUTEX_NORMAL) {
    m->owner = owner;
    m->count = count;
  }
  /* Timed out only if the caller's whole timeout fit in this one wait;
   * otherwise a slice ended: a spurious wakeup, and the caller re-checks. */
  if (R_VALUE(rc) == KERNELRESULT(TimedOut) && timeout_ns <= (u64)COND_SLICE_NS)
    return L_ETIMEDOUT;
  return 0;
}

int b_pthread_cond_wait(b_pthread_cond_t *c, b_pthread_mutex_t *m) {
  return cond_wait_ns(c, m, UINT64_MAX);
}

u64 b_abs_realtime_to_timeout_ns(const struct b_timespec *abs); /* below */

int b_pthread_cond_timedwait(b_pthread_cond_t *c, b_pthread_mutex_t *m,
                             const struct b_timespec *abstime) {
  return cond_wait_ns(c, m, b_abs_realtime_to_timeout_ns(abstime));
}

int b_pthread_cond_timedwait_relative_np(b_pthread_cond_t *c, b_pthread_mutex_t *m,
                                         const struct b_timespec *rel) {
  u64 ns = rel ? (u64)rel->tv_sec * 1000000000ull + (u64)rel->tv_nsec : 0;
  return cond_wait_ns(c, m, ns);
}

int b_clock_gettime(int clk, struct b_timespec *ts); /* bionic_time.c */

/* Android before 4.3's pthread_cond_timedwait_monotonic(_np): an ABSOLUTE
 * time on CLOCK_MONOTONIC. */
int b_pthread_cond_timedwait_monotonic(b_pthread_cond_t *c, b_pthread_mutex_t *m,
                                       const struct b_timespec *abstime) {
  u64 ns = 0;
  if (abstime) {
    struct b_timespec now;
    b_clock_gettime(L_CLOCK_MONOTONIC, &now);
    s64 d = ((s64)abstime->tv_sec - now.tv_sec) * 1000000000ll + ((s64)abstime->tv_nsec - now.tv_nsec);
    ns = d > 0 ? (u64)d : 0;
  }
  return cond_wait_ns(c, m, ns);
}
int b_pthread_cond_timedwait_monotonic_np(b_pthread_cond_t *c, b_pthread_mutex_t *m,
                                          const struct b_timespec *abstime) {
  return b_pthread_cond_timedwait_monotonic(c, m, abstime);
}

/* pthread_condattr_t is a long; the clock and pshared bits change nothing
 * here (a timed wait is relative once converted). */
int b_pthread_condattr_init(int32_t *a) {
  if (a)
    *a = 0;
  return 0;
}
int b_pthread_condattr_destroy(int32_t *a) { return 0; }

u64 b_abs_realtime_to_timeout_ns(const struct b_timespec *abs) {
  if (!abs)
    return 0;
  struct b_timespec now;
  b_clock_gettime(L_CLOCK_REALTIME, &now);
  s64 d = ((s64)abs->tv_sec - now.tv_sec) * 1000000000ll + ((s64)abs->tv_nsec - now.tv_nsec);
  return d > 0 ? (u64)d : 0;
}

/* ================================== keys =================================== */
int b_pthread_key_create(b_pthread_key_t *key, void (*dtor)(void *)) {
  mutexLock(&g_keys_lock);
  for (int k = 0; k < B_KEYS_MAX; k++)
    if (!g_keys[k].used) {
      g_keys[k].used = 1;
      g_keys[k].dtor = dtor;
      mutexUnlock(&g_keys_lock);
      *key = k;
      return 0;
    }
  mutexUnlock(&g_keys_lock);
  debugPrintf("[pthread] out of pthread keys\n");
  return L_EAGAIN;
}

int b_pthread_key_delete(b_pthread_key_t key) {
  if (key < 0 || key >= B_KEYS_MAX)
    return L_EINVAL;
  mutexLock(&g_keys_lock);
  g_keys[key].used = 0;
  g_keys[key].dtor = NULL;
  mutexUnlock(&g_keys_lock);
  return 0;
}

void *b_pthread_getspecific(b_pthread_key_t key) {
  COOP_SAMPLE();
  if ((unsigned)key >= B_KEYS_MAX)
    return NULL;
  BThread *t = t_self ? t_self : b_thread_self();
  return t ? t->keys[key] : NULL;
}

int b_pthread_setspecific(b_pthread_key_t key, const void *v) {
  if ((unsigned)key >= B_KEYS_MAX)
    return L_EINVAL;
  BThread *t = t_self ? t_self : b_thread_self();
  if (!t)
    return L_ENOMEM;
  t->keys[key] = (void *)v;
  return 0;
}

/* ================================== once =================================== */
int b_pthread_once(b_pthread_once_t *once, void (*init)(void)) {
  if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2)
    return 0;
  int expect = 0;
  if (__atomic_compare_exchange_n(once, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    init();
    __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2)
    svcSleepThread(100000);
  return 0;
}

/* ============================ cleanup handlers ============================= */
typedef struct b_cleanup {
  struct b_cleanup *prev;
  void (*routine)(void *);
  void *arg;
} b_cleanup;

void b___pthread_cleanup_push(b_cleanup *c, void (*routine)(void *), void *arg) {
  BThread *t = b_thread_self();
  c->prev = t ? t->cleanup_top : NULL;
  c->routine = routine;
  c->arg = arg;
  if (t)
    t->cleanup_top = c;
}

void b___pthread_cleanup_pop(b_cleanup *c, int execute) {
  BThread *t = b_thread_self();
  if (t)
    t->cleanup_top = c->prev;
  if (execute && c->routine)
    c->routine(c->arg);
}

/* =============================== semaphores ================================ */
typedef struct {
  uint32_t magic;
  Mutex m;
  CondVar cv;
  int count;
} BSem;

static BSem *sem_get(b_sem_t *s) {
  BSem *b = (BSem *)(uintptr_t)__atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
  if (b && b->magic == BSEM_MAGIC)
    return b;
  /* Used before sem_init (or zero-filled): make one with a zero count. */
  BSem *n = calloc(1, sizeof *n);
  if (!n)
    return NULL;
  n->magic = BSEM_MAGIC;
  uint32_t expect = (uint32_t)(uintptr_t)b;
  if (__atomic_compare_exchange_n(&s->count, &expect, (uint32_t)(uintptr_t)n, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return n;
  free(n);
  return (BSem *)(uintptr_t)__atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
}

int b_sem_init(b_sem_t *s, int pshared, unsigned int value) {
  BSem *b = calloc(1, sizeof *b);
  if (!b) {
    b_set_errno(L_ENOMEM);
    return -1;
  }
  b->magic = BSEM_MAGIC;
  b->count = (int)value;
  __atomic_store_n(&s->count, (uint32_t)(uintptr_t)b, __ATOMIC_RELEASE);
  return 0;
}

int b_sem_destroy(b_sem_t *s) {
  BSem *b = (BSem *)(uintptr_t)__atomic_exchange_n(&s->count, 0, __ATOMIC_ACQ_REL);
  if (b && b->magic == BSEM_MAGIC) {
    b->magic = 0;
    free(b);
  }
  return 0;
}

int b_sem_post(b_sem_t *s) {
  BSem *b = sem_get(s);
  if (!b) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  mutexLock(&b->m);
  b->count++;
  condvarWakeOne(&b->cv);
  mutexUnlock(&b->m);
  return 0;
}

static int sem_wait_ns(b_sem_t *s, u64 timeout_ns) {
  BSem *b = sem_get(s);
  if (!b) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  u64 deadline = timeout_ns == UINT64_MAX ? UINT64_MAX : armGetSystemTick() + armNsToTicks(timeout_ns);
  mutexLock(&b->m);
  while (b->count <= 0) {
    if (timeout_ns == UINT64_MAX) {
      condvarWait(&b->cv, &b->m);
    } else {
      u64 now = armGetSystemTick();
      if (now >= deadline) {
        mutexUnlock(&b->m);
        b_set_errno(L_ETIMEDOUT);
        return -1;
      }
      condvarWaitTimeout(&b->cv, &b->m, armTicksToNs(deadline - now));
    }
  }
  b->count--;
  mutexUnlock(&b->m);
  return 0;
}

int b_sem_wait(b_sem_t *s) { return sem_wait_ns(s, UINT64_MAX); }
int b_sem_timedwait(b_sem_t *s, const struct b_timespec *abs) {
  return sem_wait_ns(s, b_abs_realtime_to_timeout_ns(abs));
}
int b_sem_trywait(b_sem_t *s) {
  BSem *b = sem_get(s);
  if (!b) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  mutexLock(&b->m);
  int ok = b->count > 0;
  if (ok)
    b->count--;
  mutexUnlock(&b->m);
  if (!ok)
    b_set_errno(L_EAGAIN);
  return ok ? 0 : -1;
}
int b_sem_getvalue(b_sem_t *s, int *v) {
  BSem *b = sem_get(s);
  if (!b || !v) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  *v = b->count;
  return 0;
}

int b_sched_yield(void) {
  svcSleepThread(0);
  return 0;
}

/* ========================= scheduling / naming ============================ */
/* Thread priorities are fixed at creation (DCR_GUEST_PRIO); the engine's requests
 * to raise or lower them are accepted and ignored. bionic reports SCHED_OTHER
 * with a 0..0 priority range, which is what Unity and Mono expect to see. */
int b_pthread_getschedparam(b_pthread_t th, int *policy, struct b_sched_param *p) {
  if (policy)
    *policy = 0; /* SCHED_OTHER */
  if (p)
    p->sched_priority = 0;
  return 0;
}
int b_pthread_setschedparam(b_pthread_t th, int policy, const struct b_sched_param *p) { return 0; }
int b_sched_get_priority_max(int policy) { return 0; }
int b_sched_get_priority_min(int policy) { return 0; }

int b_pthread_setname_np(b_pthread_t th, const char *name) {
  BThread *t = (BThread *)th;
  if (t && t->magic == BTHREAD_MAGIC && name)
    snprintf(t->name, sizeof t->name, "%s", name);
  return 0;
}

/* Every thread's CPU clock reads the thread-CPU clock id; bionic_time serves
 * that from the monotonic clock (Mono's thread pool uses it only for ratios). */
int b_pthread_getcpuclockid(b_pthread_t th, int *clock_id) {
  if (clock_id)
    *clock_id = L_CLOCK_THREAD_CPUTIME_ID;
  return 0;
}

int b_pthread_sigmask(int how, const uint32_t *set, uint32_t *old) {
  if (old)
    *old = 0;
  return 0;
}

/* ============================ condvar self-test =============================
 * Boot-time proof that the arbiter-based condvars work on this kernel (the
 * SVC register layout has no libnx32 stub to copy, and it changed in
 * Atmosphere 1.8.0). A wrong layout shows up as a timeout of zero or of
 * decades; either is detected, and the other layout is tried. A helper
 * thread rescues any wait that overruns by 600 ms, so a wrong layout can
 * never hang the boot. */
static b_pthread_mutex_t st_m;
static b_pthread_cond_t st_c;
static volatile int st_flag, st_phase;
static volatile u64 st_rescue_at; /* system tick; 0 = none */

static u64 ms_ticks(u64 ms) { return armNsToTicks(ms * 1000000ull); }

static void st_helper(void *arg) {
  u64 p2 = 0;
  while (st_phase != 3) {
    svcSleepThread(5000000ll);
    u64 now = armGetSystemTick(), r = st_rescue_at;
    if (r && now > r) {
      b_pthread_cond_broadcast(&st_c);
      st_rescue_at = now + ms_ticks(100);
    }
    /* Phase 2: Unity's pattern -- set the flag, then signal without the mutex. */
    if (st_phase == 2 && !st_flag) {
      if (!p2) {
        p2 = now;
      } else if (now - p2 >= ms_ticks(20)) {
        st_flag = 1;
        b_pthread_cond_signal(&st_c);
      }
    }
  }
}

static int st_timed_wait(u64 *ms_out, int *rc_out) {
  b_pthread_mutex_lock(&st_m);
  u64 t0 = armGetSystemTick();
  st_rescue_at = t0 + ms_ticks(600);
  int r = cond_wait_ns(&st_c, &st_m, 30000000ull);
  st_rescue_at = 0;
  u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;
  b_pthread_mutex_unlock(&st_m);
  *ms_out = ms;
  *rc_out = r;
  return r == L_ETIMEDOUT && ms >= 25 && ms < 200;
}

void dcr_pthread_selftest(void) {
  b_pthread_mutex_init(&st_m, NULL);
  b_pthread_cond_init(&st_c, NULL);
  st_phase = 1;
  Thread th;
  if (R_FAILED(threadCreate(&th, st_helper, NULL, NULL, 0x4000, 0x2C, -2)) ||
      R_FAILED(threadStart(&th))) {
    debugPrintf("[pthread] condvar self-test: could not start its helper thread\n");
    return;
  }
  if (g_arb_old_abi < 0)
    g_arb_old_abi = 1;

  /* 1: a 30 ms timed wait nobody signals; on failure, the other layout. */
  u64 ms;
  int rc;
  int ok1 = st_timed_wait(&ms, &rc);
  if (!ok1) {
    debugPrintf("[pthread] condvar self-test: timed wait wrong with the %s WaitForAddress "
                "layout (%llu ms, rc %d): trying the other\n",
                g_arb_old_abi ? "int32" : "int64", (unsigned long long)ms, rc);
    g_arb_old_abi ^= 1;
    ok1 = st_timed_wait(&ms, &rc);
  }

  /* 2: flag + signal from the helper without the mutex. */
  st_phase = 2;
  u64 t0 = armGetSystemTick();
  st_rescue_at = t0 + ms_ticks(600);
  b_pthread_mutex_lock(&st_m);
  while (!st_flag)
    b_pthread_cond_wait(&st_c, &st_m);
  b_pthread_mutex_unlock(&st_m);
  st_rescue_at = 0;
  u64 ms2 = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;
  st_phase = 3;
  threadWaitForExit(&th);
  threadClose(&th);
  debugPrintf("[pthread] condvar self-test (%s WaitForAddress layout): timed wait %s (%llu ms); "
              "signal without the mutex woke the waiter after %llu ms: %s\n",
              g_arb_old_abi ? "int32" : "int64", ok1 ? "OK" : "FAILED", (unsigned long long)ms,
              (unsigned long long)ms2, ms2 < 300 ? "OK" : "SLOW (woke on the backstop)");
}

/* ============================ reader/writer locks ===========================
 * Old bionic's pthread_rwlock_t is 40 bytes, a mutex, a condvar and counters;
 * PTHREAD_RWLOCK_INITIALIZER is all zeros, which is also a valid state here.
 * No writer preference: a thread that holds a read lock and takes it again
 * while a writer waits must not deadlock (one engine's resource locks do). */
typedef struct {
  b_pthread_mutex_t lock;
  b_pthread_cond_t cond;
  int32_t readers;   /* read locks held */
  int32_t writer;    /* thread tag of the writer, 0 = none */
  int32_t unused[6];
} BRwlock;
BIONIC_STATIC_ASSERT(sizeof(BRwlock) == 40, "pthread_rwlock_t");

int b_pthread_rwlock_init(BRwlock *rw, const void *attr) {
  if (!rw)
    return L_EINVAL;
  memset(rw, 0, sizeof *rw);
  return 0;
}

int b_pthread_rwlock_destroy(BRwlock *rw) { return 0; }

int b_pthread_rwlock_rdlock(BRwlock *rw) {
  b_pthread_mutex_lock(&rw->lock);
  while (rw->writer && rw->writer != (int32_t)self_tag())
    b_pthread_cond_wait(&rw->cond, &rw->lock);
  rw->readers++;
  b_pthread_mutex_unlock(&rw->lock);
  return 0;
}

int b_pthread_rwlock_wrlock(BRwlock *rw) {
  b_pthread_mutex_lock(&rw->lock);
  while (rw->writer || rw->readers)
    b_pthread_cond_wait(&rw->cond, &rw->lock);
  rw->writer = (int32_t)self_tag();
  b_pthread_mutex_unlock(&rw->lock);
  return 0;
}

int b_pthread_rwlock_tryrdlock(BRwlock *rw) {
  b_pthread_mutex_lock(&rw->lock);
  int ok = !rw->writer;
  if (ok)
    rw->readers++;
  b_pthread_mutex_unlock(&rw->lock);
  return ok ? 0 : L_EBUSY;
}

int b_pthread_rwlock_trywrlock(BRwlock *rw) {
  b_pthread_mutex_lock(&rw->lock);
  int ok = !rw->writer && !rw->readers;
  if (ok)
    rw->writer = (int32_t)self_tag();
  b_pthread_mutex_unlock(&rw->lock);
  return ok ? 0 : L_EBUSY;
}

int b_pthread_rwlock_unlock(BRwlock *rw) {
  b_pthread_mutex_lock(&rw->lock);
  if (rw->writer == (int32_t)self_tag())
    rw->writer = 0;
  else if (rw->readers > 0)
    rw->readers--;
  b_pthread_cond_broadcast(&rw->cond);
  b_pthread_mutex_unlock(&rw->lock);
  return 0;
}
