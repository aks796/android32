/* dcr_setup.c -- a first launch from nothing but the game's APK.
 *
 * The game folder (sdmc:PORT_ROOT_PATH) needs only the user's own APK and
 * the launcher NRO (launcher/). Everything else is made from the APK here,
 * before anything is loaded:
 *   the game's native libraries   PORT_ABI_DIR out of the APK (the port's
 *                                 plan names them)
 *   classes.txt                   the Java class names its classes*.dex
 *                                 define (jni_core.c answers FindClass with
 *                                 exactly those)
 * then the port's own steps (dcr_setup.h: port_setup_plan), and made again
 * whenever the APK changes: .setup records the CRC-32 of each source entry.
 * Files that are already right (copied by a staging tool, say) are checked
 * once and kept. An APK that compresses what the engine reads at every load
 * can be rewritten uncompressed once, first (the plan's store_apk_prefix:
 * Crossy Road's Unity data).
 *
 * UPDATES FROM THE NRO. This program runs as a forwarder title through an
 * ExeFS override, /atmosphere/contents/<title id>/exefs.nsp, which the
 * launcher wrote on its first run (it carries PORT_NSP_NAME in its romfs).
 * When the NRO in the game folder carries a NEWER build than the one running
 * (its romfs build file against DCR_BUILD), the override is rewritten from it
 * (dcr_exefs.h) and the program restarts into the new build: updating is
 * copying the new NRO over the old one. Never a downgrade, never a file this
 * program did not come from (the override must exist and name this title),
 * and never twice for the same build (.update records the attempt).
 *
 * The seven ports' copies were one file: this is it, with Labyrinth 2's
 * copy out of the NRO, Flappy Birds' and Sonic's bar that follows the bytes
 * written, Angry Birds Space's per-dex bar, Asphalt 8's file helpers and
 * hand-over to the launcher, and Crossy Road's stored-APK rewrite. MIT.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "rt_settings.h" /* first: dcr_formats.h's nro_build() needs PORT_PAYLOAD_NAME */
#include "dcr_build.h"
#include "dcr_setup.h" /* miniz */
#include "dcr_exefs.h"
#include "dcr_formats.h"
#include "dcr_path.h" /* dcr_game_root */
#include "error.h"
#include "rt_boot.h" /* rt_boot_icon_nro */
#include "util.h"

/* The port may leave the plan out (a port whose setup is all its own): the
 * runtime's then makes classes.txt only. */
extern const RtSetupPlan port_setup_plan __attribute__((weak));
static const RtSetupPlan k_no_plan = {.classes_p0 = 750, .classes_p1 = 950};

/* Where the bar stands while an update from the NRO (or a hand-over to the
 * launcher) restarts the program: dcr lab2 abs pvz flappy 1000, sonic a8r 500. */
#ifndef RT_SETUP_UPDATE_PERMILLE
#define RT_SETUP_UPDATE_PERMILLE 1000
#endif

#define RT_SETUP_MAX_LIBS 8

#ifndef RT_SETUP_APK_REQUIREMENT
#define RT_SETUP_APK_REQUIREMENT                                         \
  "This port needs " PORT_TITLE " (" PORT_PACKAGE ") for\n"               \
  "32-bit ARM (" PORT_ABI_DIR "): use the APK of your own copy."
#endif

void rt_root_path(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s/%s", dcr_game_root(), name);
}

