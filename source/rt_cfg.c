/* rt_cfg.c -- <game folder>/config.ini, the user's settings: the engine.
 *
 * Written with every option, its default and a line of explanation on the
 * first start; an existing file is appended to (options a newer build adds,
 * at the end, with their defaults), so edits and comments survive updates.
 * A file of an older format ([config] version) is written again whole, the
 * player's values kept, after the port's migration rows have moved changed
 * defaults. Plain INI: [section], key = value, # comments; booleans take
 * true/false, yes/no, on/off, 1/0. Read once at start-up (and again after a
 * settings screen saves): changes apply the next time the game starts.
 *
 * The options are the port's (its dcr_config.c hands its table to
 * rt_config_load()); this is the machinery all seven ports had copies of,
 * from the Crossy Road port: Asphalt 8's (dst pointers, K_INT, a choice that
 * is not one falls back to the default's, get/set/save through a .part
 * file), Sonic's format version with its table of changed defaults, and
 * Crossy Road's "section.key" lookup for its C#. The resolution is read here
 * for every port alike (720, 1080, auto) and handed to the window. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

#include "dcr_build.h"
#include "dcr_path.h"  /* dcr_game_root */
#include "rt_cfg.h"
#include "rt_settings.h"
#include "rt_window.h" /* dcr_window_set_size */
#include "util.h"

/* The longest value kept (the ports had 16 to 48; Labyrinth 2's device id
 * is 16 hex digits, Asphalt 8's nickname 16 letters). */
#ifndef RT_CFG_VALUE_MAX
#define RT_CFG_VALUE_MAX 64
#endif

#define RT_CFG_HEADER                                                       \
  "# " PORT_TITLE " for Switch -- settings.\n"                              \
  "# Changes apply the next time the game starts. Delete this file to get\n" \
  "# the defaults back.\n"

static RtConfig g_rt = {RT_SCREEN_W, RT_SCREEN_H, 1, 0, 0, 0};

const RtConfig *rt_config(void) { return &g_rt; }

__attribute__((weak)) void port_config_new_file(void) {}

static const CfgTable *g_t;
static int g_n;                            /* rows: the table's, then the version's */
static char (*g_val)[RT_CFG_VALUE_MAX];
static unsigned char *g_have;              /* the file had it */
static CfgOpt g_version_row;
static char g_version_def[12];

static const CfgOpt *opt_at(int i) {
  return i < (int)g_t->nopts ? &g_t->opts[i] : &g_version_row;
}

static int opt_index(const char *section, const char *key) {
  if (!g_t || !section || !key)
    return -1;
  for (int i = 0; i < g_n; i++)
    if (!strcasecmp(opt_at(i)->section, section) && !strcasecmp(opt_at(i)->key, key))
      return i;
  return -1;
}

static void config_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/config.ini", dcr_game_root());
}

/* ------------------------------------------------------------- the file */
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  return s;
}

