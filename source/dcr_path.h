/* dcr_path.h -- Android path -> SD-card path translation. */
#ifndef DCR_PATH_H
#define DCR_PATH_H
#include <stddef.h>

#define DCR_PATH_MAX 768

/* Returns `path` itself when no translation applies, else `out`. */
const char *dcr_translate_path(const char *path, char *out, size_t cap);
void dcr_path_prepare_dirs(void);

#include "config.h"
#define DCR_PKG_NAME DCR_PACKAGE
#define DCR_ANDROID_FILES "/data/data/" DCR_PKG_NAME "/files"
#define DCR_ANDROID_CACHE "/data/data/" DCR_PKG_NAME "/cache"
#define DCR_ANDROID_EXT_FILES "/storage/emulated/0/Android/data/" DCR_PKG_NAME "/files"
#define DCR_ANDROID_APK "/data/app/" DCR_PKG_NAME "-1/base.apk"

int dcr_path_traced(const char *p); /* worth logging (StreamingAssets, APK)? */

#endif
