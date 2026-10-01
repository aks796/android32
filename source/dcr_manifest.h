/* dcr_manifest.h -- package facts read from the user's APK (see dcr_manifest.c). */
#ifndef DCR_MANIFEST_H
#define DCR_MANIFEST_H
#include <stdint.h>

enum { DCR_META_STRING, DCR_META_INT, DCR_META_BOOL, DCR_META_FLOAT };

typedef struct {
  char name[96];
  int type;
  int32_t i;     /* INT / BOOL (0 or -1) */
  float f;       /* FLOAT */
  char s[128];   /* STRING */
} DcrMeta;

/* Reads the APK's manifest, replacing what an earlier call read. 0 on success. */
int dcr_manifest_load(const char *apk_path);
int dcr_manifest_probe(const char *apk_path); /* the same, without log lines (the APK search) */
int dcr_manifest_loaded(void);
/* Until an APK is loaded: PORT_PACKAGE, "1.0", 1. */
const char *dcr_manifest_package(void);
const char *dcr_manifest_version_name(void);
int dcr_manifest_version_code(void);
const DcrMeta *dcr_manifest_meta(const char *name);
int dcr_manifest_meta_count(void);
const DcrMeta *dcr_manifest_meta_at(int i);

#endif