static const char *base_name(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* ------------------------------------------------------------ progress */
/* Setup work shows on screen as the PvZ Touch port's green progress bar
 * (util.c log_console_progress): the name, what is being done, the bar. The
 * log goes to debug.log only -- or scrolls on screen instead with config.ini
 * [debug] boot_log_on_screen. A start with nothing to do shows nothing. */
static int g_setup_shown, g_in_setup;

__attribute__((weak)) void port_setup_progress_draw(const char *what, int permille) {
  log_console_progress(what, permille);
  log_console_update(); /* the log on screen, if it is */
}

__attribute__((weak)) int port_setup_lib_override(const char *lib, unsigned long apk_crc, RtSetupCtx *ctx) {
  return 0;
}

void dcr_setup_progress(const char *what, int permille) {
  if (!g_setup_shown) {
    g_setup_shown = 1;
    debugPrintf("[setup] setting up %s -- this happens once\n", PORT_TITLE);
  }
  port_setup_progress_draw(what, permille);
}

void rt_setup_progress_in(const char *what, int p0, int p1, uint64_t done, uint64_t total) {
  int at = p0;
  if (total)
    at += (int)((long long)(p1 - p0) * (long long)(done < total ? done : total) / (long long)total);
  dcr_setup_progress(what, at);
}

int dcr_setup_did_work(void) { return g_setup_shown; }

void rt_setup_finish(void) {
  if (g_setup_shown)
    dcr_setup_progress("Starting the game", 1000);
}

/* --------------------------------------------------------------- files */
long rt_file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

void rt_mkdirs(const char *dir) {
  char p[512];
  snprintf(p, sizeof p, "%s", dir);
  char *s = strchr(p, ':'); /* past "sdmc:" and the root */
  s = s ? s + 1 : p;
  while (*s == '/')
    s++;
  for (s = strchr(s, '/'); s; s = strchr(s + 1, '/')) {
    *s = 0;
    mkdir(p, 0777);
    *s = '/';
  }
  mkdir(p, 0777);
}

void rt_mkdirs_for(const char *file) {
  char d[512];
  snprintf(d, sizeof d, "%s", file);
  char *s = strrchr(d, '/');
  if (s) {
    *s = 0;
    rt_mkdirs(d);
  }
}

uint8_t *rt_read_whole(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  *len = 0;
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
  if (b && n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  if (b)
    b[n] = 0; /* text files are used as strings */
  *len = b ? (size_t)n : 0;
  return b;
}

/* Write to <dst>.part, then put it in place: a half-written file is never
 * mistaken for a whole one. */
int rt_write_atomic(const char *dst, const void *buf, size_t len) {
  char tmp[520];
  snprintf(tmp, sizeof tmp, "%s.part", dst);
  rt_mkdirs_for(dst);
  FILE *f = fopen(tmp, "wb");
  if (!f)
    return 0;
  int ok = !len || fwrite(buf, 1, len, f) == len;
  if (fclose(f) != 0)
    ok = 0;
  if (ok) {
    unlink(dst);
    ok = rename(tmp, dst) == 0;
  }
  if (!ok)
    unlink(tmp);
  return ok;
}

/* The file already holds exactly these bytes. */
static int same_file(const char *path, const void *buf, size_t len) {
  if (rt_file_size(path) != (long)len)
    return 0;
  size_t n;
  uint8_t *cur = rt_read_whole(path, &n);
  int same = cur && n == len && !memcmp(cur, buf, len);
  free(cur);
  return same;
}

int rt_write_if_changed(const char *dst, const void *buf, size_t len) {
  if (same_file(dst, buf, len))
    return 0;
  if (!rt_write_atomic(dst, buf, len)) {
    debugPrintf("[setup] could not write %s\n", dst);
    return 0;
  }
  return 1;
}

unsigned long rt_crc_of_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  static unsigned char buf[1 << 16];
  mz_ulong crc = mz_crc32(0, NULL, 0);
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    crc = mz_crc32(crc, buf, n);
  fclose(f);
  return (unsigned long)crc;
}

/* ---------------------------------------------------------------- .setup */
/* One line per made file: "<name> <crc of its source> <size>". 16: Crossy
 * Road keeps 3 libraries, classes.txt and 5 DuckTales files there. */
#define MAX_STAMP 16
typedef struct {
  char name[32];
  unsigned long crc, size;
} Stamp;
static Stamp g_stamp[MAX_STAMP];
static int g_nstamp, g_stamp_dirty, g_stamp_loaded;

static void stamp_load(void) {
  if (g_stamp_loaded)
    return;
  g_stamp_loaded = 1;
  char path[300];
  rt_root_path(path, sizeof path, ".setup");
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  while (g_nstamp < MAX_STAMP && fscanf(f, "%31s %lx %lu", g_stamp[g_nstamp].name,
                                        &g_stamp[g_nstamp].crc, &g_stamp[g_nstamp].size) == 3)
    g_nstamp++;
  fclose(f);
}

static Stamp *stamp_get(const char *name) {
  stamp_load();
  for (int i = 0; i < g_nstamp; i++)
    if (!strcmp(g_stamp[i].name, name))
      return &g_stamp[i];
  return NULL;
}

static void stamp_set(const char *name, unsigned long crc, unsigned long size) {
  Stamp *s = stamp_get(name);
  if (!s && g_nstamp < MAX_STAMP) {
    s = &g_stamp[g_nstamp++];
    snprintf(s->name, sizeof s->name, "%s", name);
  }
  if (s && (s->crc != crc || s->size != size)) {
    s->crc = crc;
    s->size = size;
    g_stamp_dirty = 1;
  }
}

static void stamp_save(void) {
  if (!g_stamp_dirty)
    return;
  char path[300];
  rt_root_path(path, sizeof path, ".setup");
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (int i = 0; i < g_nstamp; i++)
    fprintf(f, "%s %08lx %lu\n", g_stamp[i].name, g_stamp[i].crc, g_stamp[i].size);
  fclose(f);
  g_stamp_dirty = 0;
}

int rt_setup_stamp_get(const char *key, unsigned long *crc, unsigned long *size) {
  Stamp *s = stamp_get(key);
  if (!s)
    return 0;
  if (crc)
    *crc = s->crc;
  if (size)
    *size = s->size;
  return 1;
}

void rt_setup_stamp_set(const char *key, unsigned long crc, unsigned long size) { stamp_set(key, crc, size); }
void rt_setup_stamp_save(void) { stamp_save(); }

/* ------------------------------------------------------------ extraction */
/* Write to <dst>.part, then put it in place: a half-written library is never
 * mistaken for a whole one. The bar follows the bytes written: base + done
 * of total, p0..p1. */
typedef struct {
  FILE *f;
  const char *what;
  int p0, p1, ok;
  uint64_t base, total;
} Unpack;

static size_t unpack_write(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  Unpack *u = opaque;
  if (!u->ok || fwrite(buf, 1, n, u->f) != n) {
    u->ok = 0;
    return 0;
  }
  if (u->what && u->total)
    rt_setup_progress_in(u->what, u->p0, u->p1, u->base + ofs + n, u->total);
  return n;
}

static int extract_to(mz_zip_archive *zip, int idx, const char *dst, const char *what, int p0, int p1,
                      uint64_t base, uint64_t total) {
  char tmp[520];
  snprintf(tmp, sizeof tmp, "%s.part", dst);
  rt_mkdirs_for(dst);
  unlink(tmp);
  Unpack u = {fopen(tmp, "wb"), what, p0, p1, 1, base, total};
  if (!u.f)
    return -1;
  int ok = mz_zip_reader_extract_to_callback(zip, (mz_uint)idx, unpack_write, &u, 0) && u.ok;
  if (fclose(u.f) != 0)
    ok = 0;
  if (!ok) {
    unlink(tmp);
    return -1;
  }
  unlink(dst);
  return rename(tmp, dst);
}

int rt_setup_extract_entry(mz_zip_archive *zip, int idx, const char *dst, const char *what, int p0, int p1) {
  mz_zip_archive_file_stat st;
  if (idx < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)idx, &st))
    return -1;
  if (what)
    dcr_setup_progress(what, p0);
  return extract_to(zip, idx, dst, what, p0, p1, 0, st.m_uncomp_size);
}