static void parse(FILE *f) {
  char line[256], section[32] = "";
  while (fgets(line, sizeof line, f)) {
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
      continue;
    if (*s == '[') {
      char *e = strchr(s, ']');
      if (e) {
        *e = 0;
        snprintf(section, sizeof section, "%s", trim(s + 1));
      }
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    char *hash = strpbrk(val, "#;");
    if (hash) {
      *hash = 0;
      val = trim(val);
    }
    for (int i = 0; i < g_n; i++)
      if (!strcasecmp(section, opt_at(i)->section) && !strcasecmp(key, opt_at(i)->key)) {
        snprintf(g_val[i], sizeof g_val[i], "%s", val);
        g_have[i] = 1;
      }
  }
}

static const char *section_intro(const char *section) {
  for (unsigned k = 0; k < g_t->nsections; k++)
    if (!strcmp(g_t->sections[k].section, section))
      return g_t->sections[k].intro;
  return NULL;
}

static void write_opts(FILE *f, int only_missing) {
  const char *last = NULL;
  for (int i = 0; i < g_n; i++) {
    const CfgOpt *o = opt_at(i);
    if (only_missing && g_have[i])
      continue;
    if (!last || strcmp(last, o->section)) {
      fprintf(f, "\n[%s]\n", o->section);
      const char *intro = section_intro(o->section);
      if (intro)
        fputs(intro, f);
    }
    last = o->section;
    if (o->help)
      fprintf(f, "# %s\n", o->help);
    fprintf(f, "%s = %s\n", o->key, g_val[i]);
  }
}

/* The whole file, through <path>.part: a half-written config.ini is never
 * left in place of a whole one. */
static int write_all(const char *path, const char *header) {
  char tmp[320];
  snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return -1;
  fputs(header, f);
  write_opts(f, 0);
  int ok = fclose(f) == 0;
  if (ok) {
    remove(path);
    ok = rename(tmp, path) == 0;
  }
  if (!ok) {
    remove(tmp);
    debugPrintf("[config] could not write %s\n", path);
    return -1;
  }
  return 0;
}

static const char *save_header(void) {
  return g_t->save_header ? g_t->save_header : g_t->header ? g_t->header : RT_CFG_HEADER;
}

/* An older format: the changed defaults moved, the file written again. */
static void migrate(const char *path) {
  if (g_t->version <= 0)
    return;
  const int iv = g_n - 1;
  const int ver = g_have[iv] ? atoi(g_val[iv]) : 1;
  if (ver >= g_t->version)
    return;
  char changed[256] = "";
  for (unsigned m = 0; m < g_t->nmigrate; m++) {
    const CfgMigrate *r = &g_t->migrate[m];
    const int below = r->below_version > 0 ? r->below_version : g_t->version;
    const int i = opt_index(r->section, r->key);
    if (ver >= below || i < 0 || (r->from && strcasecmp(g_val[i], r->from)) || !strcmp(g_val[i], r->to))
      continue;
    size_t n = strlen(changed);
    snprintf(changed + n, sizeof changed - n, " %s %s -> %s;", r->key, g_val[i], r->to);
    snprintf(g_val[i], sizeof g_val[i], "%s", r->to);
  }
  snprintf(g_val[iv], sizeof g_val[iv], "%d", g_t->version);
  if (write_all(path, save_header()) == 0) {
    memset(g_have, 1, (size_t)g_n);
    debugPrintf("[config] config.ini updated to format %d (your settings kept):%s\n", g_t->version,
                changed[0] ? changed : " no values changed");
  }
}

/* ------------------------------------------------------------ the values */
static int as_bool(int i) {
  const char *v = g_val[i];
  if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1"))
    return 1;
  if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"))
    return 0;
  debugPrintf("[config] %s = %s: not true/false, using %s\n", opt_at(i)->key, v, opt_at(i)->def);
  return !strcmp(opt_at(i)->def, "true");
}

/* index of the value in a comma-separated list, -1 if it is not one */
static int choice_of(const char *choices, const char *v) {
  const char *c = choices;
  for (int idx = 0; c && *c; idx++) {
    const char *e = strchr(c, ',');
    size_t n = e ? (size_t)(e - c) : strlen(c);
    if (strlen(v) == n && !strncasecmp(v, c, n))
      return idx;
    if (!e)
      break;
    c = e + 1;
  }
  return -1;
}

/* the index of the value; the default's when it is not one of the choices */
static int as_choice(int i) {
  const CfgOpt *o = opt_at(i);
  int r = choice_of(o->choices, g_val[i]);
  if (r >= 0)
    return r;
  if (strcasecmp(g_val[i], o->def))
    debugPrintf("[config] %s = %s: not one of %s, using %s\n", o->key, g_val[i],
                o->choices ? o->choices : "(none)", o->def);
  r = choice_of(o->choices, o->def);
  return r >= 0 ? r : 0;
}

static int as_int(int i) {
  const CfgOpt *o = opt_at(i);
  const char *v = g_val[i];
  if (o->choices) {
    if (choice_of(o->choices, v) >= 0)
      return atoi(v); /* a word ("system") is 0 */
    debugPrintf("[config] %s = %s: not one of %s, using %s\n", o->key, v, o->choices, o->def);
    return atoi(o->def);
  }
  char *end;
  long n = strtol(v, &end, 10);
  if (end == v || *end || (o->lo < o->hi && (n < (long)o->lo || n > (long)o->hi))) {
    if (o->lo < o->hi)
      debugPrintf("[config] %s = %s: not %g..%g, using %s\n", o->key, v, (double)o->lo, (double)o->hi, o->def);
    else
      debugPrintf("[config] %s = %s: not a number, using %s\n", o->key, v, o->def);
    return atoi(o->def);
  }
  return (int)n;
}

static float as_float(int i) {
  const CfgOpt *o = opt_at(i);
  float v = (float)atof(g_val[i]);
  if (o->lo < o->hi && !(v >= o->lo && v <= o->hi)) {
    debugPrintf("[config] %s = %s: not %g..%g, using %s\n", o->key, g_val[i], (double)o->lo, (double)o->hi,
                o->def);
    v = (float)atof(o->def);
  }
  return v;
}

static int as_kind(int i) {
  switch (opt_at(i)->kind) {
  case CFG_BOOL:
    return as_bool(i);
  case CFG_CHOICE:
    return as_choice(i);
  case CFG_INT:
    return as_int(i);
  case CFG_FLOAT:
    return (int)as_float(i);
  default:
    return atoi(g_val[i]);
  }
}

static int height_of(const char *r, int docked) {
  return !strcmp(r, "720") ? 720 : !strcmp(r, "1080") ? 1080 : !strcasecmp(r, "auto") ? (docked ? 1080 : 720) : 0;
}

static void rt_bool(const char *section, const char *key, int *out) {
  int i = opt_index(section, key);
  if (i >= 0)
    *out = as_bool(i);
}

/* g_val -> the dst fields, rt_config(), the window, then the port's apply */
static void apply(void) {
  for (int i = 0; i < g_n; i++) {
    const CfgOpt *o = opt_at(i);
    if (!o->dst)
      continue;
    if (o->kind == CFG_FLOAT)
      *(float *)o->dst = as_float(i);
    else if (o->kind == CFG_TEXT) {
      if (o->dst_cap)
        snprintf((char *)o->dst, o->dst_cap, "%s", g_val[i]);
    } else
      *(int *)o->dst = as_kind(i);
  }

  rt_bool("performance", "boost_cpu_when_loading", &g_rt.boost);
  rt_bool("debug", "gl_selftest", &g_rt.gl_selftest);
  rt_bool("debug", "boot_log_on_screen", &g_rt.boot_log);
  rt_bool("debug", "log_java_calls", &g_rt.log_jni);

  int i = opt_index("display", "resolution");
  if (i >= 0) {
    const char *r = g_val[i];
    int docked = appletGetOperationMode() == AppletOperationMode_Console;
    int h = height_of(r, docked);
    if (!h) {
      debugPrintf("[config] resolution = %s: not 720, 1080 or auto, using %s\n", r, opt_at(i)->def);
      h = height_of(opt_at(i)->def, docked);
      if (!h)
        h = RT_SCREEN_H;
    }
    g_rt.res_h = h;
    g_rt.res_w = h * 16 / 9;
    dcr_window_set_size(g_rt.res_w, g_rt.res_h);
  }

  if (g_t->apply)
    g_t->apply();
}

/* ---------------------------------------------------------------- load */
void rt_config_load(const CfgTable *t) {
  if (!t)
    return;
  const int n = (int)t->nopts + (t->version > 0);
  char(*val)[RT_CFG_VALUE_MAX] = calloc((size_t)n + 1, sizeof *val);
  unsigned char *have = calloc((size_t)n + 1, 1);
  if (!val || !have) {
    free(val);
    free(have);
    debugPrintf("[config] out of memory: the defaults stay\n");
    return;
  }
  free(g_val);
  free(g_have);
  g_val = val;
  g_have = have;
  g_t = t;
  g_n = n;
  snprintf(g_version_def, sizeof g_version_def, "%d", t->version);
  g_version_row = (CfgOpt){"config", "version", g_version_def, "Settings file format; leave as it is.",
                           CFG_INT, NULL, NULL, 0, 0, 0, 0};

  for (int i = 0; i < g_n; i++)
    snprintf(g_val[i], sizeof g_val[i], "%s", opt_at(i)->def);
  char path[300];
  config_path(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (f) {
    parse(f);
    fclose(f);
    migrate(path);
    int missing = 0;
    for (int i = 0; i < g_n; i++)
      missing += !g_have[i];
    if (missing && (f = fopen(path, "a"))) {
      fprintf(f, "\n# Added by build %llu (new options, at their defaults):\n", (unsigned long long)DCR_BUILD);
      write_opts(f, 1);
      fclose(f);
      debugPrintf("[config] added %d new option%s to config.ini\n", missing, missing > 1 ? "s" : "");
    }
  } else {
    port_config_new_file();
    if ((f = fopen(path, "w"))) {
      fputs(t->header ? t->header : RT_CFG_HEADER, f);
      write_opts(f, 0);
      fclose(f);
      debugPrintf("[config] wrote config.ini with the defaults\n");
    }
  }

  apply();
}

/* ------------------------------------------------------------- lookups */
const char *rt_config_get(const char *section, const char *key) {
  int i = opt_index(section, key);
  return i < 0 ? "" : g_val[i];
}

const char *rt_config_choices(const char *section, const char *key) {
  int i = opt_index(section, key);
  return i < 0 ? NULL : opt_at(i)->choices;
}

int rt_config_bool(const char *section, const char *key) {
  int i = opt_index(section, key);
  return i < 0 ? 0 : as_bool(i);
}

int rt_config_int(const char *section, const char *key) {
  int i = opt_index(section, key);
  return i < 0 ? 0 : as_kind(i);
}

float rt_config_float(const char *section, const char *key) {
  int i = opt_index(section, key);
  return i < 0 ? 0.0f : opt_at(i)->kind == CFG_FLOAT ? as_float(i) : (float)atof(g_val[i]);
}

int rt_config_value(const char *key, int dflt) {
  const char *dot = key ? strchr(key, '.') : NULL;
  if (!dot || !g_t)
    return dflt;
  for (int i = 0; i < g_n; i++) {
    const CfgOpt *o = opt_at(i);
    if (strlen(o->section) != (size_t)(dot - key) || strncasecmp(key, o->section, (size_t)(dot - key)) ||
        strcasecmp(dot + 1, o->key))
      continue;
    const char *v = g_val[i];
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
      return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off"))
      return 0;
    char *end;
    long n = strtol(v, &end, 10);
    return end != v ? (int)n : dflt;
  }
  return dflt;
}

/* ------------------------------------------------ a settings screen's edits */
int rt_config_set(const char *section, const char *key, const char *value) {
  int i = opt_index(section, key);
  if (i < 0 || !value || strlen(value) >= RT_CFG_VALUE_MAX || strpbrk(value, "\r\n#;"))
    return -1;
  const CfgOpt *o = opt_at(i);
  if (o->kind == CFG_BOOL && strcmp(value, "true") && strcmp(value, "false"))
    return -1;
  if ((o->kind == CFG_CHOICE || o->kind == CFG_INT) && o->choices && choice_of(o->choices, value) < 0)
    return -1;
  snprintf(g_val[i], sizeof g_val[i], "%s", value);
  g_have[i] = 1;
  return 0;
}

int rt_config_save(void) {
  if (!g_t)
    return -1;
  char path[300];
  config_path(path, sizeof path);
  if (write_all(path, save_header()) != 0)
    return -1;
  apply();
  return 0;
}

/* The names the ports call today (weak: a port that still defines its own
 * keeps it while it migrates). */
__attribute__((weak)) int dcr_config_value(const char *key, int dflt) { return rt_config_value(key, dflt); }
__attribute__((weak)) const char *dcr_config_get(const char *section, const char *key) {
  return rt_config_get(section, key);
}
__attribute__((weak)) const char *dcr_config_choices(const char *section, const char *key) {
  return rt_config_choices(section, key);
}
__attribute__((weak)) int dcr_config_set(const char *section, const char *key, const char *value) {
  return rt_config_set(section, key, value);
}
__attribute__((weak)) int dcr_config_save(void) { return rt_config_save(); }
