/* dcr_sched.h -- guest thread placement and priority (see dcr_sched.c). */
#ifndef DCR_SCHED_H
#define DCR_SCHED_H
#include <switch.h>

/* The kernel time-slices priority 59 on cores 0-2 (every 10 ms). */
#define DCR_GUEST_PRIO 59
#define DCR_GUEST_CORES 0x7ull

Result dcr_thread_set_cores(Handle h, s32 ideal, u64 mask);
Result dcr_thread_get_cores(Handle h, s32 *ideal, u64 *mask);
s32 dcr_sched_next_core(void);
void dcr_sched_guest(Handle h); /* a new guest thread: cores 0-2 */
void dcr_sched_init(void);      /* the main thread becomes a guest; self-test */

#endif
