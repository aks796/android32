/* main.c -- boot sequence for the Labyrinth 2 wrapper (32-bit).
 *
 * The order here matters; each step says why it is where it is. MIT.
 */
#include <malloc.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lab.h"
#include "config.h"
#include "dcr_config.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "dcr_sched.h"
#include "dcr_time.h"
#include "error.h"
#include "nx_init.h"
#include "selfproc.h"
#include "so_util.h"
#include "util.h"

void dcr_setup_update_from_nro(void); /* dcr_setup.c */
int dcr_setup_ipad_from_nro(char *out, size_t cap);
void dcr_setup_from_apk(const char *apk);

static char g_root[256] = "sdmc:" DCR_ROOT_PATH;
const char *dcr_game_root(void) { return g_root; }
static char g_apk[512];
const char *dcr_apk_path(void) { return g_apk; }

/* macOS leaves "._name" files beside what it copies: never the game's */
static int apple_double(const char *n) { return n[0] == '.' && n[1] == '_'; }

static int has_ext(const char *n, const char *ext) {
  size_t a = strlen(n), b = strlen(ext);
  return a > b && !strcasecmp(n + a - b, ext) && !apple_double(n);
}

/* Builds before 2026-09-30 kept everything in sd:/switch/labyrinth2 (the
 * NRO was Labyrinth2.nro). The first start of this one moves it all here --
 * the APK and .ipa, config.ini, data/ (the saves), fonts, level packs --
 * but for the old NRO; what this folder has already stays. For the log
 * (it is not open yet). */
static char g_moved[640];

static void move_old_folder(void) {
  const char *old = "sdmc:" DCR_OLD_ROOT_PATH;
  DIR *d = opendir(old);
  if (!d)
    return;
  /* the names first: renaming while reading the folder can skip some */
  static char names[256][256];
  int n = 0;
  struct dirent *de;
  while (n < 256 && (de = readdir(d))) {
    size_t len = strlen(de->d_name);
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || has_ext(de->d_name, ".nro") ||
        len >= sizeof names[0])
      continue;
    memcpy(names[n++], de->d_name, len + 1);
  }
  closedir(d);
  int moved = 0, left = 0;
  for (int i = 0; i < n; i++) {
    char src[640], dst[640];
    struct stat st;
    snprintf(src, sizeof src, "%s/%.255s", old, names[i]);
    snprintf(dst, sizeof dst, "%s/%.255s", g_root, names[i]);
    if (stat(dst, &st) == 0 || rename(src, dst) != 0)
      left++;
    else
      moved++;
  }
  if (moved)
    snprintf(g_moved, sizeof g_moved, "%d file%s and folder%s moved from %s to %s (%d left there)", moved,
             moved == 1 ? "" : "s", moved == 1 ? "" : "s", old, g_root, left);
}

/* The APK: any *.apk here whose manifest is Labyrinth 2's, whatever it is
 * called -- game.apk first, else the newest. "" when there is none. */
static void find_apk(char *out, size_t cap) {
  out[0] = 0;
  DIR *d = opendir(g_root);
  if (!d)
    return;
  time_t best_t = 0;
  int best_named = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!has_ext(de->d_name, ".apk"))
      continue;
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_root, de->d_name);
    if (dcr_manifest_load(p) != 0 || strcmp(dcr_manifest_package(), DCR_PACKAGE)) {
      debugPrintf("[boot] %s is not an APK of Labyrinth 2 (%s): not used\n", de->d_name, dcr_manifest_package());
      continue;
    }
    struct stat st;
    time_t t = stat(p, &st) == 0 ? st.st_mtime : 0;
    int named = !strcasecmp(de->d_name, DCR_APK_NAME);
    if (!out[0] || named > best_named || (named == best_named && t > best_t)) {
      snprintf(out, cap, "%s", p);
      best_t = t, best_named = named;
    }
  }
  closedir(d);
}

extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden")));

static void report_boot(void) {
  const u64 MB = 1024 * 1024;
  debugPrintf("[boot] === labyrinth2_nx: Labyrinth 2 (Illusion Labs engine, armeabi) ===\n");
  static const char *const paths[] = {"none needed", "patched through a writable alias (hardware)",
                                      "direct writes (emulator: pseudo-handle refused)"};
  debugPrintf("[boot] text relocations: %s\n",
              __dcr_reloc_path < 3 ? paths[__dcr_reloc_path] : "?");
  debugPrintf("[heap] total %u MB, used %u MB at start, heap region %u MB, heap %u MB @ %p\n",
              (unsigned)(g_nxinit.total / MB), (unsigned)(g_nxinit.used / MB),
              (unsigned)(g_nxinit.heap_region / MB), (unsigned)(g_nxinit.heap / MB),
              (void *)g_nxinit.heap_base);
  debugPrintf("[svc] sm=%x applet=%x hid=%x time=%x fs=%x sdmc=%x\n", g_nxinit.rc_sm,
              g_nxinit.rc_applet, g_nxinit.rc_hid, g_nxinit.rc_time, g_nxinit.rc_fs,
              g_nxinit.rc_sdmc);
  if (R_FAILED(g_nxinit.rc_time))
    debugPrintf("[svc] time service unavailable: clocks fall back to the system tick\n");
}

