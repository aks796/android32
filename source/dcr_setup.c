/* dcr_setup.c -- a first launch from nothing but game.apk.
 *
 * The game folder (/switch/labyrinth2_nx) needs only the user's own APK, as
 * game.apk, and the launcher NRO (launcher/). Everything else is made from the
 * APK here, before anything is loaded:
 *   liblabyrinthii.so       lib/armeabi/ out of game.apk
 *   classes.txt             the Java class names its classes.dex defines
 *                           (jni_core.c answers FindClass with exactly those)
 * and made again whenever game.apk changes: .setup records the CRC-32 of each
 * source entry. Files that are already right are checked once and kept. The
 * engine's own files (textures, meshes, level packs) are unpacked into its
 * files dir by lab_files.c; the menus read their pictures and the sounds
 * straight out of the APK (lab_apk.c).
 *
 * UPDATES FROM THE NRO. This program runs as a forwarder title through an
 * ExeFS override, /atmosphere/contents/<title id>/exefs.nsp, which the
 * launcher wrote on its first run (it carries labyrinth2_nx.nsp in its romfs).
 * When the NRO in the game folder carries a NEWER build than the one running
 * (romfs:/labyrinth2_nx.build against DCR_BUILD), the override is rewritten from
 * it (dcr_exefs.h) and the program restarts into the new build: updating is
 * copying the new NRO over the old one. Never a downgrade, never a file this
 * program did not come from (the override must exist and name this title),
 * and never twice for the same build (.update records the attempt). MIT.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>

#include "config.h"
#include "dcr_build.h"
#include "dcr_config.h"
#include "dcr_exefs.h"
#include "dcr_formats.h"
#include "error.h"
#include "util.h"

const char *dcr_game_root(void); /* main.c */

static const char *const k_libs[] = {LAB_LIB_GAME};
#define ABI_DIR LAB_ABI_DIR

static void root_path(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s/%s", dcr_game_root(), name);
}

/* Setup work shows on screen as the PvZ Touch port's green progress bar
 * (util.c log_console_progress): the name, what is being done, the bar. The
 * log goes to debug.log only -- or scrolls on screen instead with config.ini
 * [debug] boot_log_on_screen. A start with nothing to do shows nothing.
 *
 * The bar, in permille of the whole first launch:
 *     0- 100  liblabyrinthii.so unpacked from the APK
 *   100- 150  the Java class list
 *   150- 400  the iPad game's files copied out of the NRO (by bytes)
 *   400- 850  the game's files and levels from the APK (lab_files.c, by files)
 *   850-1000  the iPad level packs, floors and walls (lab_files.c), then the
 *             game starts (main.c) */
static int g_setup_shown;
void dcr_setup_progress(const char *what, int permille) {
  if (!g_setup_shown) {
    g_setup_shown = 1;
    debugPrintf("[setup] setting up Labyrinth 2 -- this happens once\n");
  }
  log_console_progress(what, permille);
  log_console_update(); /* the log on screen, if it is */
}
int dcr_setup_did_work(void) { return g_setup_shown; }

static long file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

/* ---------------------------------------------------------------- .setup */
/* One line per made file: "<name> <crc of its source> <size>". */
#define MAX_STAMP 8
typedef struct {
  char name[32];
  unsigned long crc, size;
} Stamp;
static Stamp g_stamp[MAX_STAMP];
static int g_nstamp, g_stamp_dirty;

static void stamp_load(void) {
  char path[300];
  root_path(path, sizeof path, ".setup");
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  while (g_nstamp < MAX_STAMP && fscanf(f, "%31s %lx %lu", g_stamp[g_nstamp].name,
                                        &g_stamp[g_nstamp].crc, &g_stamp[g_nstamp].size) == 3)
    g_nstamp++;
  fclose(f);
}

static Stamp *stamp_get(const char *name) {
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
  root_path(path, sizeof path, ".setup");
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (int i = 0; i < g_nstamp; i++)
    fprintf(f, "%s %08lx %lu\n", g_stamp[i].name, g_stamp[i].crc, g_stamp[i].size);
  fclose(f);
}

