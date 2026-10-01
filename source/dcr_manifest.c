/* dcr_manifest.c -- read the package facts out of the user's own APK.
 *
 * The game asks "Java" for its package name, versionName/versionCode and the
 * <meta-data> of the application (ApplicationInfo.metaData); they are read
 * from the APK the user supplies (so the boot log also says which build it
 * is) rather than written into the port. Some must match the data inside the
 * APK exactly (Unity checks unity.build-id against the build GUID of its
 * data: DcrMeta.s keeps 128 bytes for it). The manifest is Android binary XML
 * (AXML): a string pool, a resource-id map, and element chunks whose
 * attributes are typed values. Only <manifest> and <meta-data> elements are
 * of interest. MIT.
 */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dcr_manifest.h"
#include "rt_settings.h"
#include "util.h"

/* RT_MANIFEST_LOG_PREFIX (optional): meta-data entries whose names begin
 * with it are logged after a load. dcr "unity" (unity.build-id and co.);
 * others none. */

#define MAX_META 64

static struct {
  int loaded;
  char package[128];
  char version_name[64];
  int version_code;
  int nmeta;
  DcrMeta meta[MAX_META];
} M;

/* ------------------------------------------------------------- AXML */
#define RES_STRING_POOL 0x0001
#define RES_XML 0x0003
#define RES_XML_START_ELEMENT 0x0102
#define RES_XML_RESOURCE_MAP 0x0180

#define ATTR_NAME 0x01010003
#define ATTR_VALUE 0x01010024
#define ATTR_VERSION_CODE 0x0101021b
#define ATTR_VERSION_NAME 0x0101021c

#define TYPE_REFERENCE 0x01
#define TYPE_STRING 0x03
#define TYPE_FLOAT 0x04
#define TYPE_INT_DEC 0x10
#define TYPE_INT_HEX 0x11
#define TYPE_BOOLEAN 0x12

typedef struct {
  const uint8_t *base;
  uint32_t count, flags, strings_start;
  const uint32_t *offsets;
  const uint8_t *end;
} Pool;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* String i of the pool as UTF-8 into out (ASCII-safe; non-ASCII UTF-16 -> '?'). */
static const char *pool_str(const Pool *sp, uint32_t i, char *out, size_t cap) {
  out[0] = 0;
  if (!sp->base || i >= sp->count)
    return out;
  const uint8_t *s = sp->base + sp->strings_start + rd32((const uint8_t *)&sp->offsets[i]);
  if (s >= sp->end)
    return out;
  if (sp->flags & 0x100) { /* UTF-8: char count, byte count, bytes */
    if (*s & 0x80) s += 2; else s += 1;
    size_t n = *s & 0x80 ? (size_t)((s[0] & 0x7f) << 8 | s[1]) : s[0];
    s += (*s & 0x80) ? 2 : 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
  } else { /* UTF-16 */
    size_t n = rd16(s);
    s += 2;
    if (n & 0x8000) {
      n = (n & 0x7fff) << 16 | rd16(s);
      s += 2;
    }
    size_t o = 0;
    for (size_t k = 0; k < n && o + 1 < cap && s + 2 * k + 1 < sp->end; k++) {
      uint16_t c = rd16(s + 2 * k);
      out[o++] = c < 0x80 ? (char)c : '?';
    }
    out[o] = 0;
  }
  return out;
}

static void add_meta(const char *name, int type, uint32_t data, const char *str) {
  if (M.nmeta >= MAX_META || !name[0])
    return;
  DcrMeta *e = &M.meta[M.nmeta++];
  snprintf(e->name, sizeof e->name, "%s", name);
  e->type = type == TYPE_STRING ? DCR_META_STRING
          : type == TYPE_BOOLEAN ? DCR_META_BOOL
          : type == TYPE_FLOAT ? DCR_META_FLOAT
          : DCR_META_INT; /* decimal/hex ints and unresolved @references */
  e->i = (int32_t)data;
  memcpy(&e->f, &data, 4);
  snprintf(e->s, sizeof e->s, "%s", str ? str : "");
}