/* ------------------------------------------------------------- libraries */
static void ensure_libs(RtSetupCtx *ctx, const RtSetupPlan *p) {
  if (p->nlibs > RT_SETUP_MAX_LIBS)
    fatal_error("port_setup_plan: %u libraries, the runtime takes %d.", p->nlibs, RT_SETUP_MAX_LIBS);
  int idx[RT_SETUP_MAX_LIBS];
  unsigned long crcs[RT_SETUP_MAX_LIBS], sizes[RT_SETUP_MAX_LIBS];
  int need[RT_SETUP_MAX_LIBS] = {0};
  uint64_t total = 0;
  int any = 0;
  for (unsigned i = 0; i < p->nlibs; i++) {
    const char *lib = p->libs[i];
    char arc[96], dst[300];
    snprintf(arc, sizeof arc, PORT_ABI_DIR "%s", lib);
    rt_root_path(dst, sizeof dst, lib);
    idx[i] = mz_zip_reader_locate_file(ctx->apk, arc, NULL, 0);
    mz_zip_archive_file_stat st;
    if (idx[i] < 0 || !mz_zip_reader_file_stat(ctx->apk, (mz_uint)idx[i], &st))
      fatal_error("%s has no %s.\n\n%s", ctx->apk_path, arc,
                  p->apk_requirement ? p->apk_requirement : RT_SETUP_APK_REQUIREMENT);
    unsigned long crc = (unsigned long)st.m_crc32, size = (unsigned long)st.m_uncomp_size;
    crcs[i] = crc;
    sizes[i] = size;
    if (port_setup_lib_override(lib, crc, ctx))
      continue;
    long have = rt_file_size(dst);
    Stamp *s = stamp_get(lib);
    if (have == (long)size && s && s->crc == crc)
      continue; /* made from this APK before */
    if (have == (long)size && rt_crc_of_file(dst) == crc) {
      stamp_set(lib, crc, size); /* already the right file */
      continue;
    }
    need[i] = 1;
    any = 1;
    total += size;
  }
  if (!any)
    return;
  const char *what = p->libs_what ? p->libs_what
                     : p->nlibs > 1 ? "Unpacking the game's libraries"
                                    : "Unpacking the game's library";
  uint64_t done = 0;
  for (unsigned i = 0; i < p->nlibs; i++) {
    if (!need[i])
      continue;
    char dst[300];
    rt_root_path(dst, sizeof dst, p->libs[i]);
    rt_setup_progress_in(what, p->libs_p0, p->libs_p1, done, total);
    debugPrintf("[setup] unpacking %s from the APK (%lu KB)...\n", p->libs[i], sizes[i] >> 10);
    if (extract_to(ctx->apk, idx[i], dst, what, p->libs_p0, p->libs_p1, done, total) != 0)
      fatal_error("Could not write %s (from %s).\n\nIs the SD card full or read-only?", dst, ctx->apk_path);
    stamp_set(p->libs[i], crcs[i], sizes[i]);
    done += sizes[i];
  }
}

