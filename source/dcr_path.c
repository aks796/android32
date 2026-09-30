/* dcr_path.c -- turn the paths an Android build of the game asks for into
 * paths on the Switch SD card, and hold the game folder's and the APK's
 * paths.
 *
 * The rules, in the order they are tried (the order is behaviour: the
 * package's own folders must win over the generic shared-storage rule):
 *
 *   file://...   jar:file://...base.apk!/assets/...    (prefixes stripped first:
 *                                  the jar forms hold "/assets" as a substring
 *                                  too, so the "!/" anchor is taken before any
 *                                  /assets test)
 *   <root>/...  (with or without "sdmc:")              already ours
 *   /assets/...   assets/...   ./assets/...            (device/relative forms)
 *        -> <root>/assets/...
 *   anything relative                                  (RT_PATH_CWD_RELATIVE)
 *        -> the game's working directory (dcr_cwd(), its chdir(); it starts
 *           in /data/data/<pkg>/files), then these rules again
 *   /data/data/<pkg>/...   /data/user/0/<pkg>/...       (internal storage)
 *        -> <root>/data/...
 *   <shared storage>/Android/data/<pkg>/...            (external files)
 *        -> <root>/external/...
 *   <shared storage>/Android/obb/<pkg>/...             (expansion files)
 *        -> <root>/obb/...
 *   <shared storage>/<name>/...  for each name in RT_PATH_SD_SUBDIRS
 *        -> <root>/<name>/...
 *   <shared storage>/...                               (anything else there)
 *        -> <root>/sdcard/...    never the SD card's root, where newlib
 *                                would put /sdcard/... (RT_PATH_SDCARD_DIR)
 *   /data/app/<pkg>-1/lib/arm/libfoo.so                (native lib dir)
 *        -> <root>/libfoo.so
 *   /data/app/<pkg>-1/base.apk                         (ApplicationInfo.sourceDir)
 *        -> the player's own APK, whatever it is called (dcr_apk_path())
 *   then port_path_fixup(), the port's last word
 *
 * <shared storage> is any of /storage/emulated/0, /storage/emulated/legacy,
 * /storage/sdcard0, /sdcard, /mnt/sdcard, /mnt/extSdCard. Paths that match
 * nothing are returned unchanged. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dcr_path.h"
#include "rt_settings.h"
#include "util.h"

/* Relative paths are resolved against the game's working directory, as on
 * Android (the engine chdir()s into its files directory and saves in
 * "userdata/..."). 0: they keep the old rules (a relative assets/... is
 * <root>/assets/..., anything else is left to newlib, relative to <root>).
 * dcr 0 (Unity: not verified with the cwd rule); the others 1. */
#ifndef RT_PATH_CWD_RELATIVE
#define RT_PATH_CWD_RELATIVE 1
#endif

/* The folder in <root> that other shared storage goes to; "" turns the rule
 * off (such paths are left unchanged). All ports "sdcard". */
#ifndef RT_PATH_SDCARD_DIR
#define RT_PATH_SDCARD_DIR "sdcard"
#endif

/* RT_PATH_SD_SUBDIRS (optional): folders in shared storage that are the
 * game's own, as a list of strings: <shared storage>/<name>/... ->
 * <root>/<name>/.... a8r "gameloft" (the mod's data folder); others none. */

/* RT_PATH_EXTRA_DIRS (optional): more folders dcr_path_prepare_dirs() makes
 * in <root>, as a list of strings. a8r "data/databases", "sdcard",
 * "gameloft", "gameloft/games", "gameloft/games/GloftA8HP"; others none. */

/* What dcr_path_traced() logs besides the APK. lab2/abs/pvz/sonic/flappy
 * "main.pak" (inherited from pvz); a8r ".obb"; dcr overrides the function. */
#ifndef RT_PATH_TRACE_EXTRA
#define RT_PATH_TRACE_EXTRA "main.pak"
#endif

#define PKG PORT_PACKAGE

/* ------------------------------------------------------------- the folders */
static char g_root[256] = "sdmc:" PORT_ROOT_PATH;
static char g_data[300];
static char g_apk[DCR_PATH_MAX];

