/* rt_boot.c -- the game program's start-up steps that do not depend on the
 * game: the boot report, the old folder's move, the APK search.
 *
 * They are here rather than in main.c so that a port which keeps its own
 * main.c (it replaces the runtime's) still has them. The runtime's main()
 * calls them in this order: rt_boot_migrate() before anything is written to
 * the game folder, dcr_report_boot() and rt_boot_migrate_report() once the
 * log is open, rt_boot_check_title() right after, rt_boot_find_apks() after
 * the NRO self-update. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <unistd.h>

#include "dcr_build.h" /* DCR_BUILD: RT_MIGRATE_MOVE_NEWER_NRO compares NROs with it */
#include "dcr_exefs.h"
#include "dcr_formats.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "error.h"
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
  if (dcr_manifest_probe(path) != 0)
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

/* --------------------------------------------------------- whose icon is it
 * The launcher installs this program for the forwarder icon made for its own
 * NRO: the icon's romfs names the NRO it starts (/nextNroPath). Launchers
 * before 2026-09-30 checked only that the icon was a forwarder, so one
 * started from sphaira running in sphaira's own forwarder icon installed the
 * game there: that icon has started the game ever since, never sphaira. A
 * forwarder made for this game's NRO is the only icon it may run in; on any
 * other (the NRO it names exists and does not carry this program), the
 * override is removed and the icon restarted, so it starts what it was made
 * for again. An icon whose NRO is gone (moved, renamed by hand) is left as it
 * is: whose it was cannot be told. Not a forwarder (an emulator running the
 * NSP directly): nothing to check. */