/* ------------------------------------------------------------ classes.txt */
static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void ensure_classes(mz_zip_archive *zip, int p0, int p1) {
  /* classes.dex, classes2.dex, ... at the top of the APK */
  int idx[32], n = 0;
  mz_ulong crc = mz_crc32(0, NULL, 0);
  for (int k = 1; k <= 32 && n < 32; k++) {
    char nm[32];
    if (k == 1)
      snprintf(nm, sizeof nm, "classes.dex");
    else
      snprintf(nm, sizeof nm, "classes%d.dex", k);
    int i = mz_zip_reader_locate_file(zip, nm, NULL, 0);
    mz_zip_archive_file_stat st;
    if (i < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)i, &st))
      break;
    idx[n++] = i;
    uint32_t c = st.m_crc32;
    crc = mz_crc32(crc, (const unsigned char *)&c, sizeof c);
  }
  char dst[300];
  rt_root_path(dst, sizeof dst, "classes.txt");
  Stamp *s = stamp_get("classes.txt");
  if (!n || (s && s->crc == (unsigned long)crc && s->size == (unsigned long)n && rt_file_size(dst) > 0))
    return; /* nothing to read, or already made from these */

  dcr_setup_progress("Reading the game's Java classes", p0);
  debugPrintf("[setup] listing the Java classes of the APK (%d dex file%s)...\n", n, n > 1 ? "s" : "");
  Names ns = {0};
  for (int k = 0; k < n; k++) {
    if (k)
      dcr_setup_progress("Reading the game's Java classes", p0 + k * (p1 - p0) / n);
    size_t len = 0;
    void *d = mz_zip_reader_extract_to_heap(zip, (mz_uint)idx[k], &len, 0);
    if (d) {
      dex_names(d, len, &ns);
      free(d);
    }
  }
  if (ns.n) {
    qsort(ns.v, (size_t)ns.n, sizeof *ns.v, cmp_str);
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s.part", dst);
    FILE *f = fopen(tmp, "w");
    int written = 0;
    if (f) {
      fputs("# Java classes defined by the game's APK (names only). The wrapper's JNI\n"
            "# FindClass/Class.forName report exactly these, plus the Android framework.\n", f);
      for (int i = 0; i < ns.n; i++)
        if (i == 0 || strcmp(ns.v[i], ns.v[i - 1])) {
          fputs(ns.v[i], f);
          fputc('\n', f);
          written++;
        }
      if (fclose(f) == 0) {
        unlink(dst);
        if (rename(tmp, dst) == 0) {
          stamp_set("classes.txt", (unsigned long)crc, (unsigned long)n);
          debugPrintf("[setup] classes.txt: %d Java class names\n", written);
        }
      }
    }
  }
  for (int i = 0; i < ns.n; i++)
    free(ns.v[i]);
  free(ns.v);
}

/* ------------------------------------------------- an uncompressed APK */
/* Crossy Road's APK as published compresses the game's data (assets/bin/Data:
 * 114 MB in 12), which Unity then inflates at every read -- each 1 MB .split
 * of a level on every map load. The layout that port runs on stores every
 * entry (what its tools/stage_sd.py makes), so a compressed APK is rewritten
 * that way once: every entry kept, same names, same CRCs, data 4-byte
 * aligned. The byte layout below is the one proven on hardware. */
static int compressed_under(mz_zip_archive *zip, const char *prefix, uint64_t *stored_size) {
  mz_uint n = mz_zip_reader_get_num_files(zip);
  size_t plen = strlen(prefix);
  int found = 0;
  *stored_size = 22;
  for (mz_uint i = 0; i < n; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st))
      continue;
    if (st.m_method != 0 && !strncmp(st.m_filename, prefix, plen))
      found = 1;
    *stored_size += 30 + 3 + 46 + 2 * strlen(st.m_filename) + st.m_uncomp_size;
  }
  return found;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { exefs_wr32(p, v); }

