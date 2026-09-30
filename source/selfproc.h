/* selfproc.h -- a real (non-pseudo) handle to our own process. */
#ifndef DCR_SELFPROC_H
#define DCR_SELFPROC_H
#include <switch.h>

/* Obtained once via self-IPC; INVALID_HANDLE if that failed. */
Handle dcr_self_process(void);

#endif
