/* dcr_dircache.c -- "does this file exist?" from directory listings.
 *
 * Resource systems look most files up in the app's files directory first
 * and only then in the APK. Nearly all of those files exist only in the APK,
 * so a load is thousands of fopens of files that are not there -- and on the
 * Switch each is a file-system open (an IPC to fs-srv, on the SD card). In
 * PvZ Touch the loading thread sat in fsFsOpenFile for ~90% of a 184 s load
 * (hardware run 7).
 *
 * So, for paths in the emulated app dirs (<game>/data/..., <game>/external/...)
 * only: the first question about a directory lists it once (opendir), and a
 * read-only open, stat or access of a name that is not in the listing fails
 * at once with ENOENT, without the file system. A directory whose parent's
 * listing lacks it is missing without being listed at all. Names compare
 * case-insensitively, as FAT does, so nothing the card would find is ever
 * called missing. Anything that creates, deletes or renames a file or a
 * directory through the bionic shims forgets everything (rare: saves, the
 * font cache, logs). When in doubt (out of scope, "..", a listing that
 * fails, a full table) the answer is "ask the file system".
 *
 * The scope is fixed on the first question, so the game folder
 * (dcr_game_root()) must be final before the first file operation. MIT.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_dircache.h"
#include "dcr_path.h"
#include "rt_settings.h"
#include "util.h"

/* 1: answer misses from the listings; 0: always ask the file system. dcr 0
 * (Mono probes and writes many folders under data/, and dcr's own bionic_io
 * lacks the forget-on-create hooks: not verified there); the others 1. */
#ifndef RT_DIRCACHE
#define RT_DIRCACHE 1
#endif

#define MAX_DIRS 256

typedef struct {
  char *path;    /* normalized, lower case */
  int missing;   /* the directory itself does not exist */
  char **names;  /* its entries, lower case */
  int n;
} Dir;

static Dir g_dirs[MAX_DIRS];
static int g_ndirs;
static Mutex g_lock;
static unsigned long g_answered, g_listed;

static void lower(char *s) {
  for (; *s; s++)
    *s = (char)tolower((unsigned char)*s);
}

/* out: `real` with repeated '/' collapsed and "/." removed, lower case.
 * 0 if it has ".." or does not fit. */
static int normalize(const char *real, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = real; *p; p++) {
    if (*p == '/' && n && out[n - 1] == '/')
      continue;
    if (*p == '/' && p[1] == '.' && (p[2] == '/' || !p[2])) {
      p++;
      continue;
    }
    if (*p == '.' && p[1] == '.' && n && out[n - 1] == '/')
      return 0;
    if (n + 1 >= cap)
      return 0;
    out[n++] = *p;
  }
  while (n > 1 && out[n - 1] == '/')
    n--;
  out[n] = 0;
  lower(out);
  return 1;
}

static int in_scope(const char *norm) {
  static char data[320], ext[320];
  if (!data[0]) {
    snprintf(data, sizeof data, "%s/", dcr_data_dir());
    snprintf(ext, sizeof ext, "%s/external/", dcr_game_root());
    lower(data);
    lower(ext);
  }
  return !strncmp(norm, data, strlen(data)) || !strncmp(norm, ext, strlen(ext));
}

static int has_name(const Dir *d, const char *name) {
  for (int i = 0; i < d->n; i++)
    if (!strcmp(d->names[i], name))
      return 1;
  return 0;
}

static Dir *find(const char *norm) {
  for (int i = 0; i < g_ndirs; i++)
    if (!strcmp(g_dirs[i].path, norm))
      return &g_dirs[i];
  return NULL;
}

/* The directory `norm` (in scope), listed or known missing; NULL: unknown. */
static Dir *get_dir(const char *norm) {
  Dir *d = find(norm);
  if (d)
    return d;
  if (g_ndirs == MAX_DIRS)
    return NULL;
  char parent[320];
  snprintf(parent, sizeof parent, "%s", norm);
  char *slash = strrchr(parent, '/');
  int missing = 0;
  if (slash && slash > parent) {
    *slash = 0;
    char probe[330]; /* the parent is itself inside data/ or external/ */
    snprintf(probe, sizeof probe, "%s/", parent);
    if (in_scope(probe)) {
      Dir *p = get_dir(parent);
      if (p && (p->missing || !has_name(p, slash + 1)))
        missing = 1;
    }
  }
  d = &g_dirs[g_ndirs];
  memset(d, 0, sizeof *d);
  if (!missing) {
    DIR *dir = opendir(norm);
    if (!dir) {
      if (errno != ENOENT && errno != ENOTDIR)
        return NULL; /* not a clear "no": ask the file system each time */
      missing = 1;
    } else {
      int cap = 0;
      struct dirent *e;
      while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        if (d->n == cap) {
          cap = cap ? cap * 2 : 16;
          char **nn = realloc(d->names, sizeof *nn * (size_t)cap);
          if (!nn) {
            closedir(dir);
            for (int i = 0; i < d->n; i++)
              free(d->names[i]);
            free(d->names);
            return NULL;
          }
          d->names = nn;
        }
        char *s = strdup(e->d_name);
        if (!s)
          continue;
        lower(s);
        d->names[d->n++] = s;
      }
      closedir(dir);
      g_listed++;
    }
  }
  d->path = strdup(norm);
  if (!d->path) {
    for (int i = 0; i < d->n; i++)
      free(d->names[i]);
    free(d->names);
    return NULL;
  }
  d->missing = missing;
  g_ndirs++;
  return d;
}

/* 1: `real` certainly does not exist (answer ENOENT); 0: ask the file system. */
int dcr_dircache_missing(const char *real) {
  if (!RT_DIRCACHE)
    return 0;
  char norm[320];
  if (!real || !normalize(real, norm, sizeof norm) || !in_scope(norm))
    return 0;
  char *slash = strrchr(norm, '/');
  if (!slash || !slash[1])
    return 0;
  *slash = 0;
  mutexLock(&g_lock);
  Dir *d = get_dir(norm);
  int r = d && (d->missing || !has_name(d, slash + 1));
  if (r)
    g_answered++;
  mutexUnlock(&g_lock);
  return r;
}

/* Something was created, deleted or renamed: forget every listing. */
void dcr_dircache_forget(void) {
  mutexLock(&g_lock);
  for (int i = 0; i < g_ndirs; i++) {
    for (int k = 0; k < g_dirs[i].n; k++)
      free(g_dirs[i].names[k]);
    free(g_dirs[i].names);
    free(g_dirs[i].path);
  }
  g_ndirs = 0;
  mutexUnlock(&g_lock);
}

void dcr_dircache_report(void) {
  if (g_listed || g_answered)
    debugPrintf("[io] directory index: %lu listings, %lu missing files answered without the card\n",
                g_listed, g_answered);
}