static int write_stored_zip(mz_zip_archive *zip, const char *dst, uint64_t total, const char *what, int p0,
                            int p1) {
  mz_uint n = mz_zip_reader_get_num_files(zip);
  FILE *f = fopen(dst, "wb");
  uint8_t *cd = malloc((size_t)n * (46 + 512)), *c = cd;
  if (!f || !cd || n > 0xFFFF) {
    if (f)
      fclose(f);
    free(cd);
    return -1;
  }
  setvbuf(f, NULL, _IOFBF, 1 << 20);
  uint32_t off = 0;
  int ok = 1;
  for (mz_uint i = 0; i < n && ok; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, i, &st)) {
      ok = 0;
      break;
    }
    size_t nl = strlen(st.m_filename), len = 0;
    void *data = NULL;
    if (st.m_uncomp_size && !(data = mz_zip_reader_extract_to_heap(zip, i, &len, 0))) {
      ok = 0;
      break;
    }
    uint32_t size = (uint32_t)st.m_uncomp_size, pad = (4 - (off + 30 + nl) % 4) % 4;
    uint8_t h[30] = {0};
    put32(h, 0x04034b50);
    put16(h + 4, 10);        /* version needed: stored */
    put16(h + 12, 0x0021);   /* 1980-01-01 */
    put32(h + 14, st.m_crc32);
    put32(h + 18, size);
    put32(h + 22, size);
    put16(h + 26, (unsigned)nl);
    put16(h + 28, pad);      /* extra field: alignment padding */
    static const uint8_t zeros[4];
    ok = fwrite(h, 1, 30, f) == 30 && fwrite(st.m_filename, 1, nl, f) == nl &&
         fwrite(zeros, 1, pad, f) == pad && (!size || fwrite(data, 1, size, f) == size);
    free(data);
    memset(c, 0, 46);
    put32(c, 0x02014b50);
    put16(c + 4, 20);
    put16(c + 6, 10);
    put16(c + 14, 0x0021);
    put32(c + 16, st.m_crc32);
    put32(c + 20, size);
    put32(c + 24, size);
    put16(c + 28, (unsigned)nl);
    put32(c + 42, off);
    memcpy(c + 46, st.m_filename, nl);
    c += 46 + nl;
    off += 30 + (uint32_t)nl + pad + size;
    rt_setup_progress_in(what, p0, p1, off, total);
    if (i % 100 == 99)
      debugPrintf("[setup]   %u/%u files\n", (unsigned)(i + 1), (unsigned)n);
  }
  uint8_t e[22] = {0};
  put32(e, 0x06054b50);
  put16(e + 8, n);
  put16(e + 10, n);
  put32(e + 12, (uint32_t)(c - cd));
  put32(e + 16, off);
  if (ok)
    ok = fwrite(cd, 1, (size_t)(c - cd), f) == (size_t)(c - cd) && fwrite(e, 1, 22, f) == 22;
  free(cd);
  if (fclose(f) != 0)
    ok = 0;
  return ok ? 0 : -1;
}

/* The APK rewritten stored, if it is compressed; the zip reopened on the
 * result. The original is replaced only once its copy has been read back. */
int rt_setup_store_apk(mz_zip_archive *zip, const char *apk, const char *prefix, int p0, int p1) {
  uint64_t need;
  if (!prefix || !compressed_under(zip, prefix, &need))
    return 0;
  const char *name = base_name(apk);
  struct statvfs vs;
  if (statvfs(dcr_game_root(), &vs) == 0 &&
      (uint64_t)vs.f_bavail * vs.f_frsize < need + (16u << 20)) {
    debugPrintf("[setup] %s is compressed and there is not room (%llu MB) for its uncompressed "
                "copy: using it as it is -- loading is slower\n", name, (unsigned long long)(need >> 20));
    return 0;
  }
  char what[160];
  snprintf(what, sizeof what, "Unpacking %s (once, about a minute)", name);
  dcr_setup_progress(what, p0);
  debugPrintf("[setup] %s is the compressed original: writing it uncompressed, once "
              "(%llu MB; this takes a minute)...\n", name, (unsigned long long)(need >> 20));
  char tmp[320], old[320];
  snprintf(tmp, sizeof tmp, "%s.part", apk);
  snprintf(old, sizeof old, "%s.original", apk);
  unlink(tmp);
  int ok = write_stored_zip(zip, tmp, need, what, p0, p1) == 0;
  snprintf(what, sizeof what, "Checking the unpacked %s", name);
  dcr_setup_progress(what, p1);
  mz_uint n = mz_zip_reader_get_num_files(zip);
  mz_zip_reader_end(zip);
  if (ok) {
    mz_zip_archive chk;
    uint64_t dummy;
    memset(&chk, 0, sizeof chk);
    ok = mz_zip_reader_init_file(&chk, tmp, 0) && mz_zip_reader_get_num_files(&chk) == n &&
         !compressed_under(&chk, prefix, &dummy);
    mz_zip_reader_end(&chk);
  }
  if (ok) {
    unlink(old);
    ok = rename(apk, old) == 0 && rename(tmp, apk) == 0;
    if (ok)
      unlink(old);
    else if (rt_file_size(apk) < 0)
      rename(old, apk); /* put the original back */
  }
  if (!ok) {
    unlink(tmp);
    debugPrintf("[setup] could not write the uncompressed %s: using the compressed one -- "
                "loading is slower\n", name);
  } else {
    debugPrintf("[setup] %s is now uncompressed (%u files)\n", name, (unsigned)n);
  }
  memset(zip, 0, sizeof *zip);
  if (!mz_zip_reader_init_file(zip, apk, 0))
    fatal_error("%s could not be reopened after being rewritten.", apk);
  return ok;
}

