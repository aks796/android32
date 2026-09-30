/* dcr_setup.h -- a first launch from nothing but the game's APK, and
 * updates from the launcher NRO (dcr_setup.c).
 *
 * The runtime does what every port's setup did alike: the game's native
 * libraries and classes.txt out of the APK (again whenever the APK changes:
 * .setup stamps), the NRO self-update, the green progress bar. The port
 * describes the rest in `port_setup_plan`: which libraries, which part of the
 * bar each stage owns, and its own steps (files of its own made from the APK,
 * copied out of the NRO, ...), each run at its point with the APK open or
 * closed. The helpers the steps need (stamps, extraction with the bar,
 * copies out of the NRO, atomic writes) are here too. MIT.
 */
#ifndef DCR_SETUP_H
#define DCR_SETUP_H

#include <stddef.h>
#include <stdint.h>

#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include <miniz/miniz.h>

/* ------------------------------------------------------------ the plan */
/* What a step is handed. */
typedef struct RtSetupCtx {
  mz_zip_archive *apk;  /* the game's APK, open; NULL in RT_STEP_AFTER_ZIP steps */
  const char *apk_path;
  int p0, p1;           /* the step's part of the bar, permille (from its row) */
} RtSetupCtx;

/* When a step runs, in dcr_setup_from_apk()'s order:
 *   the APK opened, .setup read
 *   the APK rewritten stored (plan.store_apk_prefix)
 *   RT_STEP_BEFORE_LIBS steps
 *   the libraries, classes.txt
 *   RT_STEP_WITH_ZIP steps (the APK still open)
 *   the APK closed
 *   RT_STEP_AFTER_ZIP steps
 *   .setup written, "Starting the game" (unless plan.finish_in_port) */
enum { RT_STEP_BEFORE_LIBS, RT_STEP_WITH_ZIP, RT_STEP_AFTER_ZIP };

typedef struct RtSetupStep {
  const char *name; /* for the log */
  int when;         /* RT_STEP_* */
  int p0, p1;       /* its part of the bar, permille */
  void (*run)(RtSetupCtx *ctx);
} RtSetupStep;

typedef struct RtSetupPlan {
  /* Unpacked from PORT_ABI_DIR in the APK to the game folder, stamped by
   * name (the .setup key: never rename one, or every player unpacks again). */
  const char *const *libs;
  unsigned nlibs;
  const char *libs_what;        /* the bar's text; NULL: "Unpacking the game's library/ies" */
  const char *apk_requirement;  /* after "<apk> has no <lib>." when one is missing; NULL:
                                 * PORT_TITLE, PORT_PACKAGE and PORT_ABI_DIR said */
  int libs_p0, libs_p1;         /* the bar while unpacking, by bytes */
  int classes_p0, classes_p1;   /* the bar while listing classes, by dex file */
  /* A compressed APK is rewritten with every entry stored (4-byte aligned,
   * same names and CRCs) when an entry under this prefix is compressed
   * (Crossy Road: "assets/", which Unity would inflate at every read).
   * NULL: never. The bar: store_p0..store_p1 by bytes written, then the
   * read-back check at store_p1. */
  const char *store_apk_prefix;
  int store_p0, store_p1;
  const RtSetupStep *steps;
  unsigned nsteps;
  /* 1: the port shows "Starting the game" itself (rt_setup_finish()), after
   * work of its own that continues the bar (Labyrinth 2's files). */
  int finish_in_port;
} RtSetupPlan;

/* The port defines it: const RtSetupPlan port_setup_plan = {...}; */
extern const RtSetupPlan port_setup_plan;

/* ------------------------------------------------------------- entries */
/* Everything above, for the APK at `apk` (a missing or unreadable APK is
 * left to the caller to report). */
void dcr_setup_from_apk(const char *apk);

/* The NRO self-update: when the game folder holds a launcher NRO carrying a
 * newer build than this one, the ExeFS override is rewritten from its
 * PORT_NSP_NAME and the program restarts into it (returns otherwise). */
void dcr_setup_update_from_nro(void);

/* Steps aside for the launcher: this title's ExeFS override is removed and
 * the program restarts, so the icon starts the launcher NRO again (which
 * writes the override anew after its own work: Asphalt 8's zips). `what` is
 * the bar's text. Returns only when that cannot be done: 0 not a forwarder
 * title or no override of ours, -1 the override could not be removed. */
int rt_setup_restart_into_launcher(const char *what);