const char *dcr_game_root(void) { return g_root; }

void dcr_set_game_root(const char *root) {
  snprintf(g_root, sizeof g_root, "%s", root);
  g_data[0] = 0;
}

const char *dcr_data_dir(void) {
  if (!g_data[0])
    snprintf(g_data, sizeof g_data, "%s/data", g_root);
  return g_data;
}

const char *dcr_apk_path(void) { return g_apk; }
void dcr_set_apk_path(const char *path) { snprintf(g_apk, sizeof g_apk, "%s", path ? path : ""); }

/* bionic_io.c has the real one (the game's chdir()); this is for a build
 * without it. */
__attribute__((weak)) const char *dcr_cwd(void) { return DCR_ANDROID_FILES; }

__attribute__((weak)) const char *port_path_fixup(const char *real, char *out, size_t cap) {
  (void)out, (void)cap;
  return real;
}

/* ------------------------------------------------------------- the rules */
#define SD_LIST(tail)                                                                         \
  "/storage/emulated/0" tail, "/storage/emulated/legacy" tail, "/storage/sdcard0" tail,       \
      "/sdcard" tail, "/mnt/sdcard" tail, "/mnt/extSdCard" tail
static const char *const k_sd[] = {SD_LIST("")};
static const char *const k_sd_ext[] = {SD_LIST("/Android/data/" PKG)};
static const char *const k_sd_obb[] = {SD_LIST("/Android/obb/" PKG)};
#define N_SD (sizeof k_sd / sizeof k_sd[0])

#ifdef RT_PATH_SD_SUBDIRS
static const char *const k_sd_subdirs[] = {RT_PATH_SD_SUBDIRS, NULL};
#else
static const char *const k_sd_subdirs[] = {NULL};
#endif

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* `p` begins with `pre` followed by a separator or end -> the tail, else NULL. */
static const char *after_prefix(const char *p, const char *pre) {
  size_t n = strlen(pre);
  if (!n || strncmp(p, pre, n) != 0)
    return NULL;
  const char *t = p + n;
  if (is_sep(*t))
    return t + 1;
  if (!*t)
    return "";
  return NULL;
}

static const char *after_any(const char *p, const char *const *pre, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char *t = after_prefix(p, pre[i]);
    if (t)
      return t;
  }
  return NULL;
}

/* Collapse duplicate separators and turn '\' into '/'. */
static void clean(const char *in, char *out, size_t cap) {
  size_t o = 0;
  int prev_sep = 0;
  for (; *in && o + 1 < cap; in++) {
    char c = *in == '\\' ? '/' : *in;
    if (c == '/') {
      if (prev_sep)
        continue;
      prev_sep = 1;
    } else {
      prev_sep = 0;
    }
    out[o++] = c;
  }
  out[o] = 0;
}

static const char *join(char *out, size_t cap, const char *sub, const char *tail) {
  char t[DCR_PATH_MAX];
  clean(tail, t, sizeof t);
  if (t[0])
    snprintf(out, cap, "%s/%s/%s", dcr_game_root(), sub, t);
  else
    snprintf(out, cap, "%s/%s", dcr_game_root(), sub);
  return out;
}

/* Paths worth tracing (limited): the APK, and the port's extra (a native
 * file-system probe, the OBB). */
__attribute__((weak)) int dcr_path_traced(const char *p) {
  return p && (strstr(p, ".apk") || (RT_PATH_TRACE_EXTRA[0] && strstr(p, RT_PATH_TRACE_EXTRA)));
}

static const char *translate(const char *path, char *out, size_t cap);

const char *dcr_translate_path(const char *path, char *out, size_t cap) {
  const char *r = translate(path, out, cap);
  if (r && out && cap)
    r = port_path_fixup(r, out, cap);
  static int logged;
  if (dcr_path_traced(path) && logged < 64) {
    logged++;
    debugPrintf("[path] %s -> %s\n", path, r);
  }
  return r;
}

static const char *translate_abs(const char *path, char *out, size_t cap);

