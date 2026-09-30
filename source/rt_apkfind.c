/* rt_apkfind.c -- the player's APKs in the game folder, whatever they are
 * called, told apart by what is in them.
 *
 * Players copy their APK in under the name it downloaded with. So every
 * *.apk in the folder (with RT_APK_ANY_FILE: every file that is not one of
 * the port's own) is looked into, and the port's role table
 * (PORT_APK_ROLES) says what each kind holds:
 *   need / reject   zip entries that must / must not be there (the game's
 *                   library; a translation pak that marks another build);
 *   package         the manifest's package, exact or as a prefix, or only as
 *                   a preference (RT_APK_PACKAGE_BONUS);
 *   check           anything else, a callback.
 * A file is the first role it matches. Of several for one role the best is
 * taken: the role's own name (for an adopting role, a newly copied file:
 * that is an update), a package match, the wanted versionCode, the highest
 * versionCode (RT_APK_HIGHEST_VERSION), an .apk name, the newest file, the
 * first by name. An adopting role (RT_APK_ADOPT) then renames its APK to its
 * name, keeping the one before as <name>.previous and other copies as
 * *.unused, so that none is taken later.
 *
 * The zip is only looked at through its central directory (end record in
 * the last 64 KB); nothing is unpacked. The package comes from the caller's
 * manifest reader (the game program's dcr_manifest.c); the launcher has none,
 * so there it is not checked, and a file whose only test is its package is
 * taken as it is but never renamed. Files starting with '.' (macOS's "._"
 * copies), folders and empty files are never looked at. MIT.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "rt_apkfind.h"

/* ------------------------------------------------------------- zip files */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static int read_at(FILE *f, uint64_t off, void *buf, size_t n) {
  if (fseeko(f, (off_t)off, SEEK_SET) != 0)
    return -1;
  return fread(buf, 1, n, f) == n ? 0 : -1;
}

int rt_zip_walk(FILE *f, uint64_t base, uint64_t size, RtZipEntryFn cb, void *ctx) {
  if (!f || size < 22)
    return -1;
  /* the end-of-central-directory record: within the last 64 KB + 22 */
  uint32_t tail = size < 65557u ? (uint32_t)size : 65557u;
  uint8_t *t = malloc(tail);
  if (!t || read_at(f, base + size - tail, t, tail) != 0) {
    free(t);
    return -1;
  }
  int64_t eocd = -1;
  for (int64_t i = (int64_t)tail - 22; i >= 0; i--)
    if (t[i] == 'P' && rd32(t + i) == 0x06054b50u) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    free(t);
    return -1;
  }
  uint32_t count = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
  free(t);
  if ((uint64_t)cd_off + cd_size > size || cd_size > (64u << 20))
    return -1;
  uint8_t *cd = malloc(cd_size ? cd_size : 1);
  if (!cd || read_at(f, base + cd_off, cd, cd_size) != 0) {
    free(cd);
    return -1;
  }
  RtZipEntry e;
  uint32_t p = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (p + 46 > cd_size || rd32(cd + p) != 0x02014b50u)
      break;
    uint32_t nlen = rd16(cd + p + 28), xlen = rd16(cd + p + 30), clen = rd16(cd + p + 32);
    if (p + 46 + nlen > cd_size)
      break;
    e.name_len = nlen;
    size_t n = nlen < sizeof e.name - 1 ? nlen : sizeof e.name - 1;
    memcpy(e.name, cd + p + 46, n);
    e.name[n] = 0;
    e.method = (int)rd16(cd + p + 10);
    e.crc = rd32(cd + p + 16);
    e.comp_size = rd32(cd + p + 20);
    e.size = rd32(cd + p + 24);
    e.lho = rd32(cd + p + 42);
    if (cb(&e, ctx))
      break;
    p += 46 + nlen + xlen + clen;
  }
  free(cd);
  return 0;
}

int rt_zip_walk_path(const char *path, RtZipEntryFn cb, void *ctx) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  int rc = -1;
  if (fseeko(f, 0, SEEK_END) == 0) {
    off_t size = ftello(f);
    if (size > 0)
      rc = rt_zip_walk(f, 0, (uint64_t)size, cb, ctx);
  }
  fclose(f);
  return rc;
}

uint64_t rt_zip_data_offset(FILE *f, uint64_t base, const RtZipEntry *e) {
  uint8_t lh[30];
  if (read_at(f, base + e->lho, lh, sizeof lh) != 0 || rd32(lh) != 0x04034b50u)
    return 0;
  return base + e->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
}

