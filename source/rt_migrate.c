/* rt_migrate.c -- an older install's game folder, moved into this one.
 *
 * Ports renamed their SD folder when their NRO was renamed
 * (/switch/labyrinth2 -> /switch/labyrinth2_nx and so on: PORT_OLD_ROOT_PATHS).
 * On the first start after the update, what the old folder holds -- the
 * player's APK and data, config.ini, the saves (data/), the unpacked
 * libraries, the logs -- moves into the new one, entry by entry, by rename
 * (the same SD card: a 1.5 GB OBB moves at once; nothing is copied or
 * deleted). The rules, from the seven ports' own moves:
 *   - never overwrite: an entry the new folder has already stays in the old
 *     one; a folder both have is merged entry by entry (RT_MIGRATE_MERGE);
 *   - the names are read before anything moves: renaming while readdir walks
 *     a FAT folder can skip entries;
 *   - NROs stay: a forwarder may point at the old launcher. With
 *     RT_MIGRATE_MOVE_NEWER_NRO an NRO carrying this build or a newer one
 *     comes along (a new launcher dropped into the old folder), and the
 *     running launcher is never moved;
 *   - an APK the new folder has a same-sized copy of stays (a player who
 *     copied it in again: no second copy of hundreds of MB);
 *   - both folders holding an install (RT_MIGRATE_BOTH_INSTALLED): nothing
 *     is touched, and the message says so;
 *   - the old folder is removed once it is empty.
 * What moved is appended to <new folder>/migrated.txt (RT_MIGRATE_RECORD):
 * the game program's log is not open yet when it runs, and the launcher has
 * none. Plain newlib (dirent, stdio, stat), no libnx: the 64-bit launcher
 * and the 32-bit game program build the same code; whichever runs first
 * moves, and the other finds nothing to do. MIT.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dcr_formats.h"
#include "rt_migrate.h"
#include "rt_settings.h"

/* A folder both have: merged entry by entry (1), or left in the old folder
 * (0). abs, pvz 1; dcr, lab2, sonic, flappy, a8r had 0 (they only moved
 * the top level; merging never overwrites either). */
#ifndef RT_MIGRATE_MERGE
#define RT_MIGRATE_MERGE 1
#endif

/* 1: an NRO in the old folder carrying this build or a newer one moves too
 * (a new launcher dropped there). abs 1; the others 0: every NRO stays. */
#ifndef RT_MIGRATE_MOVE_NEWER_NRO
#define RT_MIGRATE_MOVE_NEWER_NRO 0
#endif

/* 1: an APK the new folder has a same-sized copy of stays. pvz 1; harmless
 * for the others (they had none). */
#ifndef RT_MIGRATE_SAME_SIZE_APK
#define RT_MIGRATE_SAME_SIZE_APK 1
#endif

/* RT_MIGRATE_BOTH_INSTALLED (optional): names (a list of strings) that
 * mark an install; in both folders, nothing is moved. a8r "A8R.apk",
 * "gameloft"; others none. */

/* RT_MIGRATE_ONCE_MARKER (optional): a file in the new folder that says the
 * move was done; with it there, the old folder is not looked at again. pvz
 * ".moved"; others none (a second run finds nothing to move). */

/* Appended with what moved, in the new folder; "" for none. a8r had it; now
 * every port. */
#ifndef RT_MIGRATE_RECORD
#define RT_MIGRATE_RECORD "migrated.txt"
#endif

#define MAX_DEPTH 8

static int exists(const char *path, int *is_dir) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  if (is_dir)
    *is_dir = S_ISDIR(st.st_mode);
  return 1;
}

static long long size_of(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
}

static int ends_with(const char *s, const char *tail) {
  size_t n = strlen(s), t = strlen(tail);
  return n >= t && !strcasecmp(s + n - t, tail);
}

/* A path without "sdmc:" in front, for comparing. */
static const char *no_dev(const char *p) {
  const char *c = strchr(p, ':');
  return c && c < p + 8 ? c + 1 : p;
}

/* a folder's names, read whole before any is renamed away (NULL-ended; NULL
 * if it cannot be read) */
static char **list_names(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return NULL;
  char **v = NULL;
  size_t n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    if (n + 2 > cap) {
      cap = cap ? cap * 2 : 64;
      char **nv = realloc(v, cap * sizeof *nv);
      if (!nv)
        break;
      v = nv;
    }
    v[n] = strdup(e->d_name);
    if (v[n])
      n++;
  }
  closedir(d);
  if (!v)
    v = calloc(1, sizeof *v);
  else
    v[n] = NULL;
  return v;
}

static void free_names(char **v) {
  for (size_t i = 0; v && v[i]; i++)
    free(v[i]);
  free(v);
}

