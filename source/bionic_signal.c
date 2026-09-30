/* bionic_signal.c -- POSIX signals for the bionic ABI.
 *
 * Horizon has no signals. Handlers are recorded (so code that installs them
 * proceeds, and so the log can say who wanted what), and nothing is delivered
 * asynchronously: faults go to the crash report (exc_handler.c, which may
 * deliver a synchronous fault to a recorded handler: dcr_sigaction_get). The
 * cases that matter:
 *
 *   tkill / pthread_kill with a garbage collector's suspend / restart signals
 *   (Boehm's: 30 and 24) -- a collector stops the world by signalling every
 *   thread and waiting for acknowledgements. A port with such a collector
 *   answers them in port_gc_signal() (on the target's behalf).
 *
 *   raise / kill of ourselves with SIGABRT, SIGSEGV etc. -- fatal and logged
 *   (abort() in the game ends there); port_on_fatal_signal() may log first.
 * MIT.
 */
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "error.h"
#include "util.h"

static struct b_sigaction g_actions[L_NSIG];
static Mutex g_sig_lock;

int b_sigaction(int sig, const struct b_sigaction *act, struct b_sigaction *old) {
  if (sig <= 0 || sig >= L_NSIG) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  mutexLock(&g_sig_lock);
  if (old)
    *old = g_actions[sig];
  if (act) {
    g_actions[sig] = *act;
    debugPrintf("[signal] handler for %d set to %p (from %p)\n", sig, act->sa_handler_or_action,
                __builtin_return_address(0));
  }
  mutexUnlock(&g_sig_lock);
  return 0;
}

void *b_bsd_signal(int sig, void *handler) {
  struct b_sigaction act = {handler, 0, 0, NULL}, old;
  if (b_sigaction(sig, &act, &old) < 0)
    return (void *)-1; /* SIG_ERR */
  return old.sa_handler_or_action;
}

/* A copy of the current action for `sig`; 0 if out of range. */
int dcr_sigaction_get(int sig, struct b_sigaction *out) {
  if (sig <= 0 || sig >= L_NSIG)
    return 0;
  *out = g_actions[sig];
  return 1;
}

void *dcr_signal_handler(int sig) {
  return (sig > 0 && sig < L_NSIG) ? g_actions[sig].sa_handler_or_action : NULL;
}

int b_sigprocmask(int how, const uint32_t *set, uint32_t *old) {
  if (old)
    *old = 0;
  return 0;
}

int b_sigsuspend(const uint32_t *mask) {
  /* No signal ever arrives (a collector's suspend handler, the usual caller,
   * never runs here). Returning EINTR is the POSIX answer and makes any
   * caller loop safely. */
  svcSleepThread(1000000);
  b_set_errno(L_EINTR);
  return -1;
}

/* Callbacks (bionic_pthread.h); the defaults handle nothing. */
__attribute__((weak)) int port_gc_signal(BThread *target, int sig) { return 0; }
__attribute__((weak)) void port_on_fatal_signal(int sig, BThread *t) {}

static int deliver_to(BThread *t, int sig) {
  if (sig == 0)
    return 0; /* existence probe */
  if (port_gc_signal(t, sig))
    return 0;
  if (sig == L_SIGABRT || sig == L_SIGSEGV || sig == L_SIGBUS || sig == L_SIGKILL) {
    debugPrintf("[fatal] signal %d raised by the game (tid %d, from %p)\n", sig,
                t ? t->tid : -1, __builtin_return_address(0));
    port_on_fatal_signal(sig, t);
    log_flush_ring();
    fatal_error("The game raised signal %d from %p.", sig, __builtin_return_address(0));
  }
  static int logged;
  if (logged++ < 16)
    debugPrintf("[signal] signal %d to tid %d dropped (no delivery on Horizon)\n", sig,
                t ? t->tid : -1);
  return 0;
}

int b_tkill(int tid, int sig) {
  BThread *t = b_thread_by_tid(tid);
  if (!t) {
    b_set_errno(L_ESRCH);
    return -1;
  }
  return deliver_to(t, sig);
}

int b_pthread_kill(b_pthread_t th, int sig) {
  BThread *t = (BThread *)th;
  if (!t || t->magic != BTHREAD_MAGIC)
    return L_ESRCH;
  deliver_to(t, sig);
  return 0;
}

int b_raise(int sig) { return deliver_to(b_thread_self(), sig); }

int b_kill(b_pid_t pid, int sig) {
  if (pid == 4242 || pid == 0 || pid == -1)
    return deliver_to(b_thread_self(), sig);
  b_set_errno(L_ESRCH);
  return -1;
}

int b_sigemptyset(uint32_t *set) {
  if (set)
    *set = 0;
  return 0;
}