typedef struct {
  const char *want;
  size_t len;
  int found;
} HasCtx;

static int has_cb(const RtZipEntry *e, void *ctx) {
  HasCtx *h = ctx;
  if (e->name_len == h->len && !memcmp(e->name, h->want, h->len))
    h->found = 1;
  return h->found;
}

int rt_zip_has(const char *path, const char *entry) {
  HasCtx h = {entry, strlen(entry), 0};
  return rt_zip_walk_path(path, has_cb, &h) == 0 && h.found;
}

/* ------------------------------------------------------------- the APKs */
#define MAX_NAMES 64 /* need + reject entries over all the roles */

typedef struct {
  char *name;
  long long size;
  time_t mtime;
  int zip;          /* 1 a zip, 0 not, -1 not looked at yet */
  uint64_t hits;    /* the entries of the names table it has */
  int manifest;     /* 1 read, 0 not yet, -1 unreadable */
  char package[128];
  int vcode;
  int role;         /* the role it is, or -1 */
  int verified;     /* its package was checked */
  int pkg_match;
} Cand;

typedef struct {
  const char *names[MAX_NAMES];
  size_t lens[MAX_NAMES];
  int n;
  uint64_t hits;
} Want;

static int want_cb(const RtZipEntry *e, void *ctx) {
  Want *w = ctx;
  for (int i = 0; i < w->n; i++)
    if (e->name_len == w->lens[i] && !memcmp(e->name, w->names[i], w->lens[i]))
      w->hits |= 1ull << i;
  return 0;
}

static int want_index(Want *w, const char *name) {
  for (int i = 0; i < w->n; i++)
    if (!strcmp(w->names[i], name))
      return i;
  if (w->n == MAX_NAMES)
    return -1;
  w->names[w->n] = name;
  w->lens[w->n] = strlen(name);
  return w->n++;
}

static int ends_with(const char *s, const char *tail) {
  size_t n = strlen(s), t = strlen(tail);
  return n >= t && !strcasecmp(s + n - t, tail);
}

/* the port's own files: never the player's APK */
static int port_file(const char *name) {
  static const char *const own[] = {".nro", ".nsp", ".ini", ".log", ".txt", ".so", ".setup", ".update",
                                    ".rgba", ".ttf", ".part", ".previous", ".unused", ".build"};
  for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
    if (ends_with(name, own[i]))
      return 1;
  return 0;
}

static int same_name(const char *a, const char *b) { return a && b && !strcasecmp(a, b); }