int rt_boot_icon_nro(char *out, size_t cap) {
  out[0] = 0;
  if (R_FAILED(romfsMountFromCurrentProcess("fwd")))
    return -1;
  FILE *f = fopen("fwd:/nextNroPath", "rb");
  if (f) {
    char buf[FS_MAX_PATH + 8] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = buf;
    while (*p == ' ' || *p == '"')
      p++;
    if (!strncasecmp(p, "sdmc:", 5))
      p += 5;
    snprintf(out, cap, "%s", p);
    n = strlen(out);
    while (n && (out[n - 1] == '"' || out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\r'))
      out[--n] = 0;
  }
  romfsUnmount("fwd");
  return out[0] ? 0 : -1;
}

/* title_id.txt in the game folder: every HOME-menu icon this game is
 * installed on, the one in use first, with where each override is and how to
 * remove the port, since the folders in atmosphere/contents/ are only
 * numbers. Icons named by the file before stay listed while their exefs.nsp
 * is still there. Written when it is missing or out of date. */
#define TITLE_FILE_MAX_ICONS 8

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

static int override_exists(u64 tid) {
  char p[96];
  snprintf(p, sizeof p, "sdmc:/atmosphere/contents/%016llX/exefs.nsp", (unsigned long long)tid);
  FILE *f = fopen(p, "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

static void write_title_file(u64 tid) {
  const char *root = dcr_game_root();
  const char *shown = strncmp(root, "sdmc:", 5) ? root : root + 5;
  char path[320], tmp[330], have[2048];
  snprintf(path, sizeof path, "%s/title_id.txt", root);
  size_t got = 0;
  FILE *f = fopen(path, "rb");
  if (f) {
    got = fread(have, 1, sizeof have - 1, f);
    fclose(f);
  }
  have[got] = 0;

  /* the icons: this one, then those the old file named that still have
   * this game's override (any run of exactly 16 hex digits is a title ID) */
  u64 ids[TITLE_FILE_MAX_ICONS];
  int count = 0;
  ids[count++] = tid;
  for (size_t i = 0; i + 16 <= got && count < TITLE_FILE_MAX_ICONS; i++) {
    if ((i > 0 && is_hex(have[i - 1])) || is_hex(have[i + 16]))
      continue;
    size_t k = 0;
    while (k < 16 && is_hex(have[i + k]))
      k++;
    if (k != 16)
      continue;
    char hex[17];
    memcpy(hex, have + i, 16);
    hex[16] = 0;
    u64 id = strtoull(hex, NULL, 16);
    int seen = 0;
    for (int j = 0; j < count; j++)
      seen |= ids[j] == id;
    if (!seen && exefs_is_forwarder_tid(id) && override_exists(id))
      ids[count++] = id;
    i += 15;
  }

  char want[2048];
  size_t n = 0;
#define ADD(...) (n += (size_t)snprintf(want + n, n < sizeof want ? sizeof want - n : 0, __VA_ARGS__))
  ADD(PORT_TITLE " runs from the HOME menu icon with title ID %016llX.\n\n", (unsigned long long)tid);
  ADD("It is installed on %s:\n", count > 1 ? "these icons (the first is the one in use)" : "this icon");
  for (int j = 0; j < count; j++)
    ADD("  %016llX: sd:/atmosphere/contents/%016llX/exefs.nsp\n", (unsigned long long)ids[j],
        (unsigned long long)ids[j]);
  ADD("\nTo remove " PORT_TITLE ":\n"
      "  1. delete %s in sd:/atmosphere/contents/ named above\n"
      "  2. delete %s: System Settings > Data Management > Manage Software\n"
      "  3. delete sd:%s/ (your saves and settings are in it)\n",
      count > 1 ? "each folder" : "the folder", count > 1 ? "the icons" : "its icon", shown);
#undef ADD
  if (n >= sizeof want)
    return;
  if (got == n && !memcmp(have, want, n))
    return;
  snprintf(tmp, sizeof tmp, "%s.part", path);
  f = fopen(tmp, "wb");
  int ok = f && fwrite(want, 1, n, f) == n;
  if (f && fclose(f) != 0)
    ok = 0;
  if (ok) {
    unlink(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok)
    unlink(tmp);
  debugPrintf("[boot] %s %s (%d icon%s)\n", ok ? "wrote" : "could not write", path, count, count > 1 ? "s" : "");
}

void rt_boot_check_title(void) {
  u64 tid = 0;
  if (R_FAILED(svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || !exefs_is_forwarder_tid(tid))
    return;
  char nro[FS_MAX_PATH], path[FS_MAX_PATH + 8];
  if (rt_boot_icon_nro(nro, sizeof nro) != 0) {
    debugPrintf("[boot] this icon (%016llX) names no NRO: not checked\n", (unsigned long long)tid);
    return;
  }
  snprintf(path, sizeof path, "sdmc:%s", nro);
  FILE *f = fopen(path, "rb");
  if (!f) {
    debugPrintf("[boot] this icon starts %s, which is gone: whose icon it is cannot be told\n", nro);
    return;
  }
  long off;
  size_t size;
  int ours = nro_romfs_file(f, PORT_NSP_NAME, &off, &size) == 0;
  fclose(f);
  if (ours) {
    debugPrintf("[boot] this icon (%016llX) starts %s: this game's\n", (unsigned long long)tid, nro);
    write_title_file(tid);
    return;
  }

  /* someone else's icon: give it back */
  char ovr[128];
  snprintf(ovr, sizeof ovr, "sdmc:/atmosphere/contents/%016llX/exefs.nsp", (unsigned long long)tid);
  const char *base = strrchr(nro, '/') ? strrchr(nro, '/') + 1 : nro;
  debugPrintf("[boot] this icon (%016llX) is %s's, not " PORT_NAME ".nro's: an older launcher installed "
              PORT_TITLE " on it. Removing that and restarting the icon.\n",
              (unsigned long long)tid, base);
  int removed = unlink(ovr) == 0;
  log_console_show_text();
  debugPrintf("\n%s\n\n"
              "This icon starts %s. An older " PORT_TITLE " launcher installed the game\n"
              "on it by mistake; %s.\n\n"
              "To play " PORT_TITLE ", make a forwarder for " PORT_NAME ".nro in sphaira\n"
              "(Homebrew > " PORT_TITLE " > Install Forwarder) and start the game from that icon.\n",
              removed ? "Giving this icon back" : "Could not give this icon back", base,
              removed ? "that is undone, and the icon starts it again in a moment"
                      : "the game could not remove itself from it");
  log_console_update();
  log_flush_ring();
  svcSleepThread(8000000000ll);
  if (removed) {
    Result rc = appletRestartProgram(NULL, 0);
    fatal_error("This icon starts %s, not " PORT_NAME ".nro. " PORT_TITLE " was removed from it.\n\n"
                "Restarting did not work (0x%x): close it and start it again.",
                base, (unsigned)rc);
  }
  fatal_error("This icon starts %s, not " PORT_NAME ".nro, but " PORT_TITLE " is installed on it.\n\n"
              "Delete %s to give the icon back.",
              base, ovr);
}
