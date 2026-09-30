/* nx_init.h -- what nx_init.c's pre-main start-up recorded, for main() to log. MIT. */
#ifndef DCR_NX_INIT_H
#define DCR_NX_INIT_H
#include <stdint.h>
#include <switch.h>

typedef struct {
  u64 total, used, heap_region, heap;
  uintptr_t heap_base;
  Result rc_sm, rc_applet, rc_hid, rc_time, rc_fs, rc_sdmc;
} NxInitInfo;

extern NxInitInfo g_nxinit;

/* The default display (opened once per process, with the default window),
 * for its vsync event; NULL before __appInit or if it failed. */
ViDisplay *dcr_vi_display(void);

#endif
