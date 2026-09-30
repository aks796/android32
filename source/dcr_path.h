/* dcr_path.h -- Android path -> SD-card path translation, and the game
 * folder's and the APK's paths (dcr_path.c). MIT. */
#ifndef DCR_PATH_H
#define DCR_PATH_H
#include <stddef.h>

#include "rt_settings.h"

#define DCR_PATH_MAX 768

/* Returns `path` itself when no translation applies, else `out`. */
const char *dcr_translate_path(const char *path, char *out, size_t cap);
/* mkdir -p for the folders an Android install would have. */
void dcr_path_prepare_dirs(void);

/* The game folder: "sdmc:" PORT_ROOT_PATH. dcr_set_game_root() is for tests
 * and tools, before the first file operation. */
const char *dcr_game_root(void);
void dcr_set_game_root(const char *root);
/* <game folder>/data: the app's internal storage (/data/data/<pkg>). */
const char *dcr_data_dir(void);

/* The player's APK in the game folder, found at boot (any name); "" before.
 * The Android path of the app's APK (/data/app/<pkg>-1/base.apk) leads here. */
const char *dcr_apk_path(void);
void dcr_set_apk_path(const char *path);

/* The game's working directory (its chdir(); bionic_io.c): relative paths
 * are resolved against it (RT_PATH_CWD_RELATIVE). */
const char *dcr_cwd(void);

/* Worth logging (limited)? Weak: a port may define its own. */
int dcr_path_traced(const char *p);

/* CALLBACK: the last step of dcr_translate_path(): `real` is the translated
 * path (it may be `out` itself); return it, or `out` with another path.
 * Default: `real`. (lab2: the iPad resource folder's fall-back to data/files.) */
const char *port_path_fixup(const char *real, char *out, size_t cap);

#define DCR_PKG_NAME PORT_PACKAGE
#define DCR_ANDROID_FILES "/data/data/" DCR_PKG_NAME "/files"
#define DCR_ANDROID_CACHE "/data/data/" DCR_PKG_NAME "/cache"
#define DCR_ANDROID_EXT_FILES "/storage/emulated/0/Android/data/" DCR_PKG_NAME "/files"
#define DCR_ANDROID_APK "/data/app/" DCR_PKG_NAME "-1/base.apk"

#endif