/* ------------------------------------------------------------- libraries */
static unsigned long crc_of_file(const char *path) {
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

/* Write to <dst>.part, then put it in place: a half-written library is never
 * mistaken for a whole one. */
static int extract_entry(mz_zip_archive *zip, int idx, const char *dst) {
  char tmp[320];
  snprintf(tmp, sizeof tmp, "%s.part", dst);
  unlink(tmp);
  if (!mz_zip_reader_extract_to_file(zip, (mz_uint)idx, tmp, 0)) {
    unlink(tmp);
    return -1;
  }
  unlink(dst);
  return rename(tmp, dst);
}

static void ensure_libs(mz_zip_archive *zip, const char *apk) {
  for (unsigned i = 0; i < sizeof k_libs / sizeof k_libs[0]; i++) {
    char arc[64], dst[300];
    snprintf(arc, sizeof arc, ABI_DIR "%s", k_libs[i]);
    root_path(dst, sizeof dst, k_libs[i]);
    int idx = mz_zip_reader_locate_file(zip, arc, NULL, 0);
    mz_zip_archive_file_stat st;
    if (idx < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)idx, &st))
      fatal_error("%s has no %s.\n\n"
                  "This port needs Labyrinth 2 1.29 (se.illusionlabs.labyrinth2, armeabi):\n"
                  "put the APK of that version in the game folder.",
                  apk, arc);
    unsigned long crc = (unsigned long)st.m_crc32, size = (unsigned long)st.m_uncomp_size;
    long have = file_size(dst);
    Stamp *s = stamp_get(k_libs[i]);
    if (have == (long)size && s && s->crc == crc)
      continue; /* made from this APK before */
    if (have == (long)size && crc_of_file(dst) == crc) {
      stamp_set(k_libs[i], crc, size); /* already the right file */
      continue;
    }
    dcr_setup_progress("Unpacking the game's library", 0);
    debugPrintf("[setup] unpacking %s from the APK (%lu KB)...\n", k_libs[i], size >> 10);
    if (extract_entry(zip, idx, dst) != 0)
      fatal_error("Could not write %s (from %s).\n\nIs the SD card full or read-only?", dst, apk);
    stamp_set(k_libs[i], crc, size);
  }
}

/* ------------------------------------------------------------ classes.txt */
static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void ensure_classes(mz_zip_archive *zip) {
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
  root_path(dst, sizeof dst, "classes.txt");
  Stamp *s = stamp_get("classes.txt");
  if (!n || (s && s->crc == (unsigned long)crc && s->size == (unsigned long)n && file_size(dst) > 0))
    return; /* nothing to read, or already made from these */

  dcr_setup_progress("Reading the game's Java classes", 100);
  debugPrintf("[setup] listing the Java classes of the APK (%d dex file%s)...\n", n, n > 1 ? "s" : "");
  Names ns = {0};
  for (int k = 0; k < n; k++) {
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

void dcr_setup_from_apk(const char *apk) {
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, apk, 0))
    return; /* main.c reports a missing or unreadable APK */
  stamp_load();
  ensure_libs(&zip, apk);
  ensure_classes(&zip);
  mz_zip_reader_end(&zip);
  stamp_save();
}

/* ---------------------------------------------------- updates from the NRO */
/* The newest launcher NRO in the game folder: its path and build. */
static uint64_t find_nro(char *path, size_t cap) {
  uint64_t best = 0;
  DIR *d = opendir(dcr_game_root());
  if (!d)
    return 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n < 5 || strcasecmp(e->d_name + n - 4, ".nro"))
      continue;
    char p[320];
    root_path(p, sizeof p, e->d_name);
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

/* ------------------------------------------ the iPad game's files, from the NRO
 * The launcher carries the iPad game's files the port uses (ipad.ipa in its
 * romfs: tools/make_ipad_assets.py, from Labyrinth 2 HD 1.6.0). The newest
 * NRO in the game folder that has them is copied to data/ipad.ipa when that
 * is missing or another build's (data/ipad.ipa.from: the build and size). */
