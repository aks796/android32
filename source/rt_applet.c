/* rt_applet.c -- the applet lifecycle every port's frame loop had its own
 * copy of: focus changes, freezes, the system's exit request, and whether a
 * stop in the frames is expected (for the watchdog).
 *
 * Focus. libnx leaves an application in SuspendHomeSleep: the system freezes
 * the process for the HOME menu and sleep and sends NO focus messages, so the
 * game never got its onPause / onResume on hardware (no "focus lost" in any
 * log: found by the Asphalt 8 Retry port, 2026-09-30). With ...Notify the
 * process is still frozen, and hears of it when it runs again. The hook only
 * records what the system says (it runs inside appletMainLoop());
 * rt_applet_poll, at the top of the next frame, tells the game:
 *   lost:    port_focus_lost(), the log ring written out, the clocks stopped
 *            (dcr_time_suspend: the time away is no game time);
 *   regained: port_focus_gaining(), the clocks run again, port_focus_gained().
 *
 * Freezes. The clocks (bionic_time.c) see a freeze whether or not a focus
 * message came -- both can arrive at once on waking and leave the focus as it
 * was -- and port_process_frozen() hears of each (checked before the focus,
 * as lab2 did).
 *
 * The watchdog (watchdog.c) reports a hang when no frame came for 10 s while
 * dcr_boot_in_focus(): the frame loop runs (the first rt_applet_poll until
 * rt_applet_stop), in focus, no exit asked, no system screen up
 * (dcr_applet_busy) and the port does not hold it (port_watchdog_hold). MIT.
 */
#include <switch.h>

#include "dcr_time.h"
#include "rt_applet.h"
#include "rt_settings.h"
#include "util.h"

/* RT_WATCHDOG_BOOT: 1 lets the watchdog report hangs before the frame loop
 * runs as well (from dcr_watchdog_start on: the engine's own start-up); 0
 * only from the first rt_applet_poll. dcr 1 (its Unity/Mono start-up hangs
 * were found that way); every other port 0. */
#ifndef RT_WATCHDOG_BOOT
#define RT_WATCHDOG_BOOT 0
#endif

enum { PHASE_BOOT, PHASE_LOOP, PHASE_DONE };

static AppletHookCookie g_hook;
static int g_hooked;
static volatile int g_focused = 1; /* what the system said (the hook) */
static int g_applied = 1;          /* what the game was told (rt_applet_poll) */
static volatile int g_exit, g_busy, g_phase;
static unsigned g_freezes;

/* ---------------------------------------------------------- weak defaults */
__attribute__((weak)) void port_focus_lost(void) {}
__attribute__((weak)) void port_focus_gaining(void) {}
__attribute__((weak)) void port_focus_gained(void) {}
__attribute__((weak)) void port_process_frozen(unsigned count) { (void)count; }
__attribute__((weak)) int port_watchdog_hold(void) { return 0; }

/* ------------------------------------------------------------------ hook */
static void on_applet(AppletHookType type, void *param) {
  static const char *const names[] = {"focus state", "operation mode", "performance mode",
                                      "EXIT REQUEST", "resume", "capture button",
                                      "screenshot taken", "request to display"};
  (void)param;
  if (type == AppletHookType_OnExitRequest) {
    debugPrintf("[applet] the system asked the game to close (HOME menu > Close Software, "
                "power off/restart, or another program launched)\n");
    g_exit = 1;
  } else if ((unsigned)type < sizeof names / sizeof names[0]) {
    debugPrintf("[applet] %s (focus %d, mode %d)\n", names[type], (int)appletGetFocusState(),
                (int)appletGetOperationMode());
  }
  if (type == AppletHookType_OnFocusState || type == AppletHookType_OnOperationMode)
    g_focused = appletGetFocusState() == AppletFocusState_InFocus;
}

void rt_applet_start(void) {
  if (g_hooked)
    return;
  Result rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
  if (R_FAILED(rc))
    debugPrintf("[applet] focus notifications unavailable (0x%x)\n", (unsigned)rc);
  appletHook(&g_hook, on_applet, NULL);
  g_hooked = 1;
}

void rt_applet_poll(void) {
  if (g_phase == PHASE_BOOT)
    g_phase = PHASE_LOOP;
  const unsigned fz = dcr_time_freezes();
  if (fz != g_freezes) {
    g_freezes = fz;
    port_process_frozen(fz);
  }
  const int focused = g_focused;
  if (focused == g_applied)
    return;
  g_applied = focused;
  if (!focused) {
    debugPrintf("[applet] focus lost: pausing\n");
    port_focus_lost();
    log_flush_ring();
    dcr_time_suspend();
  } else {
    port_focus_gaining();
    dcr_time_resume();
    port_focus_gained();
    debugPrintf("[applet] focus regained: resumed\n");
  }
}

void rt_applet_stop(void) {
  if (g_hooked) {
    appletUnhook(&g_hook);
    g_hooked = 0;
  }
  g_phase = PHASE_DONE;
  if (!g_applied)
    dcr_time_resume(); /* frozen clocks would stall any timed wait in the shutdown */
}

int rt_focused(void) { return g_applied; }
int rt_exit_requested(void) { return g_exit; }
void rt_request_exit(void) { g_exit = 1; }

void dcr_applet_busy(int on) { g_busy = on; }
int dcr_applet_is_busy(void) { return g_busy; }

int dcr_boot_in_focus(void) {
  const int phase = g_phase;
  if (phase != PHASE_LOOP && !(RT_WATCHDOG_BOOT && phase == PHASE_BOOT))
    return 0;
  return g_focused && !g_exit && !g_busy && !port_watchdog_hold();
}
