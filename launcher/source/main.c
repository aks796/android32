/* <PORT_NAME>.nro -- the launcher: the one file a port ships.
 *
 * The game itself is 32-bit ARM, and a 32-bit program cannot be an NRO
 * (hbloader, which runs NROs, is 64-bit). So the game program -- the wrapper,
 * <PORT_PAYLOAD_NAME>.nsp -- rides in this NRO's romfs, and is installed on
 * the console as an Atmosphere ExeFS override for the HOME-menu icon it was
 * launched from:
 *
 *   1. the user makes a sphaira forwarder for this NRO and launches it;
 *   2. this runs inside that forwarder title: it moves an older install's
 *      folder over (rt_migrate.c), checks for the player's APK by what is in
 *      it (rt_apkfind.c: the port's own role table, adopting an APK copied in
 *      under another name where the port says so) and anything else the port
 *      needs, writes /atmosphere/contents/<the forwarder's title id>/exefs.nsp
 *      (the wrapper, main.npdm retargeted to that title id: dcr_exefs.h) and
 *      restarts the title;
 *   3. Atmosphere now starts the wrapper for that icon instead of hbloader.
 *      On its first run the wrapper unpacks the game from the player's APK
 *      (dcr_setup.c); later NROs update it in place.
 *
 * The override is only ever written for a forwarder (title id 05xx...) that
 * this program is running as and that was made for this NRO (fwd_mine.h)
 * -- never for a real game or a system title, never from hbmenu, and never for
 * sphaira's or hbmenu's own icon when this is opened from inside them.
 *
 * Everything that names the game comes from the port's port_config.h
 * (PORT_TITLE, PORT_ROOT_PATH, PORT_PAYLOAD_NAME, PORT_APK_ROLES...); what
 * it does besides comes from the weak port_launcher_*() hooks (launcher.h).
 * MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_exefs.h"
#include "fwd_mine.h"
#include "launcher.h"
#include "rt_apkfind.h"
#include "rt_migrate.h"
#include "rt_settings.h"

/* the same defaults as the game program's (rt_boot.c) */
#ifndef PORT_APK_DESC
#define PORT_APK_DESC PORT_TITLE " (" PORT_PACKAGE ")"
#endif
#ifndef PORT_APK_ROLES
#define PORT_APK_ROLES RT_APK_ROLE_DEFAULT
#endif
/* Under "Starting <title>..." on the forwarder's first launch. */
#ifndef PORT_LAUNCHER_START_NOTE
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game from the APK)"
#endif
/* PORT_LAUNCHER_BYLINE (optional): a line under the title (flappy: who
 * made the game and the port). */
/* 1: from the forwarder icon the launcher shows the progress screen
 * (launcher_bar) and text only for anything to read; 0: text throughout.
 * a8r 1; the others 0. */
#ifndef RT_LAUNCHER_BAR
#define RT_LAUNCHER_BAR 0
#endif
/* The NRO's name in sphaira's list (the Makefile's APP_TITLE). */
#ifndef LAUNCHER_APP_TITLE
#define LAUNCHER_APP_TITLE PORT_TITLE
#endif

#define GAME_DIR "sdmc:" PORT_ROOT_PATH
#define NSP_PATH "romfs:/" PORT_NSP_NAME
#define BUILD_PATH "romfs:/" PORT_PAYLOAD_NAME ".build"

/* ------------------------------------------------------------- the hooks */
__attribute__((weak)) int port_launcher_prepare(void) { return 0; }

__attribute__((weak)) int port_launcher_check(char *status, size_t scap, char *help, size_t hcap) {
  (void)help, (void)hcap;
  if (scap)
    status[0] = 0;
  return 0;
}

__attribute__((weak)) void port_launcher_apk_note(const char *apk_path) { (void)apk_path; }

__attribute__((weak)) void port_launcher_apk_help(void) {
  printf("\nCopy the APK of your own " PORT_APK_DESC "\n"
         "(any file name) to:\n  " GAME_DIR "/\n");
}