static const char *translate(const char *path, char *out, size_t cap) {
  if (!path || !out || !cap)
    return path;
#if RT_PATH_CWD_RELATIVE
  if (!path[0] || path[0] == '/' || strchr(path, ':'))
    return translate_abs(path, out, cap);
  char rel[DCR_PATH_MAX];
  const char *t = path;
  while (t[0] == '.' && t[1] == '/')
    t += 2;
  snprintf(rel, sizeof rel, "%s/%s", dcr_cwd(), t);
  const char *r = translate_abs(rel, out, cap);
  if (r != out) {
    snprintf(out, cap, "%s", r);
    r = out;
  }
  return r;
#else
  return translate_abs(path, out, cap);
#endif
}

static const char *translate_abs(const char *path, char *out, size_t cap) {
  const char *p = path;
  while (!strncmp(p, "file://", 7))
    p += 7;
  if (!strncmp(p, "jar:file://", 11)) {
    const char *bang = strstr(p, "!/");
    if (bang)
      p = bang + 1; /* "/assets/..." */
  }

  /* <root>/... (with or without "sdmc:") is already ours. */
  const char *root = dcr_game_root();
  const char *root_nodev = strchr(root, ':') ? strchr(root, ':') + 1 : root;
  if (!strncmp(p, root, strlen(root)))
    return p == path ? path : (snprintf(out, cap, "%s", p), out);
  if (!strncmp(p, root_nodev, strlen(root_nodev))) {
    snprintf(out, cap, "sdmc:%s", p);
    return out;
  }

  const char *t;
  if ((t = after_prefix(p, "/assets")) || (t = after_prefix(p, "assets")) ||
      (t = after_prefix(p, "./assets")))
    return join(out, cap, "assets", t);

  if ((t = after_prefix(p, "/data/data/" PKG)) || (t = after_prefix(p, "/data/user/0/" PKG)))
    return join(out, cap, "data", t);

  if ((t = after_any(p, k_sd_ext, N_SD)))
    return join(out, cap, "external", t);

  if ((t = after_any(p, k_sd_obb, N_SD)))
    return join(out, cap, "obb", t);

  /* The rest of shared storage: newlib would put it at the SD card's root,
   * so it goes in the game folder too -- a named folder of the game's own
   * first (RT_PATH_SD_SUBDIRS), then <root>/sdcard. */
  if ((t = after_any(p, k_sd, N_SD))) {
    for (int i = 0; k_sd_subdirs[i]; i++) {
      const char *u = after_prefix(t, k_sd_subdirs[i]);
      if (u)
        return join(out, cap, k_sd_subdirs[i], u);
    }
    if (RT_PATH_SDCARD_DIR[0])
      return join(out, cap, RT_PATH_SDCARD_DIR, t);
  }

  if (!strncmp(p, "/data/app/", 10)) {
    const char *lib = strstr(p, "/lib/arm/");
    if (lib)
      return join(out, cap, ".", lib + 9);
    size_t n = strlen(p);
    if (n > 4 && !strcmp(p + n - 4, ".apk")) {
      if (dcr_apk_path()[0])
        snprintf(out, cap, "%s", dcr_apk_path());
      else
        snprintf(out, cap, "%s/" PORT_APK_DEFAULT_NAME, dcr_game_root());
      return out;
    }
  }

  if (p != path) {
    snprintf(out, cap, "%s", p);
    return out;
  }
  return path;
}

/* mkdir -p for the directories the game expects Android to have created. */
void dcr_path_prepare_dirs(void) {
  static const char *const dirs[] = {"data",     "data/files",     "data/cache",    "data/shared_prefs",
                                     "external", "external/files", "external/cache"};
#ifdef RT_PATH_EXTRA_DIRS
  static const char *const extra[] = {RT_PATH_EXTRA_DIRS, NULL};
#else
  static const char *const extra[] = {NULL};
#endif
  char p[DCR_PATH_MAX];
  for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    snprintf(p, sizeof p, "%s/%s", dcr_game_root(), dirs[i]);
    mkdir(p, 0777);
  }
  for (unsigned i = 0; extra[i]; i++) {
    snprintf(p, sizeof p, "%s/%s", dcr_game_root(), extra[i]);
    mkdir(p, 0777);
  }
}
