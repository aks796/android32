/* dcr_formats.h -- readers for the files the setup and the migration look
 * into, header-only and free of libnx, so that the 32-bit game program, the
 * 64-bit launcher and host tools (a port's tools/test_setup.py) all use them:
 *   dex_names()       the classes a .dex defines, as JNI names
 *   nro_romfs_file()  a file at the top of an NRO's romfs (the launcher's
 *                     <payload>.nsp / <payload>.build)
 *   nro_build()       the game-program build an NRO carries (its romfs
 *                     PORT_PAYLOAD_NAME ".build"); nro_build_named() takes
 *                     the stem, for tools built without the port's settings
 * The payload stem is the contract between the launcher's romfs, the game
 * program's self-update and the migration: a port never renames it. MIT.
 */
#ifndef DCR_FORMATS_H
#define DCR_FORMATS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcr_exefs.h"

static inline uint32_t fmt_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct {
  char **v;
  int n, cap;
} Names;

static inline void names_add(Names *ns, const char *s, size_t len) {
  if (ns->n == ns->cap) {
    int cap = ns->cap ? ns->cap * 2 : 4096;
    char **v = realloc(ns->v, (size_t)cap * sizeof *v);
    if (!v)
      return;
    ns->v = v;
    ns->cap = cap;
  }
  char *c = malloc(len + 1);
  if (!c)
    return;
  memcpy(c, s, len);
  c[len] = 0;
  ns->v[ns->n++] = c;
}

/* The classes a .dex defines (class_defs -> type_ids -> string_ids), as JNI
 * names: "Lcom/foo/Bar$Baz;" -> "com/foo/Bar$Baz". stage_sd.py's
 * dex_class_names() in C, bounds-checked. */
static inline int dex_names(const uint8_t *d, size_t len, Names *ns) {
  if (len < 0x70 || memcmp(d, "dex\n", 4))
    return -1;
  uint32_t nstr = fmt_rd32(d + 0x38), str_off = fmt_rd32(d + 0x3C);
  uint32_t ntype = fmt_rd32(d + 0x40), type_off = fmt_rd32(d + 0x44);
  uint32_t ndef = fmt_rd32(d + 0x60), def_off = fmt_rd32(d + 0x64);
  if ((uint64_t)str_off + 4ull * nstr > len || (uint64_t)type_off + 4ull * ntype > len ||
      (uint64_t)def_off + 32ull * ndef > len)
    return -1;
  int added = 0;
  for (uint32_t k = 0; k < ndef; k++) {
    uint32_t type_idx = fmt_rd32(d + def_off + 32 * k);
    if (type_idx >= ntype)
      continue;
    uint32_t str_idx = fmt_rd32(d + type_off + 4 * type_idx);
    if (str_idx >= nstr)
      continue;
    size_t p = fmt_rd32(d + str_off + 4 * str_idx);
    while (p < len && (d[p] & 0x80)) /* the uleb128 UTF-16 length */
      p++;
    p++;
    const uint8_t *e = p < len ? memchr(d + p, 0, len - p) : NULL;
    if (!e)
      continue;
    size_t n = (size_t)(e - (d + p));
    if (n >= 3 && d[p] == 'L' && d[p + n - 1] == ';') {
      names_add(ns, (const char *)d + p + 1, n - 2);
      added++;
    }
  }
  return added;
}

/* The launcher's romfs entries: file `name` at the top of the romfs of the
 * NRO at `path`, as (offset in the file, size). 0 on success. */
static inline int nro_romfs_file(FILE *f, const char *name, long *off, size_t *size) {
  uint8_t h[0x20], aset[0x38], rh[0x50];
  if (fseek(f, 0, SEEK_SET) || fread(h, 1, sizeof h, f) != sizeof h || memcmp(h + 0x10, "NRO0", 4))
    return -1;
  uint32_t nro_size = fmt_rd32(h + 0x18);
  if (fseek(f, (long)nro_size, SEEK_SET) || fread(aset, 1, sizeof aset, f) != sizeof aset ||
      memcmp(aset, "ASET", 4))
    return -1;
  uint64_t romfs = exefs_rd64(aset + 0x28), romfs_size = exefs_rd64(aset + 0x30);
  long base = (long)nro_size + (long)romfs;
  if (!romfs_size || fseek(f, base, SEEK_SET) || fread(rh, 1, sizeof rh, f) != sizeof rh)
    return -1;
  uint64_t ftab = exefs_rd64(rh + 0x38), ftab_size = exefs_rd64(rh + 0x40), data = exefs_rd64(rh + 0x48);
  if (ftab_size > (1u << 20))
    return -1;
  uint8_t *t = malloc((size_t)ftab_size);
  if (!t)
    return -1;
  int rc = -1;
  if (fseek(f, base + (long)ftab, SEEK_SET) == 0 && fread(t, 1, (size_t)ftab_size, f) == ftab_size) {
    size_t want = strlen(name);
    for (size_t p = 0; p + 0x20 <= ftab_size;) {
      uint32_t parent = fmt_rd32(t + p), nlen = fmt_rd32(t + p + 0x1C);
      if (p + 0x20 + nlen > ftab_size)
        break;
      if (parent == 0 && nlen == want && !memcmp(t + p + 0x20, name, want)) {
        *off = base + (long)data + (long)exefs_rd64(t + p + 8);
        *size = (size_t)exefs_rd64(t + p + 0x10);
        rc = 0;
        break;
      }
      p += 0x20 + ((nlen + 3) & ~3u);
    }
  }
  free(t);
  return rc;
}

/* The build number in romfs:/<stem>.build of the NRO in f; 0 if none. */
static inline uint64_t nro_build_named(FILE *f, const char *stem) {
  long off;
  size_t size;
  char name[96], txt[32] = {0};
  snprintf(name, sizeof name, "%s.build", stem);
  if (nro_romfs_file(f, name, &off, &size) || size >= sizeof txt || fseek(f, off, SEEK_SET) ||
      fread(txt, 1, size, f) != size)
    return 0;
  return strtoull(txt, NULL, 10);
}

/* PORT_PAYLOAD_NAME: set by runtime.mk and launcher.mk from the 32-bit
 * program's name (the wrapper Makefile's TARGET). */
#ifdef PORT_PAYLOAD_NAME
static inline uint64_t nro_build(FILE *f) { return nro_build_named(f, PORT_PAYLOAD_NAME); }
#endif

#endif