/* "a, b (why)" onto a list, while it fits */
static void note(char *list, size_t cap, const char *name, const char *why) {
  size_t n = strlen(list);
  if (n + strlen(name) + (why ? strlen(why) + 3 : 0) + 3 >= cap)
    return;
  snprintf(list + n, cap - n, "%s%s%s%s%s", n ? ", " : "", name, why ? " (" : "", why ? why : "", why ? ")" : "");
}

/* an APK of that size in the folder already (the same file, copied anew) */
static int same_apk_here(const char *root, long long size) {
  if (size <= 0)
    return 0;
  DIR *d = opendir(root);
  if (!d)
    return 0;
  int found = 0;
  struct dirent *e;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] == '.' || !ends_with(e->d_name, ".apk"))
      continue;
    char p[768];
    snprintf(p, sizeof p, "%s/%s", root, e->d_name);
    found = size_of(p) == size;
  }
  closedir(d);
  return found;
}

static int is_skipped(const RtMigratePolicy *pol, const char *src, const char *name) {
  if (!pol->skip || !pol->skip[0])
    return 0;
  if (strchr(pol->skip, '/'))
    return !strcasecmp(no_dev(pol->skip), no_dev(src));
  return !strcasecmp(pol->skip, name);
}

/* an NRO that stays (a launcher a forwarder may point at) */
static int nro_stays(const RtMigratePolicy *pol, const char *src) {
  if (!pol->move_newer_nro || !pol->build)
    return 1; /* (build 0: this program's is not known) */
  FILE *f = fopen(src, "rb");
  uint64_t b = f ? nro_build(f) : 0;
  if (f)
    fclose(f);
  return !b || b < pol->build; /* an older launcher, or not this game's */
}

static void move_into(const char *from, const char *to, int top, const RtMigratePolicy *pol, RtMigrateResult *r,
                      int depth) {
  char **names = list_names(from);
  for (size_t i = 0; names && names[i]; i++) {
    const char *nm = names[i];
    char src[768], dst[768];
    snprintf(src, sizeof src, "%s/%s", from, nm);
    snprintf(dst, sizeof dst, "%s/%s", to, nm);
    if (top && is_skipped(pol, src, nm)) {
      r->kept++;
      note(r->left_names, sizeof r->left_names, nm, "running");
      continue;
    }
    if (top && ends_with(nm, ".nro") && nro_stays(pol, src)) {
      r->nros++;
      note(r->left_names, sizeof r->left_names, nm, "a launcher");
      continue;
    }
    int src_dir = 0, dst_dir = 0;
    exists(src, &src_dir);
    if (top && pol->same_size_apk && !src_dir && nm[0] != '.' && ends_with(nm, ".apk") &&
        same_apk_here(to, size_of(src))) {
      r->kept++;
      note(r->left_names, sizeof r->left_names, nm, "a copy is here");
      continue;
    }
    if (exists(dst, &dst_dir)) {
      if (pol->merge && src_dir && dst_dir && depth < MAX_DEPTH) {
        unsigned before = r->moved;
        r->merged++;
        move_into(src, dst, 0, pol, r, depth + 1);
        rmdir(src); /* if it is empty now */
        if (top && r->moved > before)
          note(r->moved_names, sizeof r->moved_names, nm, "merged");
      } else {
        r->kept++;
        note(r->left_names, sizeof r->left_names, nm, "already here");
      }
      continue;
    }
    if (rename(src, dst) == 0) {
      r->moved++;
      if (top)
        note(r->moved_names, sizeof r->moved_names, nm, NULL);
    } else {
      r->failed++;
      note(r->left_names, sizeof r->left_names, nm, "could not move");
    }
  }
  free_names(names);
}

static int any_there(const char *root, const char *const *names) {
  for (int i = 0; names && names[i]; i++) {
    char p[768];
    snprintf(p, sizeof p, "%s/%s", root, names[i]);
    if (exists(p, NULL))
      return 1;
  }
  return 0;
}

static void append_file(const char *dir, const char *name, const char *line) {
  if (!name || !name[0] || !line[0])
    return;
  char p[768];
  snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "a");
  if (f) {
    fprintf(f, "%s\n", line);
    fclose(f);
  }
}

void rt_migrate_policy(RtMigratePolicy *p, uint64_t build, const char *skip) {
#ifdef RT_MIGRATE_BOTH_INSTALLED
  static const char *const both[] = {RT_MIGRATE_BOTH_INSTALLED, NULL};
#else
  static const char *const both[] = {NULL};
#endif
  memset(p, 0, sizeof *p);
  p->build = build;
  p->skip = skip;
  p->both_installed = both;
#ifdef RT_MIGRATE_ONCE_MARKER
  p->once_marker = RT_MIGRATE_ONCE_MARKER;
#endif
  p->record = RT_MIGRATE_RECORD;
  p->merge = RT_MIGRATE_MERGE;
  p->move_newer_nro = RT_MIGRATE_MOVE_NEWER_NRO;
  p->same_size_apk = RT_MIGRATE_SAME_SIZE_APK;
}