/* ------------------------------------------------- an APK's package name */
/* AndroidManifest.xml is binary XML: its strings are UTF-16 (older tools
 * wrote UTF-8), so both are looked for. */
static int bytes_have(const uint8_t *b, size_t n, const char *s) {
  size_t k = strlen(s);
  for (size_t i = 0; i + k <= n; i++)
    if (!memcmp(b + i, s, k))
      return 1;
  for (size_t i = 0; i + 2 * k <= n; i++) {
    size_t j = 0;
    while (j < k && b[i + 2 * j] == (uint8_t)s[j] && b[i + 2 * j + 1] == 0)
      j++;
    if (j == k)
      return 1;
  }
  return 0;
}

int rt_apk_has_package(const char *apk, const char *package) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!package || !*package || !mz_zip_reader_init_file(&zip, apk, 0))
    return 0;
  size_t n = 0;
  uint8_t *m = mz_zip_reader_extract_file_to_heap(&zip, "AndroidManifest.xml", &n, 0);
  mz_zip_reader_end(&zip);
  int has = m && bytes_have(m, n, package);
  if (m)
    mz_free(m);
  return has;
}

/* ------------------------------------------------------------- the driver */
static void run_steps(const RtSetupPlan *p, int when, RtSetupCtx *ctx) {
  for (unsigned i = 0; i < p->nsteps; i++) {
    const RtSetupStep *s = &p->steps[i];
    if (s->when != when || !s->run)
      continue;
    ctx->p0 = s->p0;
    ctx->p1 = s->p1;
    s->run(ctx);
  }
}

void dcr_setup_from_apk(const char *apk) {
  const RtSetupPlan *p = &port_setup_plan ? &port_setup_plan : &k_no_plan;
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, apk, 0))
    return; /* main.c reports a missing or unreadable APK */
  g_in_setup = 1;
  stamp_load();
  RtSetupCtx ctx = {&zip, apk, 0, 0};
  /* first: it reopens the zip, and the libraries come out of the result */
  if (p->store_apk_prefix)
    rt_setup_store_apk(&zip, apk, p->store_apk_prefix, p->store_p0, p->store_p1);
  run_steps(p, RT_STEP_BEFORE_LIBS, &ctx);
  ensure_libs(&ctx, p);
  ensure_classes(&zip, p->classes_p0, p->classes_p1);
  run_steps(p, RT_STEP_WITH_ZIP, &ctx);
  mz_zip_reader_end(&zip);
  ctx.apk = NULL;
  run_steps(p, RT_STEP_AFTER_ZIP, &ctx);
  stamp_save();
  g_in_setup = 0;
  if (!p->finish_in_port)
    rt_setup_finish();
}

/* ---------------------------------------------------- the launcher NRO */
/* The newest launcher NRO in the game folder: its path and build. macOS's
 * "._" shadow files are skipped. */
uint64_t rt_find_nro(char *path, size_t cap) {
  uint64_t best = 0;
  DIR *d = opendir(dcr_game_root());
  if (!d)
    return 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n < 5 || strcasecmp(e->d_name + n - 4, ".nro") || !strncmp(e->d_name, "._", 2))
      continue;
    char p[320];
    rt_root_path(p, sizeof p, e->d_name);
    FILE *f = fopen(p, "rb");
    if (!f)
      continue;
    uint64_t b = nro_build(f);
    fclose(f);
    if (b > best) {
      best = b;
      snprintf(path, cap, "%s", p);
    }
  }
  closedir(d);
  return best;
}

/* A file the launcher carries in its romfs for the port (Labyrinth 2's
 * iPad game, PvZ's English files, Crossy Road's DuckTales pack), copied out
 * of the newest NRO in the game folder that has it. */
