/* dcr_time.h -- the process's clocks (bionic_time.c).
 *
 * Two things the system tick counts that a game's clocks must not:
 *   - a focus suspension (HOME menu, sleep), between dcr_time_suspend() and
 *     dcr_time_resume(): MONOTONIC stands still through it;
 *   - a freeze (the system stopping the whole process, which no message need
 *     announce): found by a watch thread and left out of every clock.
 * RT_TIME_SHIFT (port_config.h) says whether the wall clock leaves the
 * freezes out as well: a bitmask of the two values below. MIT.
 */
#ifndef DCR_TIME_H
#define DCR_TIME_H
#include <stdint.h>

/* RT_TIME_SHIFT bits: which wall clocks run on the process's own time (the
 * tick less the freezes) rather than the real one. */
#define RT_TIME_SHIFT_REALTIME 1 /* clock_gettime(REALTIME), gettimeofday, ftime */
#define RT_TIME_SHIFT_TIME     2 /* time(); needs RT_TIME_SHIFT_REALTIME as well */

void dcr_time_init(void);         /* after services are up; starts the watch thread */
void dcr_time_suspend(void);      /* focus lost (HOME / sleep) */
void dcr_time_resume(void);       /* focus regained */
uint64_t dcr_monotonic_ns(void);  /* monotonic: suspensions and freezes removed */
uint64_t dcr_run_ns(void);        /* the system tick less the time the process was frozen */
unsigned dcr_time_freezes(void);  /* freezes (HOME menu, sleep) seen so far */

#endif
