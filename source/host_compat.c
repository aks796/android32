/* host_compat.c -- libc functions the host's own libraries need that
 * devkitARM's newlib declares but does not implement. MIT. */
#include <time.h>

/* C11 timespec_get (Mesa's c11/threads.h uses it for timed waits). */
int timespec_get(struct timespec *ts, int base) {
  if (!ts || base != TIME_UTC)
    return 0;
  return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0;
}