__attribute__((weak)) void port_launcher_instructions(void) {
  printf("  1. put the APK of your own " PORT_APK_DESC "\n"
         "     (any file name) in " PORT_ROOT_PATH "\n");
}

/* at file scope: compound literals in PORT_APK_ROLES (the need lists) are
 * static there */
static const RtApkRole k_port_roles[] = {PORT_APK_ROLES};

__attribute__((weak)) const RtApkRole *port_apk_roles(int *count) {
  *count = (int)(sizeof k_port_roles / sizeof k_port_roles[0]);
  return k_port_roles;
}

/* ------------------------------------------------------------- the console */
static PadState g_pad;

static void show(void) { consoleUpdate(NULL); }

/* text to read: not while the progress screen shows */
static int text(void) { return !launcher_bar_on(); }

/* Waits for + (or the HOME menu closing us). */
static void wait_exit(void) {
  launcher_bar_off();
  printf("\nPress + to exit.\n");
  while (appletMainLoop()) {
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus)
      break;
    show();
  }
}

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  *len = b ? (size_t)n : 0;
  return b;
}

static int write_file(const char *path, const uint8_t *d, size_t len) {
  char tmp[160];
  snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return -1;
  int ok = fwrite(d, 1, len, f) == len;
  if (fclose(f) != 0)
    ok = 0;
  if (ok) {
    remove(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok)
    remove(tmp);
  return ok ? 0 : -1;
}

/* ------------------------------------------------------------- the install */
static int install(uint64_t tid) {
  size_t nsp_len = 0, out_len = 0, cur_len = 0;
  uint8_t *nsp = read_file(NSP_PATH, &nsp_len), *out = NULL;
  if (!nsp || exefs_build_override(nsp, nsp_len, tid, &out, &out_len)) {
    launcher_bar_off();
    printf("This launcher's copy of the game program is missing or damaged.\n"
           "Download " PORT_NAME ".nro again.\n");
    free(nsp);
    return -1;
  }
  free(nsp);

  char dir[96], path[128];
  snprintf(dir, sizeof dir, "sdmc:/atmosphere/contents/%016lX", tid);
  snprintf(path, sizeof path, "%s/exefs.nsp", dir);
  uint8_t *cur = read_file(path, &cur_len);
  int same = cur && cur_len == out_len && !memcmp(cur, out, out_len);
  free(cur);
  if (same) {
    /* Already installed, yet this launcher ran instead of the game. */
    launcher_bar_off();
    printf("The game program is installed for this icon (%s),\n"
           "but Atmosphere started this launcher instead of it.\n\n"
           "Update Atmosphere, then launch the icon again.\n", path);
    free(out);
    return -1;
  }
  mkdir("sdmc:/atmosphere", 0777);
  mkdir("sdmc:/atmosphere/contents", 0777);
  mkdir(dir, 0777);
  int rc = write_file(path, out, out_len);
  free(out);
  if (rc) {
    launcher_bar_off();
    printf("Could not write %s.\nIs the SD card full or read-only?\n", path);
    return -1;
  }
  if (text())
    printf("Installed the game program for this icon:\n  %s\n", path);
  return 0;
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

int main(int argc, char **argv) {
  consoleInit(NULL);
  padConfigureInput(8, HidNpadStyleSet_NpadStandard); /* + from any controller */
  padInitializeAny(&g_pad);
  Result rrc = romfsInit();

  launcher_header();
  size_t blen = 0;
  uint8_t *bnum = R_SUCCEEDED(rrc) ? read_file(BUILD_PATH, &blen) : NULL;
  printf("Game program build: %.*s\n", bnum ? (int)(blen && bnum[blen - 1] == '\n' ? blen - 1 : blen) : 7,
         bnum ? (const char *)bnum : "missing");
  uint64_t build = bnum ? strtoull((const char *)bnum, NULL, 10) : 0;
  free(bnum);

  const char *self = argc > 0 && argv[0] ? argv[0] : "";
  u64 tid = 0;
  svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
  int in_05 = appletGetAppletType() == AppletType_Application && exefs_is_forwarder_tid(tid);
  int forwarder = in_05 && fwd_is_mine(self);
#if RT_LAUNCHER_BAR
  /* From its icon this runs only when there is work (a first launch, an
   * update): the progress screen, and text again for anything to read. */
  if (forwarder)
    launcher_bar("Checking the game files", 0);
#endif

  /* An install from before the folder's rename: moved over first (never
   * this NRO while it runs). The game program does the same; whichever
   * starts first moves it. */
  static RtMigrateResult mig;
  int moved = rt_migrate_port(GAME_DIR, build, self, &mig);
  mkdir(GAME_DIR, 0777);
  if (moved == RT_MIGRATE_BOTH || mig.failed)
    launcher_bar_off();
  if (mig.msg[0] && text())
    printf("\n%s\n", mig.msg);

  if (*self && !strstr(self, PORT_ROOT_PATH "/") && text())
    printf("\nNote: this NRO is at %s.\n"
           "The game files belong in " PORT_ROOT_PATH "; keeping the NRO there\n"
           "too lets the game update itself when you replace it.\n", self);

  if (port_launcher_prepare() != 0) {
    wait_exit();
    romfsExit();
    consoleExit(NULL);
    return 0;
  }

  /* the APK, under any name: by what is in it (the game program checks its
   * package too) */
  int nroles = 0;
  const RtApkRole *roles = port_apk_roles(&nroles);
  RtApkEnv env = {NULL, NULL, 1};
  static RtApkFound apks;
  int have_apk = rt_apk_find(GAME_DIR, roles, nroles, &env, &apks) == 0;
  static char status[256], help[800];
  int have_rest = port_launcher_check(status, sizeof status, help, sizeof help) == 0;
  if (!have_apk || !have_rest)
    launcher_bar_off();
  if (apks.adopted[0] && text())
    printf("\n%s\n", apks.adopted);
  if (text()) {
    if (have_apk)
      printf("\nAPK: %s\n", base_name(apks.path[0]));
    else if (apks.summary[0])
      printf("\nAPK: MISSING (here: %s)\n", apks.summary);
    else
      printf("\nAPK: MISSING\n");
  }
  if (have_apk)
    port_launcher_apk_note(apks.path[0]);
  if (status[0] && text())
    printf("%s\n", status);

  if (!forwarder) {
    launcher_bar_off();
    if (in_05)
      printf("\nThis was opened from inside another icon (sphaira or hbmenu started\n"
             "from its own HOME-menu icon), so nothing is installed there.\n");
    printf("\nStart this from its own HOME-menu icon:\n");
    port_launcher_instructions();
    printf("  2. in sphaira: Homebrew > " LAUNCHER_APP_TITLE " > Install Forwarder\n"
           "  3. launch the new " LAUNCHER_APP_TITLE " icon on the HOME menu.\n"
           "The first launch installs the game program for that icon and starts it.\n");
    wait_exit();
  } else if (!have_apk || !have_rest) {
    launcher_bar_off();
    if (!have_apk)
      port_launcher_apk_help();
    if (!have_rest && help[0])
      printf("\n%s\n", help);
    printf("\nThen launch this icon again.\n");
    wait_exit();
  } else {
    if (launcher_bar_on())
      launcher_bar("Installing the game program for this icon", 1000);
    if (install(tid) != 0) {
      wait_exit();
    } else {
      if (launcher_bar_on())
        launcher_bar("Starting the game", 1000);
      else
        printf("\nStarting " PORT_TITLE "...\n" PORT_LAUNCHER_START_NOTE "\n");
      show();
      svcSleepThread(1500000000ll);
      romfsExit();
      Result rc = appletRestartProgram(NULL, 0);
      launcher_bar_off();
      printf("\nRestarting did not work (0x%x): close this and launch\n" PORT_TITLE " again.\n", rc);
      wait_exit();
      consoleExit(NULL);
      return 0;
    }
  }
  romfsExit();
  consoleExit(NULL);
  return 0;
}
