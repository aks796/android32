/* rt_migrate.h -- an older install's game folder, moved into this one
 * (rt_migrate.c). Free of libnx: the 32-bit game program and the 64-bit
 * launcher both run it, whichever starts first after an update. MIT.
 */
#ifndef RT_MIGRATE_H
#define RT_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

/* How a move goes. rt_migrate_policy() fills it from the RT_MIGRATE_*
 * settings; a caller may change it before rt_migrate_folder(). */
typedef struct RtMigratePolicy {
  uint64_t build;                    /* this program's build (move_newer_nro) */
  const char *skip;                  /* a file left where it is: a name, or a path (with '/'):
                                        the launcher that is running */
  const char *const *both_installed; /* NULL-ended names: in both folders = two installs, touch nothing */
  const char *once_marker;           /* a file in the new folder that says the move was done (NULL: none) */
  const char *record;                /* appended with what moved, in the new folder (NULL: none) */
  int merge;                         /* a folder both have: merged entry by entry (1) or left (0) */
  int move_newer_nro;                /* 1: an NRO carrying this build or a newer one moves too; 0: NROs stay */
  int same_size_apk;                 /* 1: an APK the new folder has a same-sized copy of stays */
} RtMigratePolicy;

/* What happened. The names are the top-level entries, as many as fit. */
typedef struct RtMigrateResult {
  int rc;                            /* as rt_migrate_folder() returned (the most notable, for several) */
  unsigned moved, kept, failed, merged, nros;
  char moved_names[256];             /* "config.ini, data, game.apk" */
  char left_names[256];              /* "Old.nro (a launcher), data (already here)" */
  char msg[800];                     /* one line for the log, the screen and the record; "" when
                                        nothing moved and nothing failed */
} RtMigrateResult;

enum {
  RT_MIGRATE_NONE = 0,   /* no old folder (or the marker says it was done) */
  RT_MIGRATE_MOVED = 1,  /* something moved, or the emptied old folder was removed */
  RT_MIGRATE_STUCK = 2,  /* the old folder is there, but nothing in it could move */
  RT_MIGRATE_BOTH = -2,  /* both folders hold an install: neither was touched (msg says so) */
};

/* The settings' policy. build: this program's build (DCR_BUILD in the game
 * program, the romfs .build in the launcher); skip: see RtMigratePolicy. */
void rt_migrate_policy(RtMigratePolicy *p, uint64_t build, const char *skip);

/* Moves old_root's entries into new_root (made if missing) by rename: the
 * same SD card, nothing is copied. Never overwrites: an entry new_root has
 * already stays in old_root (a folder both have is merged, entry by entry,
 * when pol->merge). The names are read before anything moves (FAT folders
 * change under readdir). NROs stay (forwarders point at them), but for
 * pol->move_newer_nro. The old folder is removed once it is empty. r may be
 * NULL. Returns RT_MIGRATE_*. */
int rt_migrate_folder(const char *old_root, const char *new_root, const RtMigratePolicy *pol,
                      RtMigrateResult *r);

/* Every folder in PORT_OLD_ROOT_PATHS ("sdmc:" added), into new_root, with
 * rt_migrate_policy(build, skip). r (may be NULL) sums them. */
int rt_migrate_port(const char *new_root, uint64_t build, const char *skip, RtMigrateResult *r);

/* abs's call (dcr_migrate.c), kept for ports that make it: rt_migrate_folder
 * with the settings' policy; msg: the result's msg. */
int dcr_migrate_folder(const char *old_root, const char *new_root, uint64_t build, const char *skip, char *msg,
                       size_t cap);

#endif /* RT_MIGRATE_H */
