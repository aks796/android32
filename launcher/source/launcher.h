/* launcher.h -- the shared launcher's progress screen (launcher_bar.c) and
 * the callbacks a port's own launcher sources may define (main.c has weak
 * defaults for all of them). 64-bit, standard libnx. MIT.
 */
#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <stddef.h>

#include "rt_apkfind.h"

/* ---------------------------------------------------------------- the bar */
/* The progress screen, as the game program's (util.c): the game's name
 * (PORT_TITLE), PORT_SETUP_NOTE, a green bar (0..1000) and the step, redrawn
 * when either changes. launcher_bar_off() clears it for text again (errors,
 * instructions); launcher_bar_on() tells whether it is showing. The launcher
 * shows it from the forwarder icon when RT_LAUNCHER_BAR is 1; a port's hooks
 * may use it for long work (a8r: installing its zips). */
void launcher_bar(const char *what, int permille);
void launcher_bar_off(void);
int launcher_bar_on(void);
/* The screen's title lines ("<title> for Nintendo Switch -- launcher"). */
void launcher_header(void);

/* ---------------------------------------------------------------- callbacks */
/* After the old folder's move, before the APK check: work of the port's own
 * (a8r: its mod's zips unpacked into the game folder). Non-zero: stop here
 * (the port printed why); the launcher waits for + and exits. */
int port_launcher_prepare(void);

/* What else the game needs besides the APK (sonic: the expansion file; a8r:
 * the OBB). Fills a status line for the screen ("game data: found") and,
 * when something is missing, what to copy where. 0: all there. */
int port_launcher_check(char *status, size_t scap, char *help, size_t hcap);

/* More lines about the APK found (flappy: which engine build it holds). */
void port_launcher_apk_note(const char *apk_path);

/* Step 1 of "Start this from its own HOME-menu icon": what to put in the
 * game folder. Default: the APK (PORT_APK_DESC), any file name. */
void port_launcher_instructions(void);

/* The APK roles; the same table as the game program's (rt_boot.h) --
 * PORT_APK_ROLES from port_config.h unless the port defines this. */
const RtApkRole *port_apk_roles(int *count);

#endif /* LAUNCHER_H */
