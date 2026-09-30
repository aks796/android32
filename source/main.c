/* main.c -- the game program's boot sequence (32-bit), for every port.
 *
 * The port supplies port_load() (from the APK to the game's first code) and
 * port_run() (the constructors and the game), and may hook in with the weak
 * callbacks in rt_boot.h. A port with its own main.c replaces this file; the
 * steps it needs are in rt_boot.c. The order here matters; each step says
 * why it is where it is. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dcr_apkcache.h"
#include "dcr_boost.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "dcr_sched.h"
#include "dcr_setup.h"
#include "dcr_time.h"
#include "emu_fixups.h"
#include "error.h"
#include "rt_applet.h"
#include "rt_audout.h"
#include "rt_boot.h"
#include "rt_cfg.h"
#include "rt_settings.h"
#include "selfproc.h"
#include "util.h"

void dcr_config_load(void);     /* the port's dcr_config.c, on group G's INI engine */
void dcr_pthread_selftest(void); /* bionic_pthread.c */
void dcr_io_selftest(void);      /* bionic_io.c */
int dcr_gl_selftest(void);       /* gl_mesa.c */

/* 1: under an emulator, the wrapper's own instructions its A32 decoder
 * lacks are rewritten right after the log opens, before any of Mesa runs
 * (emu_fixups.c). dcr/lab2/sonic/a8r had it (dcr later, after its modules:
 * it keeps its module fix in port_load); abs/pvz/flappy had no emu_fixups.c.
 * Nothing happens on hardware. */
#ifndef RT_EMU_FIXUPS
#define RT_EMU_FIXUPS 1
#endif
/* 1: the CPU goes to the load clock from here to the first picture
 * (dcr_boost.c). a8r 0: it boosts from its start screen's PLAY, in
 * port_load. */
#ifndef RT_BOOST_AT_BOOT
#define RT_BOOST_AT_BOOT 1
#endif
/* 1: reads of the APK go through the RAM cache (dcr_apkcache.c). All ports
 * 1 (flappy's own boot never enabled it: now it does). */
#ifndef RT_APK_CACHE
#define RT_APK_CACHE 1
#endif
/* 1: an APK whose package is not PORT_PACKAGE is refused (flappy: its
 * engine draws nothing for another package); 0: a warning in the log. */
#ifndef RT_PACKAGE_MISMATCH_FATAL
#define RT_PACKAGE_MISMATCH_FATAL 0
#endif

static void find_apks(const char *root) {
  if (rt_boot_find_apks() != 0)
    fatal_error("No APK of %s in %s\n(found: %s).\n\n%s", PORT_TITLE, root, dcr_apk_summary(), port_apk_help());
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;
  const char *root = dcr_game_root();

  /* The old game folder moves in before anything is written here (the log,
   * config.ini): its files are what the rest reads. Logged below. */
  rt_boot_migrate();
  mkdir(root, 0777);
  log_init(root);
#if RT_EMU_FIXUPS
  if (dcr_is_emulator())
    dcr_emu_fix_self(); /* before any of Mesa runs */
#endif
  log_console_open(); /* blank: text only when asked or for setup work */
  dcr_report_boot();
  rt_boot_migrate_report();

  if (chdir(root) != 0)
    debugPrintf("[boot] WARNING: chdir(%s) failed\n", root);
  dcr_config_load(); /* config.ini */
#if RT_BOOST_AT_BOOT
  dcr_boost_launch_begin(); /* the load clock until the first picture (dcr_boost.c) */
#endif
  if (rt_config()->boot_log)
    log_console_show_text();
  dcr_time_init();
  dcr_path_prepare_dirs();

  /* HOME / sleep: suspended with notification from here on, before anything
   * that can take long (rt_applet.c). */
  rt_applet_start();

  /* A newer build of this program in the launcher NRO: install it and
   * restart into it before anything else happens (dcr_setup.c) -- and so
   * before any APK is renamed. */
  port_before_update();
  dcr_setup_update_from_nro();

  /* The player's APKs, whatever they are called: by what is in them. */
  find_apks(root);
  if (port_after_apk_find())
    find_apks(root); /* the port added files (from the NRO) */
  const char *apk = dcr_apk_path();
#if RT_APK_CACHE
  dcr_apkcache_set_path(apk); /* the engine's reads of it are cached (dcr_apkcache.c) */
#endif
  if (dcr_manifest_load(apk) != 0)
    fatal_error("%s is unreadable.\n\n%s", apk, port_apk_help());
  if (strcmp(dcr_manifest_package(), PORT_PACKAGE)) {
#if RT_PACKAGE_MISMATCH_FATAL
    fatal_error("%s is %s, not %s (" PORT_PACKAGE ").\n\n%s", apk, dcr_manifest_package(), PORT_TITLE,
                port_apk_help());
#else
    debugPrintf("[boot] WARNING: the APK is %s, not %s\n", dcr_manifest_package(), PORT_PACKAGE);
#endif
  }
  debugPrintf("[boot] the APK: %s, %s %s (%d)\n", strrchr(apk, '/') ? strrchr(apk, '/') + 1 : apk,
              dcr_manifest_package(), dcr_manifest_version_name(), dcr_manifest_version_code());

  if (dcr_self_process() == INVALID_HANDLE)
    fatal_error("Could not obtain a handle to this process.\n"
                "The loader needs it to map the game's code.");

  /* The port: setup from the APK, its modules loaded and patched. */
  if (port_load(apk) != 0) {
    debugPrintf("[boot] exit before the game started\n");
    log_flush_ring();
    return 0;
  }

  /* The main thread becomes a guest thread like the game's own: priority
   * 59 on cores 0-2, where the kernel time-slices (dcr_sched.c). */
  dcr_sched_init();
  port_audio_selftest();
  dcr_pthread_selftest();
  dcr_io_selftest();
#if DCR_GL_MESA
  if (dcr_is_emulator() || rt_config()->gl_selftest)
    dcr_gl_selftest(); /* always under an emulator; on hardware when asked */
#endif

  /* System.loadLibrary: the constructors, then the game. */
  port_run();
  debugPrintf("[boot] exiting\n");
  log_flush_ring();
  return 0;
}
