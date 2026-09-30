/* rt_boot.c -- the game program's start-up steps that do not depend on the
 * game: the boot report, the old folder's move, the APK search.
 *
 * They are here rather than in main.c so that a port which keeps its own
 * main.c (it replaces the runtime's) still has them. The runtime's main()
 * calls them in this order: rt_boot_migrate() before anything is written to
 * the game folder, dcr_report_boot() and rt_boot_migrate_report() once the
 * log is open, rt_boot_find_apks() after the NRO self-update. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_build.h" /* DCR_BUILD: RT_MIGRATE_MOVE_NEWER_NRO compares NROs with it */
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "nx_init.h"
#include "rt_apkfind.h"
#include "rt_boot.h"
#include "rt_migrate.h"
#include "rt_settings.h"
#include "util.h"

/* How the player's APK is described in the error screens (and the
 * launcher's instructions): "Labyrinth 2 (se.illusionlabs.labyrinth2 1.29)". */
#ifndef PORT_APK_DESC
#define PORT_APK_DESC PORT_TITLE " (" PORT_PACKAGE ")"
#endif
/* The APK role table (rt_apkfind.h), a list of RtApkRole initializers; the
 * launcher reads the same one. Default: one role, PORT_PACKAGE's APK,
 * PORT_APK_DEFAULT_NAME first (lab2's rule). */
#ifndef PORT_APK_ROLES
#define PORT_APK_ROLES RT_APK_ROLE_DEFAULT
#endif

extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden"))); /* crt0_reloc.c */

/* ------------------------------------------------------------- callbacks */
__attribute__((weak)) void port_report_boot(void) {}
__attribute__((weak)) void port_before_update(void) {}
__attribute__((weak)) int port_after_apk_find(void) { return 0; }

__attribute__((weak)) const char *port_apk_help(void) {
  return "Copy the APK of your own " PORT_APK_DESC " into that folder,\n"
         "under any file name: the game's files come from it.";
}

/* at file scope: compound literals in PORT_APK_ROLES (the need lists) are
 * static there */
static const RtApkRole k_port_roles[] = {PORT_APK_ROLES};

__attribute__((weak)) const RtApkRole *port_apk_roles(int *count) {
  *count = (int)(sizeof k_port_roles / sizeof k_port_roles[0]);
  return k_port_roles;
}

/* ------------------------------------------------------------- the report */
void dcr_report_boot(void) {
  const u64 MB = 1024 * 1024;
  debugPrintf("[boot] === %s ===\n", PORT_BANNER);
  static const char *const paths[] = {"none needed", "patched through a writable alias (hardware)",
                                      "direct writes (emulator: pseudo-handle refused)"};
  debugPrintf("[boot] text relocations: %s\n", __dcr_reloc_path < 3 ? paths[__dcr_reloc_path] : "?");
  port_report_boot();
  debugPrintf("[heap] total %u MB, used %u MB at start, heap region %u MB, heap %u MB @ %p\n",
              (unsigned)(g_nxinit.total / MB), (unsigned)(g_nxinit.used / MB),
              (unsigned)(g_nxinit.heap_region / MB), (unsigned)(g_nxinit.heap / MB),
              (void *)g_nxinit.heap_base);
  debugPrintf("[svc] sm=%x applet=%x hid=%x time=%x fs=%x sdmc=%x\n", g_nxinit.rc_sm, g_nxinit.rc_applet,
              g_nxinit.rc_hid, g_nxinit.rc_time, g_nxinit.rc_fs, g_nxinit.rc_sdmc);
  if (R_FAILED(g_nxinit.rc_time))
    debugPrintf("[svc] time service unavailable: clocks fall back to the system tick\n");
}

/* ------------------------------------------------------------- the old folder */
static RtMigrateResult g_mig;

int rt_boot_migrate(void) { return rt_migrate_port(dcr_game_root(), DCR_BUILD, NULL, &g_mig); }

void rt_boot_migrate_report(void) {
  if (g_mig.msg[0])
    debugPrintf("[setup] the game folder is %s: %s\n", dcr_game_root(), g_mig.msg);
}

const RtMigrateResult *rt_boot_migrate_result(void) { return &g_mig; }

/* ------------------------------------------------------------- the APKs */
static RtApkFound g_apks;

static int manifest_of(const char *path, char *package, size_t cap, int *version_code) {
  if (dcr_manifest_load(path) != 0)
    return -1;
  snprintf(package, cap, "%s", dcr_manifest_package());
  *version_code = dcr_manifest_version_code();
  return 0;
}

int rt_boot_find_apks(void) {
  int n = 0;
  const RtApkRole *roles = port_apk_roles(&n);
  RtApkEnv env = {manifest_of, debugPrintf, 1};
  int rc = rt_apk_find(dcr_game_root(), roles, n, &env, &g_apks);
  dcr_set_apk_path(g_apks.path[0]);
  return rc;
}

const char *dcr_apk_role_path(int role) {
  return role >= 0 && role < RT_APK_ROLES_MAX ? g_apks.path[role] : "";
}

const char *dcr_apk_summary(void) { return g_apks.summary[0] ? g_apks.summary : "no APK at all"; }
