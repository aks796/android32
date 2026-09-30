/* dcr_migrate.h -- the game folder's move from sd:/switch/sonicracing (the
 * port's first name) to sd:/switch/sonic_allstars_nx.
 *
 * On the first start after the rename, everything in the old folder -- the
 * player's APK and data, config.ini, the saves (data/), the unpacked library,
 * the logs -- is moved into the new one. Each entry is renamed (the same SD
 * card: nothing is copied, a folder moves whole). The old launcher NRO stays
 * where it is (an existing forwarder may still point at it), and an entry the
 * new folder already has is kept there, the old one left untouched.
 * Header-only, stdio and dirent: the 64-bit launcher uses it too. MIT.
 */
#ifndef DCR_MIGRATE_H
#define DCR_MIGRATE_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DCR_OLD_DIR "sdmc:/switch/sonicracing"

typedef struct {
  int moved, kept, failed; /* entries moved, left (the new folder has them), not movable */
  char names[256];         /* the moved entries' names (the log's), as many as fit */
} DcrMigrate;

static inline int dcr_migrate_is_nro(const char *n) {
  size_t k = strlen(n);
  return k > 4 && n[k - 4] == '.' && (n[k - 3] | 32) == 'n' && (n[k - 2] | 32) == 'r' && (n[k - 1] | 32) == 'o';
}

/* Moves old_dir's entries into new_dir; returns how many were moved. */
static inline int dcr_migrate(const char *old_dir, const char *new_dir, DcrMigrate *r) {
  memset(r, 0, sizeof *r);
  DIR *d = opendir(old_dir);
  if (!d)
    return 0;
  /* the names first: renaming while readdir walks the folder can skip some */
  char(*list)[256] = (char(*)[256])malloc(512 * sizeof *list);
  int n = 0;
  struct dirent *e;
  while (list && n < 512 && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || dcr_migrate_is_nro(e->d_name))
      continue;
    snprintf(list[n++], sizeof list[0], "%s", e->d_name);
  }
  closedir(d);
  if (!list || !n) {
    free(list);
    return 0;
  }
  mkdir(new_dir, 0777);
  size_t o = 0;
  for (int i = 0; i < n; i++) {
    char src[600], dst[600];
    struct stat st;
    snprintf(src, sizeof src, "%s/%s", old_dir, list[i]);
    snprintf(dst, sizeof dst, "%s/%s", new_dir, list[i]);
    if (stat(dst, &st) == 0) {
      r->kept++;
      continue;
    }
    if (rename(src, dst) != 0) {
      r->failed++;
      continue;
    }
    r->moved++;
    const size_t len = strlen(list[i]);
    if (o + len + 3 < sizeof r->names) {
      if (o)
        r->names[o++] = ',', r->names[o++] = ' ';
      memcpy(r->names + o, list[i], len);
      o += len;
      r->names[o] = 0;
    }
  }
  free(list);
  return r->moved;
}

#endif /* DCR_MIGRATE_H */