static int parse_axml(const uint8_t *buf, size_t len) {
  if (len < 8 || rd16(buf) != RES_XML)
    return -1;
  Pool sp = {0};
  const uint32_t *resmap = NULL;
  uint32_t nres = 0;
  const uint8_t *end = buf + len;
  const uint8_t *p = buf + rd16(buf + 2);
  char tag[64], an[64], sv[128];

  while (p + 8 <= end) {
    uint16_t type = rd16(p), hsize = rd16(p + 2);
    uint32_t size = rd32(p + 4);
    if (size < 8 || p + size > end)
      break;
    if (type == RES_STRING_POOL) {
      sp.base = p;
      sp.count = rd32(p + 8);
      sp.flags = rd32(p + 16);
      sp.strings_start = rd32(p + 20);
      sp.offsets = (const uint32_t *)(p + hsize);
      sp.end = p + size;
    } else if (type == RES_XML_RESOURCE_MAP) {
      resmap = (const uint32_t *)(p + hsize);
      nres = (size - hsize) / 4;
    } else if (type == RES_XML_START_ELEMENT && size >= 36) {
      const uint8_t *ext = p + hsize;
      pool_str(&sp, rd32(ext + 4), tag, sizeof tag);
      uint16_t astart = rd16(ext + 8), asize = rd16(ext + 10), acount = rd16(ext + 12);
      int is_manifest = !strcmp(tag, "manifest"), is_meta = !strcmp(tag, "meta-data");
      char meta_name[96] = "", meta_str[128] = "";
      int meta_type = -1;
      uint32_t meta_data = 0;
      for (uint16_t k = 0; k < acount; k++) {
        const uint8_t *a = ext + astart + (size_t)k * asize;
        if (a + 20 > p + size)
          break;
        uint32_t name_idx = rd32(a + 4), raw = rd32(a + 8);
        uint8_t dtype = a[15];
        uint32_t data = rd32(a + 16);
        uint32_t rid = (resmap && name_idx < nres) ? rd32((const uint8_t *)&resmap[name_idx]) : 0;
        pool_str(&sp, name_idx, an, sizeof an);
        const char *str = dtype == TYPE_STRING ? pool_str(&sp, data, sv, sizeof sv)
                        : raw != 0xffffffffu ? pool_str(&sp, raw, sv, sizeof sv) : "";
        if (is_manifest) {
          if (!strcmp(an, "package"))
            snprintf(M.package, sizeof M.package, "%s", str);
          else if (rid == ATTR_VERSION_CODE || !strcmp(an, "versionCode"))
            M.version_code = (int)data;
          else if (rid == ATTR_VERSION_NAME || !strcmp(an, "versionName"))
            snprintf(M.version_name, sizeof M.version_name, "%s", str);
        } else if (is_meta) {
          if (rid == ATTR_NAME || !strcmp(an, "name"))
            snprintf(meta_name, sizeof meta_name, "%s", str);
          else if (rid == ATTR_VALUE || !strcmp(an, "value")) {
            meta_type = dtype;
            meta_data = data;
            snprintf(meta_str, sizeof meta_str, "%s", str);
          }
        }
      }
      if (is_meta && meta_type >= 0)
        add_meta(meta_name, meta_type, meta_data, meta_str);
    }
    p += size;
  }
  return M.package[0] ? 0 : -1;
}

/* The APK search (rt_boot.c) reads every candidate's manifest: those reads
 * are not logged, only the one of the APK the game runs on. */
static int g_quiet;
#define LOG(...) do { if (!g_quiet) debugPrintf(__VA_ARGS__); } while (0)

int dcr_manifest_load(const char *apk_path) {
  memset(&M, 0, sizeof M); /* one APK's facts at a time (the APK finder reads several) */
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, apk_path, 0)) {
    LOG("[manifest] cannot open %s as a zip\n", apk_path);
    return -1;
  }
  size_t len = 0;
  void *buf = mz_zip_reader_extract_file_to_heap(&zip, "AndroidManifest.xml", &len, 0);
  mz_zip_reader_end(&zip);
  if (!buf) {
    LOG("[manifest] %s has no AndroidManifest.xml\n", apk_path);
    return -1;
  }
  int r = parse_axml(buf, len);
  mz_free(buf);
  if (r == 0) {
    M.loaded = 1;
    LOG("[manifest] %s %s (versionCode %d), %d meta-data entries\n", M.package,
                M.version_name, M.version_code, M.nmeta);
#ifdef RT_MANIFEST_LOG_PREFIX
    for (int i = 0; i < M.nmeta; i++)
      if (!strncmp(M.meta[i].name, RT_MANIFEST_LOG_PREFIX, strlen(RT_MANIFEST_LOG_PREFIX)))
        LOG("[manifest]   %s = %s%d\n", M.meta[i].name, M.meta[i].s,
                    M.meta[i].type == DCR_META_STRING ? 0 : M.meta[i].i);
#endif
  } else {
    LOG("[manifest] AndroidManifest.xml could not be parsed\n");
  }
  return r;
}

int dcr_manifest_probe(const char *apk_path) {
  g_quiet = 1;
  int r = dcr_manifest_load(apk_path);
  g_quiet = 0;
  return r;
}

int dcr_manifest_loaded(void) { return M.loaded; }
const char *dcr_manifest_package(void) { return M.package[0] ? M.package : PORT_PACKAGE; }
const char *dcr_manifest_version_name(void) { return M.version_name[0] ? M.version_name : "1.0"; }
int dcr_manifest_version_code(void) { return M.version_code ? M.version_code : 1; }

const DcrMeta *dcr_manifest_meta(const char *name) {
  for (int i = 0; i < M.nmeta; i++)
    if (!strcmp(M.meta[i].name, name))
      return &M.meta[i];
  return NULL;
}
int dcr_manifest_meta_count(void) { return M.nmeta; }
const DcrMeta *dcr_manifest_meta_at(int i) { return (i >= 0 && i < M.nmeta) ? &M.meta[i] : NULL; }