static void say(const RtApkEnv *env, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const RtApkEnv *env, const char *fmt, ...) {
  if (!env || !env->log)
    return;
  char buf[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  env->log("%s", buf);
}

/* onto a list (sep between items), while it fits */
static void addf(char *list, size_t cap, const char *sep, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void addf(char *list, size_t cap, const char *sep, const char *fmt, ...) {
  size_t n = strlen(list);
  if (n + 8 >= cap)
    return;
  if (n)
    n += (size_t)snprintf(list + n, cap - n, "%s", sep);
  if (n + 1 >= cap)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(list + n, cap - n, fmt, ap);
  va_end(ap);
}

/* Does c hold what role r needs? (entries, then check(), then the package) */
static int is_role(const RtApkRole *r, const RtApkRole *roles, int nroles, Cand *c, const char *path,
                   Want *w, const RtApkEnv *env) {
  int apk = ends_with(c->name, ".apk");
  if (!apk && !(r->flags & RT_APK_ANY_FILE))
    return 0;
  if (c->zip < 0) {
    w->hits = 0;
    c->zip = rt_zip_walk_path(path, want_cb, w) == 0;
    c->hits = w->hits;
  }
  if (!c->zip)
    return 0;
  for (int i = 0; r->need && r->need[i]; i++) {
    int k = want_index(w, r->need[i]);
    if (k < 0 || !(c->hits >> k & 1))
      return 0;
  }
  for (int i = 0; r->reject && r->reject[i]; i++) {
    int k = want_index(w, r->reject[i]);
    if (k >= 0 && (c->hits >> k & 1))
      return 0;
  }
  if (r->check && !r->check(path))
    return 0;
  c->verified = 1;
  c->pkg_match = 0;
  int wants_manifest = r->package || r->version_code || (r->flags & RT_APK_HIGHEST_VERSION);
  if (wants_manifest && env && env->manifest && !c->manifest)
    c->manifest = env->manifest(path, c->package, sizeof c->package, &c->vcode) == 0 ? 1 : -1;
  if (!r->package)
    return 1;
  if (!env || !env->manifest) {
    /* no manifest reader: taken unchecked, unless it has another role's name */
    for (int i = 0; i < nroles; i++)
      if (&roles[i] != r && same_name(c->name, roles[i].name))
        return 0;
    c->verified = 0;
    return 1;
  }
  size_t pl = strlen(r->package);
  c->pkg_match = c->manifest > 0 && ((r->flags & RT_APK_PACKAGE_PREFIX) ? !strncmp(c->package, r->package, pl)
                                                                         : !strcmp(c->package, r->package));
  return c->pkg_match || (r->flags & RT_APK_PACKAGE_BONUS);
}

/* >0: a is the better one for r */
static int better(const RtApkRole *r, const Cand *a, const Cand *b) {
  int an = same_name(a->name, r->name), bn = same_name(b->name, r->name);
  if (an != bn) /* an adopting role takes a new copy over its own (an update) -- when it can tell */
    return (r->flags & RT_APK_ADOPT) && a->verified && b->verified ? bn - an : an - bn;
  if (a->pkg_match != b->pkg_match)
    return a->pkg_match - b->pkg_match;
  if (r->version_code) {
    int av = a->vcode == r->version_code, bv = b->vcode == r->version_code;
    if (av != bv)
      return av - bv;
  }
  if ((r->flags & RT_APK_HIGHEST_VERSION) && a->vcode != b->vcode)
    return a->vcode > b->vcode ? 1 : -1;
  int aa = ends_with(a->name, ".apk"), ba = ends_with(b->name, ".apk");
  if (aa != ba)
    return aa - ba;
  if (a->mtime != b->mtime)
    return a->mtime > b->mtime ? 1 : -1;
  return strcasecmp(b->name, a->name);
}

/* A file was renamed: the candidates follow it. */
static void renamed(Cand *c, int n, const char *from, const char *to) {
  for (int i = 0; i < n; i++)
    if (same_name(c[i].name, from)) {
      char *t = strdup(to);
      if (t) {
        free(c[i].name);
        c[i].name = t;
      }
    }
}

#define NAME_MAX_ 320 /* a file name, with .previous / .unused */

static int rename_in(const char *root, const char *from, const char *to) {
  char a[RT_APK_PATH_MAX + NAME_MAX_], b[RT_APK_PATH_MAX + NAME_MAX_];
  snprintf(a, sizeof a, "%s/%s", root, from);
  snprintf(b, sizeof b, "%s/%s", root, to);
  remove(b); /* only ever a .previous or .unused of ours */
  return rename(a, b);
}

/* c[best] becomes role->name; the file of that name, if any, name.previous;
 * the role's other copies *.unused. 0, or -1 when it could not be renamed. */
static int adopt(const char *root, const RtApkRole *role, int r, Cand *c, int n, int best, RtApkFound *out,
                 const RtApkEnv *env) {
  char path[RT_APK_PATH_MAX + NAME_MAX_], prev[NAME_MAX_], from[NAME_MAX_];
  struct stat st;
  snprintf(path, sizeof path, "%s/%s", root, role->name);
  snprintf(prev, sizeof prev, "%.300s.previous", role->name);
  snprintf(from, sizeof from, "%.300s", c[best].name);
  int aside = stat(path, &st) == 0;
  if (aside) {
    if (rename_in(root, role->name, prev) != 0) {
      addf(out->adopted, sizeof out->adopted, " ", "Could not rename %s to make way for %s.", role->name, from);
      say(env, "[apk] could NOT rename %s to make way for %s: %s used as it is\n", role->name, from, from);
      return -1;
    }
    renamed(c, n, role->name, prev);
  }
  if (rename_in(root, from, role->name) != 0) {
    addf(out->adopted, sizeof out->adopted, " ", "Could not rename %s to %s.", from, role->name);
    say(env, "[apk] could NOT rename %s to %s: used as it is\n", from, role->name);
    return -1;
  }
  renamed(c, n, from, role->name);
  addf(out->adopted, sizeof out->adopted, " ", "%s is %s: now %s%s%s%s.", from, role->what, role->name,
       aside ? " (the one before is " : "", aside ? prev : "", aside ? ")" : "");
  say(env, "[apk] %s is %s: renamed %s%s%s\n", from, role->what, role->name, aside ? ", the one before " : "",
      aside ? prev : "");
  for (int i = 0; i < n; i++) /* other copies: never taken later */
    if (i != best && c[i].role == r && c[i].verified && !same_name(c[i].name, role->name) &&
        ends_with(c[i].name, ".apk")) {
      char was[NAME_MAX_ - 16], un[NAME_MAX_];
      snprintf(was, sizeof was, "%.300s", c[i].name);
      snprintf(un, sizeof un, "%.300s.unused", was);
      if (rename_in(root, was, un) == 0) {
        renamed(c, n, was, un);
        addf(out->adopted, sizeof out->adopted, " ", "%s is older: renamed %s.", was, un);
        say(env, "[apk] %s is older: renamed %s\n", was, un);
      }
    }
  return 0;
}

int rt_apk_find(const char *root, const RtApkRole *roles, int nroles, const RtApkEnv *env, RtApkFound *out) {
  memset(out, 0, sizeof *out);
  if (nroles > RT_APK_ROLES_MAX)
    nroles = RT_APK_ROLES_MAX;
  int any_file = 0;
  for (int i = 0; i < nroles; i++)
    any_file |= roles[i].flags & RT_APK_ANY_FILE;

  /* the names first: an adoption renames while the folder is being read */
  DIR *d = opendir(root);
  Cand *c = NULL;
  int n = 0, cap = 0;
  struct dirent *e;
  while (d && (e = readdir(d))) {
    const char *nm = e->d_name;
    if (nm[0] == '.' || !(ends_with(nm, ".apk") || (any_file && !port_file(nm))))
      continue;
    char p[RT_APK_PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, nm);
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
      continue;
    if (n == cap) {
      int nc = cap ? cap * 2 : 16;
      Cand *g = realloc(c, (size_t)nc * sizeof *g);
      if (!g)
        break;
      c = g, cap = nc;
    }
    memset(&c[n], 0, sizeof c[n]);
    c[n].name = strdup(nm);
    if (!c[n].name)
      continue;
    c[n].size = (long long)st.st_size;
    c[n].mtime = st.st_mtime;
    c[n].zip = -1;
    c[n].role = -1;
    n++;
  }
  if (d)
    closedir(d);

  /* what each file is */
  Want w;
  memset(&w, 0, sizeof w);
  for (int r = 0; r < nroles; r++) { /* every entry of interest, before the first walk */
    for (int i = 0; roles[r].need && roles[r].need[i]; i++)
      want_index(&w, roles[r].need[i]);
    for (int i = 0; roles[r].reject && roles[r].reject[i]; i++)
      want_index(&w, roles[r].reject[i]);
  }
  for (int i = 0; i < n; i++) {
    char p[RT_APK_PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", root, c[i].name);
    for (int r = 0; r < nroles && c[i].role < 0; r++)
      if (is_role(&roles[r], roles, nroles, &c[i], p, &w, env))
        c[i].role = r;
    const int apk = ends_with(c[i].name, ".apk");
    if (c[i].role < 0 && c[i].zip < 0 && apk) /* no role could look at it (none take .apk) */
      c[i].zip = rt_zip_walk_path(p, want_cb, &w) == 0;
    if (c[i].role < 0 && !apk)
      continue; /* RT_APK_ANY_FILE: some other file of the player's */
    const char *what = c[i].role >= 0 ? (c[i].verified ? roles[c[i].role].what : "an APK, not checked")
                       : c[i].zip > 0     ? "not the game"
                                          : "not a readable zip";
    say(env, "[apk] %s: %s (%lld MB)\n", c[i].name, what, c[i].size >> 20);
    addf(out->summary, sizeof out->summary, ", ", "%s (%s)", c[i].name, what);
  }

  /* each role's best, adopted when the role says so */
  int rc = 0;
  for (int r = 0; r < nroles; r++) {
    const RtApkRole *role = &roles[r];
    int best = -1;
    for (int i = 0; i < n; i++)
      if (c[i].role == r) {
        out->count[r]++;
        if (best < 0 || better(role, &c[i], &c[best]) > 0)
          best = i;
      }
    if (best < 0) {
      if (!(role->flags & RT_APK_OPTIONAL))
        rc = -1;
      continue;
    }
    if (env && env->adopt && (role->flags & RT_APK_ADOPT) && role->name && c[best].verified &&
        !same_name(c[best].name, role->name))
      adopt(root, role, r, c, n, best, out, env);
    const char *use = c[best].name;
    snprintf(out->path[r], sizeof out->path[r], "%s/%s", root, use);
    out->verified[r] = c[best].verified;
    say(env, "[apk] %s: %s%s\n", role->what, use, c[best].verified ? "" : " (package not checked)");
  }
  for (int i = 0; i < n; i++)
    free(c[i].name);
  free(c);
  return rc;
}