int rt_setup_copy_from_nro(const char *romfs_name, const char *dst_rel, const char *stamp_key, const char *what,
                           int p0, int p1) {
  char dst[320], from[330], best_path[320] = "";
  rt_root_path(dst, sizeof dst, dst_rel);
  snprintf(from, sizeof from, "%s.from", dst);
  uint64_t best = 0;
  long best_off = 0;
  size_t best_size = 0;
  DIR *d = opendir(dcr_game_root());
  struct dirent *e;
  while (d && (e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n < 5 || strcasecmp(e->d_name + n - 4, ".nro") || !strncmp(e->d_name, "._", 2))
      continue;
    char p[320];
    rt_root_path(p, sizeof p, e->d_name);
    FILE *f = fopen(p, "rb");
    if (!f)
      continue;
    long off = 0;
    size_t size = 0;
    uint64_t b = nro_build(f);
    if (nro_romfs_file(f, romfs_name, &off, &size) == 0 && size > 0 && b >= best) {
      best = b, best_off = off, best_size = size;
      snprintf(best_path, sizeof best_path, "%s", p);
    }
    fclose(f);
  }
  if (d)
    closedir(d);
  const int stamped = !stamp_key || *stamp_key;
  if (!best_path[0])
    return stamped && rt_file_size(dst) > 0 ? 1 : 0; /* copied before, from an NRO no longer here */

  char want[64], have[64] = "";
  snprintf(want, sizeof want, "%llu %lu", (unsigned long long)best, (unsigned long)best_size);
  FILE *s = NULL;
  if (!stamp_key) {
    if ((s = fopen(from, "r"))) {
      if (!fgets(have, sizeof have, s))
        have[0] = 0;
      have[strcspn(have, "\r\n")] = 0;
      fclose(s);
    }
    if (!strcmp(have, want) && rt_file_size(dst) == (long)best_size)
      return 1;
  } else if (*stamp_key) {
    Stamp *st = stamp_get(stamp_key);
    if (st && st->crc == (unsigned long)best && rt_file_size(dst) == (long)best_size)
      return 1;
  }

  char tmp[340];
  snprintf(tmp, sizeof tmp, "%s.part", dst);
  rt_mkdirs_for(dst);
  FILE *in = fopen(best_path, "rb"), *o = in ? fopen(tmp, "wb") : NULL;
  int ok = in && o && fseek(in, best_off, SEEK_SET) == 0;
  static uint8_t buf[256 * 1024];
  for (size_t left = best_size; ok && left;) {
    if (what)
      rt_setup_progress_in(what, p0, p1, best_size - left, best_size);
    size_t k = left < sizeof buf ? left : sizeof buf;
    ok = fread(buf, 1, k, in) == k && fwrite(buf, 1, k, o) == k;
    left -= k;
  }
  if (o && fclose(o) != 0)
    ok = 0;
  if (in)
    fclose(in);
  if (ok) {
    unlink(dst);
    ok = rename(tmp, dst) == 0;
  }
  if (!ok) {
    unlink(tmp);
    debugPrintf("[setup] %s could not be copied from %s (is the SD card full?)\n", romfs_name, best_path);
    return -1;
  }
  if (!stamp_key) {
    if ((s = fopen(from, "w"))) {
      fprintf(s, "%s\n", want);
      fclose(s);
    }
  } else if (*stamp_key) {
    stamp_set(stamp_key, (unsigned long)best, (unsigned long)best_size);
    if (!g_in_setup)
      stamp_save();
  }
  debugPrintf("[setup] %s: %lu KB from %s\n", romfs_name, (unsigned long)(best_size >> 10), base_name(best_path));
  return 2;
}