int rt_migrate_folder(const char *old_root, const char *new_root, const RtMigratePolicy *pol,
                      RtMigrateResult *res) {
  RtMigrateResult scratch;
  RtMigrateResult *r = res ? res : &scratch;
  memset(r, 0, sizeof *r);
  int old_dir = 0;
  if (!exists(old_root, &old_dir) || !old_dir || !strcasecmp(no_dev(old_root), no_dev(new_root)))
    return r->rc = RT_MIGRATE_NONE;
  if (pol->once_marker && pol->once_marker[0]) {
    char m[768];
    snprintf(m, sizeof m, "%s/%s", new_root, pol->once_marker);
    if (exists(m, NULL))
      return r->rc = RT_MIGRATE_NONE;
  }
  if (any_there(new_root, pol->both_installed) && any_there(old_root, pol->both_installed)) {
    snprintf(r->msg, sizeof r->msg,
             "Both %s and %s hold the game: the old folder was left as it is (delete it once the new one works).",
             old_root, new_root);
    return r->rc = RT_MIGRATE_BOTH;
  }
  mkdir(new_root, 0777);
  move_into(old_root, new_root, 1, pol, r, 0);
  int gone = rmdir(old_root) == 0;
  r->rc = r->moved || gone ? RT_MIGRATE_MOVED : (r->kept || r->failed || r->nros) ? RT_MIGRATE_STUCK : RT_MIGRATE_NONE;
  if (r->moved || r->failed) {
    int n = snprintf(r->msg, sizeof r->msg, "Moved %u item%s from %s to %s", r->moved, r->moved == 1 ? "" : "s",
                     old_root, new_root);
    if (n > 0 && (size_t)n < sizeof r->msg && r->moved_names[0])
      n += snprintf(r->msg + n, sizeof r->msg - (size_t)n, ": %s", r->moved_names);
    if (n > 0 && (size_t)n < sizeof r->msg)
      n += snprintf(r->msg + n, sizeof r->msg - (size_t)n, gone ? "; the old folder is gone." : ".");
    if (n > 0 && (size_t)n < sizeof r->msg && r->left_names[0])
      snprintf(r->msg + n, sizeof r->msg - (size_t)n, " Left in the old folder: %s.", r->left_names);
    append_file(new_root, pol->record, r->msg);
  }
  if (pol->once_marker && pol->once_marker[0]) {
    char line[160];
    snprintf(line, sizeof line, "moved from %s: %u moved, %u kept there, %u failed", no_dev(old_root), r->moved,
             r->kept + r->nros, r->failed);
    append_file(new_root, pol->once_marker, line);
  }
  return r->rc;
}

/* the most notable outcome of several: both-installed, moved, stuck, none */
static int rank(int rc) {
  return rc == RT_MIGRATE_BOTH ? 3 : rc == RT_MIGRATE_MOVED ? 2 : rc == RT_MIGRATE_STUCK ? 1 : 0;
}

int rt_migrate_port(const char *new_root, uint64_t build, const char *skip, RtMigrateResult *res) {
#ifdef PORT_OLD_ROOT_PATHS
  static const char *const olds[] = {PORT_OLD_ROOT_PATHS, NULL};
#else
  static const char *const olds[] = {NULL};
#endif
  RtMigrateResult scratch, one;
  RtMigrateResult *r = res ? res : &scratch;
  memset(r, 0, sizeof *r);
  RtMigratePolicy pol;
  rt_migrate_policy(&pol, build, skip);
  for (int i = 0; olds[i]; i++) {
    char old[400];
    snprintf(old, sizeof old, "%s%s", strchr(olds[i], ':') ? "" : "sdmc:", olds[i]);
    int rc = rt_migrate_folder(old, new_root, &pol, &one);
    if (rank(rc) > rank(r->rc))
      r->rc = rc;
    r->moved += one.moved, r->kept += one.kept, r->failed += one.failed, r->merged += one.merged,
        r->nros += one.nros;
    if (one.moved_names[0])
      note(r->moved_names, sizeof r->moved_names, one.moved_names, NULL);
    if (one.left_names[0])
      note(r->left_names, sizeof r->left_names, one.left_names, NULL);
    if (one.msg[0]) {
      size_t n = strlen(r->msg);
      snprintf(r->msg + n, sizeof r->msg - n, "%s%s", n ? " " : "", one.msg);
    }
  }
  return r->rc;
}

int dcr_migrate_folder(const char *old_root, const char *new_root, uint64_t build, const char *skip, char *msg,
                       size_t cap) {
  RtMigratePolicy pol;
  RtMigrateResult r;
  rt_migrate_policy(&pol, build, skip);
  int rc = rt_migrate_folder(old_root, new_root, &pol, &r);
  if (msg && cap)
    snprintf(msg, cap, "%s", r.msg);
  return rc;
}
