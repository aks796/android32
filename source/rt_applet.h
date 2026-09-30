/* rt_applet.h -- the applet lifecycle: focus, freezes, the system's exit
 * request, and whether the watchdog should expect frames (rt_applet.c).
 *
 * rt_applet_start() is called early in boot (main.c; again is a no-op). A
 * port's frame loop:
 *
 *   while (!rt_exit_requested() && appletMainLoop()) {
 *     rt_applet_poll();
 *     if (!rt_focused()) { svcSleepThread(50000000ll); continue; }
 *     ... one frame ...
 *   }
 *   rt_applet_stop();           (then the port's own way out)
 * MIT.
 */
#ifndef RT_APPLET_H
#define RT_APPLET_H

/* Focus messages on (SuspendHomeSleepNotify), then the hook. */
void rt_applet_start(void);
/* Once per frame, on the thread that runs appletMainLoop(): calls the
 * callbacks below for what happened since the last call. The first call also
 * tells the watchdog the frame loop is running. */
void rt_applet_poll(void);
/* The hook off; the clocks resumed if the game was last told it lost focus.
 * No hang reports after this: frames are no longer expected. */
void rt_applet_stop(void);

/* The focus the game was last told of (by rt_applet_poll). */
int rt_focused(void);
/* The system asked the game to close (HOME > Close Software, power off...),
 * or the port asked (rt_request_exit). */
int rt_exit_requested(void);
void rt_request_exit(void);

/* A system screen (software keyboard, controller applet, profile picker)
 * holds the calling thread while it is up: no frames, and nothing wrong.
 * Call with 1 before showing it and 0 after. */
void dcr_applet_busy(int on);
int dcr_applet_is_busy(void);

/* For the watchdog: the frame loop runs, in focus, no exit, no system
 * screen, and port_watchdog_hold() says 0. Safe from any thread. */
int dcr_boot_in_focus(void);

/* ------------------------------------------------------------ callbacks
 * Weak: a port defines the ones it needs. All but port_watchdog_hold run on
 * the thread calling rt_applet_poll. */
/* Focus lost (HOME, sleep, an overlay): pause the game, its sound; save.
 * Then the runtime writes out the log ring and stops the game's clocks. */
void port_focus_lost(void);
/* Focus coming back, before the game's clocks run again: CPU clocks up,
 * the port's own clocks resynced (sonic: its CPU clock, the boost's frame
 * start and its game clock; a8r: its CPU clock). */
void port_focus_gaining(void);
/* Focus back, clocks running: resume the game and its sound. */
void port_focus_gained(void);
/* The process was frozen (HOME menu, sleep) -- found by the clocks
 * (dcr_time_freezes()), whether or not focus messages came: count is the
 * number of freezes so far. lab2 pauses a level here. */
void port_process_frozen(unsigned count);
/* 1 while the game is paused on purpose and presents no frames (a8r: its GL
 * thread parked in Game.Pause). Called from the watchdog's thread. */
int port_watchdog_hold(void);

#endif /* RT_APPLET_H */
