/* nx_init.c -- libnx32 start-up overrides: heap sizing and service bring-up.
 *
 * HEAP. libnx's default __libnx_initheap sizes the heap as TotalMemory - Used.
 * For a 32-bit process that is wrong on hardware: the application pool can be
 * ~3 GB, but a 32-bit address space's HEAP REGION is only 1 GiB
 * (0x80000000-0xC0000000), so svcSetHeapSize would fail and libnx would abort
 * before main(). We clamp to the heap region and hold back RT_GFX_RESERVE_MB
 * for the graphics driver's own pool allocations. (The libnx32 fork's
 * __libnx_initheap clamps too, but holds back only 2 MiB and records nothing
 * for main(): this override stays.)
 *
 * SERVICES. libnx's default __appInit aborts on any failure. Under emulators the
 * time service's shared memory can fail to map for 32-bit processes (Ryujinx
 * 1.1.1098: MapSharedMemory = InvalidCurrentMemory) while everything else
 * works, so failures here are recorded and reported by main() instead of being
 * fatal before the log exists.
 *
 * WINDOW. The default window is made here (RT_OWN_WINDOW, below). MIT.
 */
#include <string.h>
#include <switch.h>

#include "rt_settings.h"
#include "nx_init.h"

/* RT_OWN_WINDOW: 1 = this file defines libnx's default window (nwindowGetDefault,
 * __nx_win_init, __nx_win_exit) so the vi display handle is ours; 0 = the
 * libnx32 fork's default_window.c (4.12.0: the same calls in the same order,
 * with nwindowGetDefaultDisplay() for the handle). Every port: 1 (hardware-
 * proven); 0 needs one hardware check of the vsync event. */
#ifndef RT_OWN_WINDOW
#define RT_OWN_WINDOW 1
#endif

NxInitInfo g_nxinit;

void __libnx_initheap(void) {
  u64 total = 0, used = 0, heap_region = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&heap_region, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);

  u64 want = total > used ? total - used : 0;
  const u64 reserve = (u64)RT_GFX_RESERVE_MB << 20;
  want = want > reserve ? want - reserve : want / 2;
  if (heap_region && want > heap_region)
    want = heap_region;
  want &= ~0x1FFFFFull; /* svcSetHeapSize granularity is 2 MiB */

  void *addr = NULL;
  u64 size = want; /* what the heap really is: the size last passed */
  Result rc = svcSetHeapSize(&addr, (u32)size);
  if (R_FAILED(rc)) {
    /* Retry smaller rather than dying before the log exists. */
    for (u64 next = want >> 1; next >= (32ull << 20) && R_FAILED(rc); next >>= 1) {
      size = next & ~0x1FFFFFull;
      rc = svcSetHeapSize(&addr, (u32)size);
    }
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + size;

  g_nxinit.total = total;
  g_nxinit.used = used;
  g_nxinit.heap_region = heap_region;
  g_nxinit.heap = size;
  g_nxinit.heap_base = (uintptr_t)addr;
}

/* ---- the default window ----
 * libnx's default_window.c, made here instead so the display handle stays
 * ours: vi allows one OpenDisplay("Default") per process (a second is
 * 2114-0009, "already opened" -- hardware 2026-09-24), and that handle is
 * what gives the display's vsync event (the Crossy Road port's dcr_vsync.c).
 * Defining all three symbols keeps libnx's copy out of the link. Same calls,
 * same order. */
#if RT_OWN_WINDOW
static ViDisplay g_vi_display;
static ViLayer g_vi_layer;
static NWindow g_default_win;

NWindow *nwindowGetDefault(void) { return &g_default_win; }
ViDisplay *dcr_vi_display(void) { return g_vi_display.initialized ? &g_vi_display : NULL; }

void __nx_win_init(void) {
  Result rc = viInitialize(ViServiceType_Default);
  if (R_SUCCEEDED(rc)) {
    rc = viOpenDefaultDisplay(&g_vi_display);
    if (R_SUCCEEDED(rc)) {
      rc = viCreateLayer(&g_vi_display, &g_vi_layer);
      if (R_SUCCEEDED(rc)) {
        rc = viSetLayerScalingMode(&g_vi_layer, ViScalingMode_FitToLayer);
        if (R_SUCCEEDED(rc)) {
          rc = nwindowCreateFromLayer(&g_default_win, &g_vi_layer);
          if (R_SUCCEEDED(rc))
            nwindowSetDimensions(&g_default_win, 1280, 720);
        }
        if (R_FAILED(rc))
          viCloseLayer(&g_vi_layer);
      }
      if (R_FAILED(rc))
        viCloseDisplay(&g_vi_display);
    }
    if (R_FAILED(rc))
      viExit();
  }
  if (R_FAILED(rc))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
}

void __nx_win_exit(void) {
  nwindowClose(&g_default_win);
  viCloseLayer(&g_vi_layer);
  viCloseDisplay(&g_vi_display);
  viExit();
}
#else
void __nx_win_init(void); /* the libnx32 fork's default_window.c */
void __nx_win_exit(void);
ViDisplay *dcr_vi_display(void) { return nwindowGetDefaultDisplay(); }
#endif

void __appInit(void) {
  g_nxinit.rc_sm = smInitialize();
  if (R_FAILED(g_nxinit.rc_sm))
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
      hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    setsysExit();
  }

  g_nxinit.rc_applet = appletInitialize();
  g_nxinit.rc_hid = hidInitialize();
  g_nxinit.rc_time = timeInitialize();
  g_nxinit.rc_fs = fsInitialize();
  if (R_SUCCEEDED(g_nxinit.rc_fs))
    g_nxinit.rc_sdmc = fsdevMountSdmc();
  else
    g_nxinit.rc_sdmc = g_nxinit.rc_fs;

  /* The default window: without it nwindowGetDefault() is an empty NWindow
   * and both the on-screen log and EGL's window surface have nothing to draw
   * into (libnx's stock __appInit makes this call; ours must too). */
  __nx_win_init();
}

void __appExit(void) {
  __nx_win_exit();
  fsdevUnmountAll();
  fsExit();
  if (R_SUCCEEDED(g_nxinit.rc_time))
    timeExit();
  hidExit();
  appletExit();
  smExit();
}