/* --------------------------------------------------------- the progress */
/* The bar (and, the first time, "[setup] setting up <PORT_TITLE>" in the
 * log). A start with nothing to do shows nothing. */
void dcr_setup_progress(const char *what, int permille);
/* p0..p1 by done/total. */
void rt_setup_progress_in(const char *what, int p0, int p1, uint64_t done, uint64_t total);
/* 1 once anything was shown. */
int dcr_setup_did_work(void);
/* "Starting the game" at 1000, if anything was shown. */
void rt_setup_finish(void);

/* ----------------------------------------------------- stamps (.setup) */
/* One line per made file: "<key> <crc of its source> <size>" (16 at most,
 * keys under 32 characters). 1 and the values when `key` is recorded. */
int rt_setup_stamp_get(const char *key, unsigned long *crc, unsigned long *size);
void rt_setup_stamp_set(const char *key, unsigned long crc, unsigned long size);
/* dcr_setup_from_apk() saves them at its end; work done outside it saves
 * its own. */
void rt_setup_stamp_save(void);

/* ---------------------------------------------------------------- files */
/* <game folder>/<name> */
void rt_root_path(char *out, size_t cap, const char *name);
/* -1 unless a regular file */
long rt_file_size(const char *path);
/* The whole file, malloc'd, NUL after the end (a text file is a string; an
 * empty one an empty buffer). NULL if it cannot be read. */
uint8_t *rt_read_whole(const char *path, size_t *len);
/* Through <dst>.part, its folders made: 1 when written. */
int rt_write_atomic(const char *dst, const void *buf, size_t len);
/* rt_write_atomic() unless the file holds exactly these bytes: 1 when written. */
int rt_write_if_changed(const char *dst, const void *buf, size_t len);
void rt_mkdirs(const char *dir);
void rt_mkdirs_for(const char *file);
/* CRC-32 of a file's bytes (0 if it cannot be read). */
unsigned long rt_crc_of_file(const char *path);

/* ----------------------------------------------------------------- zip */
/* Entry `idx` to `dst` through <dst>.part (its folders made), the bar moving
 * p0..p1 by bytes (no bar when `what` is NULL). 0 on success. */
int rt_setup_extract_entry(mz_zip_archive *zip, int idx, const char *dst, const char *what, int p0, int p1);
/* The stored rewrite of plan.store_apk_prefix, for a port that runs it
 * itself: `zip` is reopened on the result. 1 rewritten, 0 not needed or
 * not done (the APK is used as it is). */
int rt_setup_store_apk(mz_zip_archive *zip, const char *apk, const char *prefix, int p0, int p1);
/* 1 when the APK's AndroidManifest.xml names `package` (or a longer name
 * starting with it): binary XML, so UTF-16 as well as UTF-8. */
int rt_apk_has_package(const char *apk, const char *package);

/* ----------------------------------------------------------------- NRO */
/* The launcher NRO in the game folder with the newest build: its path, and
 * the build (0: none). */
uint64_t rt_find_nro(char *path, size_t cap);
/* The file `romfs_name` at the top of a launcher NRO's romfs, copied to
 * <game folder>/<dst> (the newest NRO that carries it; in 256 KB pieces, the
 * bar moving p0..p1 by bytes when `what` is set). It is copied again only
 * when the NRO's build or the file's size changes, as recorded in
 *   stamp_key NULL: <dst>.from, "<build> <size>" (Labyrinth 2's iPad files)
 *   stamp_key "":   nowhere: copied at every call
 *   stamp_key set:  .setup under that key, crc = the build
 * 2 copied now, 1 already there (from this NRO, or from one no longer in the
 * folder), 0 no NRO carries it, -1 the copy failed. */
int rt_setup_copy_from_nro(const char *romfs_name, const char *dst, const char *stamp_key, const char *what,
                           int p0, int p1);

/* ------------------------------------------------------------ callbacks */
/* Weak: draws the bar. The runtime's: log_console_progress() and
 * log_console_update(). Asphalt 8 draws it with GL while its start screen
 * holds the window. */
void port_setup_progress_draw(const char *what, int permille);
/* Weak: a library the port provides itself, not from the APK (PvZ's own
 * build of its mod). `apk_crc` is the APK's copy's CRC. 1 when handled
 * (installed or kept); 0: unpack it from the APK. The runtime's: 0. */
int port_setup_lib_override(const char *lib, unsigned long apk_crc, RtSetupCtx *ctx);

#endif /* DCR_SETUP_H */