/* ---------------------------------------------------- updates from the NRO */
void dcr_setup_update_from_nro(void) {
  u64 tid = 0;
  if (R_FAILED(svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || !exefs_is_forwarder_tid(tid))
    return;
  char ovr[128], marker[300], nro[320];
  snprintf(ovr, sizeof ovr, "sdmc:/atmosphere/contents/%016llX/exefs.nsp", (unsigned long long)tid);
  rt_root_path(marker, sizeof marker, ".update");
  if (rt_file_size(ovr) <= 0)
    return; /* not running through an override: nothing of ours to update */

  uint64_t attempted = 0;
  FILE *mf = fopen(marker, "r");
  if (mf) {
    if (fscanf(mf, "%llu", (unsigned long long *)&attempted) != 1)
      attempted = 0;
    fclose(mf);
    if (attempted == DCR_BUILD)
      unlink(marker); /* that update took */
  }
  /* The game follows the NRO its icon starts (the forwarder's
   * /nextNroPath): when that carries another build, newer or older, it is
   * installed, so going back is copying the old NRO back. Without one
   * (an icon whose NRO is gone), the newest NRO in the folder, newer only. */
  uint64_t build = 0;
  char icon[300]; /* + "sdmc:" fits nro */
  if (rt_boot_icon_nro(icon, sizeof icon) == 0) {
    snprintf(nro, sizeof nro, "sdmc:%s", icon);
    FILE *nf = fopen(nro, "rb");
    if (nf) {
      build = nro_build(nf);
      fclose(nf);
    }
  }
  if (build) {
    debugPrintf("[setup] build %llu; this icon's NRO (%s) carries build %llu%s\n", (unsigned long long)DCR_BUILD,
                icon, (unsigned long long)build,
                build == DCR_BUILD ? "" : build > DCR_BUILD ? ", newer" : ", older");
    if (build == DCR_BUILD)
      return;
  } else {
    build = rt_find_nro(nro, sizeof nro);
    debugPrintf("[setup] build %llu%s\n", (unsigned long long)DCR_BUILD,
                build > DCR_BUILD ? "; the launcher NRO carries a newer one" : "");
    if (build <= DCR_BUILD)
      return;
  }
  if (attempted == build) {
    debugPrintf("[setup] %s: build %llu was installed but this is still build %llu -- not retrying "
                "(delete %s to try again)\n", nro, (unsigned long long)build,
                (unsigned long long)DCR_BUILD, marker);
    return;
  }

  /* the override must be this program's own: 32-bit, this title */
  size_t cur_len = 0, npdm_len, nsp_len = 0;
  uint8_t *cur = rt_read_whole(ovr, &cur_len);
  const uint8_t *npdm;
  uint64_t pid = 0;
  int is64 = 1;
  int ours = cur && exefs_find(cur, cur_len, "main.npdm", &npdm, &npdm_len) == 0 &&
             npdm_info(npdm, npdm_len, &pid, &is64) == 0 && pid == tid && !is64;
  free(cur);
  if (!ours)
    return;

  FILE *f = fopen(nro, "rb");
  long off;
  uint8_t *nsp = NULL, *out = NULL;
  size_t out_len = 0;
  if (f && nro_romfs_file(f, PORT_NSP_NAME, &off, &nsp_len) == 0 && (nsp = malloc(nsp_len)) &&
      fseek(f, off, SEEK_SET) == 0 && fread(nsp, 1, nsp_len, f) == nsp_len)
    exefs_build_override(nsp, nsp_len, tid, &out, &out_len);
  if (f)
    fclose(f);
  free(nsp);
  if (!out) {
    debugPrintf("[setup] %s: its copy of the wrapper is unreadable -- not updating\n", nro);
    return;
  }
  dcr_setup_progress(build > DCR_BUILD ? "Updating to the new build, then restarting"
                                       : "Going back to the NRO's build, then restarting",
                     RT_SETUP_UPDATE_PERMILLE);
  debugPrintf("[setup] installing build %llu from %s, then restarting...\n", (unsigned long long)build, nro);
  log_console_update(); /* that line on screen, when the log is */
  char tmp[160];
  snprintf(tmp, sizeof tmp, "%s.part", ovr);
  FILE *o = fopen(tmp, "wb");
  int ok = o && fwrite(out, 1, out_len, o) == out_len;
  if (o && fclose(o) != 0)
    ok = 0;
  free(out);
  if (ok) {
    unlink(ovr);
    ok = rename(tmp, ovr) == 0;
  }
  if (!ok) {
    unlink(tmp);
    debugPrintf("[setup] could not write %s -- still running build %llu\n", ovr, (unsigned long long)DCR_BUILD);
    return;
  }
  mf = fopen(marker, "w");
  if (mf) {
    fprintf(mf, "%llu\n", (unsigned long long)build);
    fclose(mf);
  }
  log_flush_ring();
  Result rc = appletRestartProgram(NULL, 0);
  fatal_error("Installed build %llu from %s.\n\n"
              "Restarting did not work (0x%x): close the game and launch it again.",
              (unsigned long long)build, nro, (unsigned)rc);
}

/* ------------------------------------------------ back to the launcher */
/* Once the icon starts this program instead of the launcher, work only the
 * launcher does (Asphalt 8 installs the mod's zips there) would go undone.
 * Removing the override hands the icon back to the launcher, which writes
 * the override again when it is done. */
int rt_setup_restart_into_launcher(const char *what) {
  u64 tid = 0;
  if (R_FAILED(svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || !exefs_is_forwarder_tid(tid))
    return 0;
  char ovr[128];
  snprintf(ovr, sizeof ovr, "sdmc:/atmosphere/contents/%016llX/exefs.nsp", (unsigned long long)tid);
  if (rt_file_size(ovr) <= 0)
    return 0;
  dcr_setup_progress(what, RT_SETUP_UPDATE_PERMILLE);
  debugPrintf("[setup] %s: restarting into the launcher...\n", what);
  log_console_update();
  if (unlink(ovr) != 0) {
    debugPrintf("[setup] could not remove %s -- not restarting into the launcher\n", ovr);
    return -1;
  }
  log_flush_ring();
  Result rc = appletRestartProgram(NULL, 0);
  fatal_error("%s.\n\nRestarting into the launcher did not work (0x%x): close the game and\n"
              "launch it again.",
              what, (unsigned)rc);
}
