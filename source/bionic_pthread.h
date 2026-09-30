/* bionic_pthread.h -- thread records shared with the signal / GC bridge and
 * the watchdog, and the thread callbacks a port may define. MIT. */
#ifndef DCR_BIONIC_PTHREAD_H
#define DCR_BIONIC_PTHREAD_H
#include <switch.h>
#include "bionic.h"

#define BTHREAD_MAGIC 0x54485244u /* 'THRD' */

typedef struct BThread {
  uint32_t magic;
  Thread thr;               /* valid when owned */
  int owned;                /* created by b_pthread_create */
  Handle handle;            /* kernel thread handle: pause/resume/context */
  int tid;                  /* gettid() / tkill() identity */
  void *(*start)(void *);
  void *arg;
  void *ret;
  volatile int detached, finished;
  void *stack_base;         /* lowest address */
  size_t stack_size;
  void *keys[256];
  struct b_cleanup *cleanup_top;
  volatile int gc_paused;   /* suspended by a port's GC bridge (port_gc_signal) */
  uintptr_t port_tag;       /* port_thread_tag_for_new() of its creator */
  char name[16];            /* prctl(PR_SET_NAME) / pthread_setname_np (profiler, watchdog) */
  struct BThread *next, *next_zombie;
} BThread;

typedef struct {
  uint32_t magic;
  int type;
  Mutex m;
  uint32_t owner;
  int count;
} BMutex;

BThread *b_thread_self(void);
BThread *b_thread_by_tid(int tid);
void b_thread_foreach(void (*fn)(BThread *, void *), void *arg);
/* The watchdog's emergency report: a walk that takes no lock, and the raw
 * words of the registry and pause locks (0 free, else the owner's handle,
 * bit 30 = waiters). */
void b_thread_foreach_nolock(void (*fn)(BThread *, void *), void *arg);
void b_lock_words(uint32_t *reg, uint32_t *pause);
int b_gettid(void);
/* Held around every svcSetThreadActivity pause/resume (GC bridge, watchdog). */
void b_pause_lock(void);
void b_pause_unlock(void);

/* A guest thread at priority `prio` starting on core `core` (b_pthread_create
 * is this with DCR_GUEST_PRIO and the next core in turn). */
int b_pthread_create_on(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *),
                        void *arg, int prio, int core);

/* Linux futex(WAIT / WAKE) on the kernel's address arbiter. A wait lasts at
 * most 250 ms (a spurious wakeup); returns 0 or a negative Linux errno. */
int dcr_futex_wait(volatile uint32_t *addr, uint32_t val, s64 timeout_ns);
int dcr_futex_wake(volatile uint32_t *addr, int count);
/* Boot check of the condvars' kernel wait (and its SVC register layout). */
void dcr_pthread_selftest(void);

/* Cooperative sampling (RT_COOP_SAMPLE): the watchdog stores the handle of a
 * hung thread in g_sample_want; the next mutex lock or getspecific that
 * thread makes records its stack pointer and caller, and clears the request. */
extern volatile Handle g_sample_want;
extern volatile uint32_t g_sample_sp, g_sample_lr, g_sample_n;

/* ---- callbacks (weak; the defaults do nothing) ------------------------------
 * A tag each new thread inherits from its creator: port_thread_tag_for_new()
 * runs on the creating thread, and its value is handed to
 * port_thread_tag_enter() first thing on the new one (a port running two
 * copies of an engine keeps each copy's threads its own). */
uintptr_t port_thread_tag_for_new(void);
void port_thread_tag_enter(uintptr_t tag);

/* bionic_signal.c: a signal sent to one of our threads (tkill, pthread_kill,
 * raise, kill of ourselves). Return 1 if the port handled it (a garbage
 * collector's suspend / restart signals), 0 to let the runtime go on. */
int port_gc_signal(BThread *target, int sig);
/* Called before the runtime ends the process for a fatal signal the game
 * raised (SIGABRT, SIGSEGV, SIGBUS, SIGKILL): a port can log its own state. */
void port_on_fatal_signal(int sig, BThread *t);

#endif