int dcr_setup_ipad_from_nro(char *out, size_t cap) {
  char dst[320], from[320], best_path[320] = "";
  root_path(dst, sizeof dst, "data/ipad.ipa");
  root_path(from, sizeof from, "data/ipad.ipa.from");
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
    root_path(p, sizeof p, e->d_name);
    FILE *f = fopen(p, "rb");
    if (!f)
      continue;
    long off = 0;
    size_t size = 0;
    uint64_t b = nro_build(f);
    if (nro_romfs_file(f, "ipad.ipa", &off, &size) == 0 && size > 0 && b >= best) {
      best = b, best_off = off, best_size = size;
      snprintf(best_path, sizeof best_path, "%s", p);
    }
    fclose(f);
  }
  if (d)
    closedir(d);
  if (!best_path[0]) {
    if (file_size(dst) <= 0)
      return 0;
    snprintf(out, cap, "%s", dst); /* copied before, from an NRO no longer here */
    return 1;
  }
  char want[64], have[64] = "";
  snprintf(want, sizeof want, "%llu %lu", (unsigned long long)best, (unsigned long)best_size);
  FILE *s = fopen(from, "r");
  if (s) {
    if (!fgets(have, sizeof have, s))
      have[0] = 0;
    have[strcspn(have, "\r\n")] = 0;
    fclose(s);
  }
  if (!strcmp(have, want) && file_size(dst) == (long)best_size) {
    snprintf(out, cap, "%s", dst);
    return 1;
  }
  char tmp[330];
  snprintf(tmp, sizeof tmp, "%s.part", dst);
  FILE *in = fopen(best_path, "rb"), *o = in ? fopen(tmp, "wb") : NULL;
  int ok = in && o && fseek(in, best_off, SEEK_SET) == 0;
  static uint8_t buf[256 * 1024];
  for (size_t left = best_size; ok && left;) {
    dcr_setup_progress("Copying the iPad game's menus and levels",
                       150 + (int)(250.0 * (double)(best_size - left) / (double)best_size));
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
    debugPrintf("[setup] the iPad game's files could not be copied from %s (is the SD card full?)\n", best_path);
    return 0;
  }
  if ((s = fopen(from, "w"))) {
    fprintf(s, "%s\n", want);
    fclose(s);
  }
  debugPrintf("[setup] the iPad game's files: %lu KB from %s\n", (unsigned long)(best_size >> 10), best_path);
  snprintf(out, cap, "%s", dst);
  return 1;
}

static uint8_t *read_whole(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
    free(b);
    b = NULL;
  }
  fclose(f);
  *len = b ? (size_t)n : 0;
  return b;
}

void dcr_setup_update_from_nro(void) {
  u64 tid = 0;
  if (R_FAILED(svcGetInfo(&tid, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || !exefs_is_forwarder_tid(tid))
    return;
  char ovr[128], marker[300], nro[320];
  snprintf(ovr, sizeof ovr, "sdmc:/atmosphere/contents/%016llX/exefs.nsp", (unsigned long long)tid);
  root_path(marker, sizeof marker, ".update");
  if (file_size(ovr) <= 0)
    return; /* not running through an override: nothing of ours to update */

  uint64_t attempted = 0;
  FILE *mf = fopen(marker, "r");
  if (mf) {
    if (fscanf(mf, "%llu", (unsigned long long *)&attempted) != 1)
      attempted = 0;
    fclose(mf);
    if (attempted <= DCR_BUILD)
      unlink(marker); /* that update took */
  }
  uint64_t build = find_nro(nro, sizeof nro);
  debugPrintf("[setup] build %llu%s\n", (unsigned long long)DCR_BUILD,
              build > DCR_BUILD ? "; the launcher NRO carries a newer one" : "");
  if (build <= DCR_BUILD)
    return;
  if (attempted == build) {
    debugPrintf("[setup] %s: build %llu was installed but this is still build %llu -- not retrying "
                "(delete %s to try again)\n", nro, (unsigned long long)build,
                (unsigned long long)DCR_BUILD, marker);
    return;
  }

  /* the override must be this program's own: 32-bit, this title */
  size_t cur_len = 0, npdm_len, nsp_len = 0;
  uint8_t *cur = read_whole(ovr, &cur_len);
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
  if (f && nro_romfs_file(f, LAB_NSP_NAME, &off, &nsp_len) == 0 && (nsp = malloc(nsp_len)) &&
      fseek(f, off, SEEK_SET) == 0 && fread(nsp, 1, nsp_len, f) == nsp_len)
    exefs_build_override(nsp, nsp_len, tid, &out, &out_len);
  if (f)
    fclose(f);
  free(nsp);
  if (!out) {
    debugPrintf("[setup] %s: its copy of the wrapper is unreadable -- not updating\n", nro);
    return;
  }
  dcr_setup_progress("Updating to the new build, then restarting", 1000);
  debugPrintf("[setup] updating to build %llu from %s, then restarting...\n", (unsigned long long)build, nro);
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
  fatal_error("Updated to build %llu from %s.\n\n"
              "Restarting did not work (0x%x): close the game and launch it again.",
              (unsigned long long)build, nro, (unsigned)rc);
}
