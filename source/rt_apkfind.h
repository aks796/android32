/* rt_apkfind.h -- the player's APKs in the game folder, whatever they are
 * called, told apart by what is in them (rt_apkfind.c); and the zip
 * central-directory walk it uses. Free of libnx: the 32-bit game program and
 * the 64-bit launcher both use it. MIT.
 */
#ifndef RT_APKFIND_H
#define RT_APKFIND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------- zip files */
typedef struct RtZipEntry {
  char name[512];       /* NUL-terminated; cut at 511 bytes (name_len is the real length) */
  size_t name_len;
  int method;           /* 0 stored, 8 deflated */
  uint32_t crc, comp_size, size;
  uint32_t lho;         /* its local header, from the zip's start */
} RtZipEntry;

/* Called for each entry; non-zero stops the walk. */
typedef int (*RtZipEntryFn)(const RtZipEntry *e, void *ctx);

/* The central directory of the zip that starts at `base` in f and is `size`
 * bytes long (a zip stored inside another one works too). 0, or -1 when it
 * is not a zip. */
int rt_zip_walk(FILE *f, uint64_t base, uint64_t size, RtZipEntryFn cb, void *ctx);
/* The same over the whole file at path. */
int rt_zip_walk_path(const char *path, RtZipEntryFn cb, void *ctx);
/* Where e's data begins in f (its local header is read); 0 if it cannot be told. */
uint64_t rt_zip_data_offset(FILE *f, uint64_t base, const RtZipEntry *e);
/* 1: the zip at path has an entry named `entry`. */
int rt_zip_has(const char *path, const char *entry);

/* ------------------------------------------------------------- the APKs */
enum {
  RT_APK_ADOPT = 1 << 0,           /* the APK found is renamed to `name`; the one there before
                                      becomes <name>.previous, other matches <file>.unused
                                      (a newly copied APK wins over the adopted one: an update) */
  RT_APK_OPTIONAL = 1 << 1,        /* a companion: its absence is not an error */
  RT_APK_PACKAGE_PREFIX = 1 << 2,  /* `package` is a prefix ("com.disney.disneycrossyroad*") */
  RT_APK_PACKAGE_BONUS = 1 << 3,   /* the package ranks, it does not decide */
  RT_APK_HIGHEST_VERSION = 1 << 4, /* of several, the highest versionCode (before the newest file) */
  RT_APK_ANY_FILE = 1 << 5,        /* any file in the folder, not only *.apk (the port's own
                                      files -- .nro, .ini, .so, .log ... -- are never looked at) */
};

/* One kind of APK the port looks for. A file is the first role it matches. */
typedef struct RtApkRole {
  const char *what;          /* "the game", "the English source": for the log and the messages */
  const char *name;          /* the preferred name (wins ties) and the adopted name; NULL: none */
  const char *const *need;   /* zip entries that must all be there (NULL-ended; NULL: none) */
  const char *const *reject; /* zip entries that must not be there */
  const char *package;       /* the manifest's package (NULL: any); see RT_APK_PACKAGE_* */
  int version_code;          /* this versionCode ranks first (0: none) */
  int flags;                 /* RT_APK_* */
  int (*check)(const char *path); /* anything else: 1 = it is this role's (NULL: nothing) */
} RtApkRole;

/* A port without its own table (PORT_APK_ROLES) looks for this: its package,
 * PORT_APK_DEFAULT_NAME first. Expands where rt_settings.h is included. */
#define RT_APK_ROLE_DEFAULT {.what = "the game", .name = PORT_APK_DEFAULT_NAME, .package = PORT_PACKAGE}

typedef struct RtApkEnv {
  /* The manifest's package and versionCode; 0 on success. NULL (the
   * launcher): packages are not checked, and a file that matches only
   * unchecked is never adopted (renamed). */
  int (*manifest)(const char *path, char *package, size_t cap, int *version_code);
  /* debug.log lines (NULL: none) */
  void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
  int adopt; /* 1: RT_APK_ADOPT roles rename */
} RtApkEnv;

#define RT_APK_ROLES_MAX 4
#define RT_APK_PATH_MAX 768

typedef struct RtApkFound {
  char path[RT_APK_ROLES_MAX][RT_APK_PATH_MAX]; /* each role's APK; "" when none */
  int verified[RT_APK_ROLES_MAX];               /* 0: its package was not checked */
  int count[RT_APK_ROLES_MAX];                  /* files that are that role's */
  char summary[512];                            /* "a.apk (the game), b.apk (not the game)" */
  char adopted[512];                            /* what was renamed; "" for nothing */
} RtApkFound;

/* The APKs in root for each role. 0 when every role but the optional ones
 * has one; -1 otherwise (out->summary says what the folder holds). */
int rt_apk_find(const char *root, const RtApkRole *roles, int nroles, const RtApkEnv *env, RtApkFound *out);

#endif /* RT_APKFIND_H */
