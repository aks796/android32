/* dcr_time.h -- the port's clock policy (bionic_time.c) and Time hooks (dcr_time.c). */
#ifndef DCR_TIME_H
#define DCR_TIME_H
#include <stdint.h>

void dcr_time_init(void);         /* after services are up */
void dcr_time_suspend(void);      /* focus lost (HOME / sleep) */
void dcr_time_resume(void);       /* focus regained */
uint64_t dcr_monotonic_ns(void);  /* monotonic, minus time spent suspended */
uint64_t dcr_run_ns(void);        /* the system tick less the time the process was frozen */
unsigned dcr_time_freezes(void);  /* freezes (HOME menu, sleep) seen so far */

#endif