int main(int argc, char *argv[]) {
  mkdir(g_root, 0777);
  move_old_folder();
  log_init(g_root);
  if (g_moved[0])
    debugPrintf("[boot] the game folder is %s now: %s\n", g_root, g_moved);
  if (dcr_is_emulator()) {
    int dcr_emu_fix_self(void); /* emu_fixups.c: before any of Mesa runs */
    dcr_emu_fix_self();
  }
  log_console_open(); /* blank: text only when asked or for setup work */
  report_boot();

  if (chdir(g_root) != 0)
    debugPrintf("[boot] WARNING: chdir(%s) failed\n", g_root);
  dcr_config_load(); /* config.ini: controls, display, boost */
  void dcr_boost_launch_begin(void);
  dcr_boost_launch_begin(); /* CPU at 1785 MHz until the first picture (dcr_boost.c) */
  if (dcr_config()->boot_log)
    log_console_show_text();
  dcr_time_init();
  dcr_path_prepare_dirs();

  /* A newer build of this program in the launcher NRO: install it and
   * restart into it before anything else happens (dcr_setup.c). */
  dcr_setup_update_from_nro();

  char apk[512];
  find_apk(apk, sizeof apk);
  if (!apk[0])
    fatal_error("There is no APK of Labyrinth 2 in %s.\n\n"
                "Copy the APK of your own Labyrinth 2 (se.illusionlabs.labyrinth2 1.29)\n"
                "there, under any name ending in .apk: the game's library, levels,\n"
                "pictures and sounds come from it.",
                g_root);
  snprintf(g_apk, sizeof g_apk, "%s", apk);
  void dcr_apkcache_set_path(const char *real);
  dcr_apkcache_set_path(apk); /* the asset reads of it are cached (dcr_apkcache.c) */
  if (dcr_manifest_load(apk) != 0)
    fatal_error("%s is unreadable.\n\nCopy the APK of your own Labyrinth 2 there again.", apk);
  debugPrintf("[boot] the APK: %s, %s %s (%d)\n", strrchr(apk, '/') + 1, dcr_manifest_package(),
              dcr_manifest_version_name(), dcr_manifest_version_code());

  if (dcr_self_process() == INVALID_HANDLE)
    fatal_error("Could not obtain a handle to this process.\n"
                "The loader needs it to map the game's code.");

  /* liblabyrinthii.so and classes.txt, from game.apk when they are missing
   * or it has changed */
  dcr_setup_from_apk(apk);
  if (lab_apk_init(apk) != 0)
    fatal_error("Could not read the game's files from %s.\n\n"
                "Is it the APK of Labyrinth 2 (it must hold assets/ and res/raw/)?", apk);
  /* the iPad game's files: the player's own .ipa if there is one, else the
   * part of it the launcher NRO carries -- the iPad menus, its level packs
   * (lab_hd*.c, lab_files.c) */
  {
    char ipa[512] = "";
    snprintf(ipa, sizeof ipa, "%s/game.ipa", g_root);
    FILE *t = fopen(ipa, "rb");
    if (t)
      fclose(t);
    else {
      ipa[0] = 0;
      DIR *d = opendir(g_root);
      struct dirent *de;
      while (d && (de = readdir(d))) {
        if (has_ext(de->d_name, ".ipa")) {
          snprintf(ipa, sizeof ipa, "%s/%s", g_root, de->d_name);
          break;
        }
      }
      if (d)
        closedir(d);
    }
    /* none of the player's own: the iPad files the NRO carries */
    if (!ipa[0] && dcr_setup_ipad_from_nro(ipa, sizeof ipa))
      debugPrintf("[boot] the iPad game's files: the NRO's (%s)\n", ipa);
    if (ipa[0])
      lab_ipa_init(ipa);
    else
      debugPrintf("[boot] no .ipa, and no NRO here with the iPad files: the Android game's menus\n");
  }
  /* the saves (the game's registry and level table), then the engine's
   * files dir: assets and official level packs, the SD card's packs */
  lab_reg_load();
  lab_levels_load();
  lab_files_setup();
  int dcr_setup_did_work(void);
  void dcr_setup_progress(const char *what, int permille);
  if (dcr_setup_did_work())
    dcr_setup_progress("Starting the game", 1000);
  if (lab_load_module() != 0)
    fatal_error("Could not load the game library from %s.\n\n"
                "It is unpacked from the APK (" LAB_ABI_DIR ") on launch: delete\n"
                LAB_LIB_GAME " and .setup there to unpack it again.",
                g_root);

  /* The main thread becomes a guest thread like the game's own: priority
   * 59 on cores 0-2, where the kernel time-slices (dcr_sched.c). */
  dcr_sched_init();
  {
    lab_audio_selftest();
    void dcr_pthread_selftest(void);
    dcr_pthread_selftest();
    void dcr_io_selftest(void);
    dcr_io_selftest();
  }
#if DCR_GL_MESA
  if (dcr_is_emulator() || dcr_config()->gl_selftest) {
    int dcr_gl_selftest(void);
    dcr_gl_selftest();
  }
#endif

  /* System.loadLibrary: the engine's constructors, then (lab_boot.c) the
   * managers' natives and the menus. */
  lab_run_constructors();
  lab_boot_run();
  debugPrintf("[boot] exiting\n");
  log_flush_ring();
  return 0;
}
