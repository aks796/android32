/* dcr_config.c -- <game folder>/config.ini, the user's settings.
 *
 * Written with every option, its default and a line of explanation on the
 * first start; an existing file is appended to (options a newer build adds,
 * at the end, with their defaults), so edits and comments survive updates.
 * Plain INI: [section], key = value, # comments; booleans take true/false,
 * yes/no, on/off, 1/0. Read once at start-up: changes apply the next time the
 * game starts. (The machinery is the Crossy Road port's; the options are
 * Labyrinth 2's.) MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_build.h"
#include "dcr_config.h"
#include "util.h"

const char *dcr_game_root(void);        /* main.c */
void dcr_window_set_size(int w, int h); /* android_ndk.c */

static DcrConfig g_cfg = {
    .tilt = LAB_TILT_STICK,
    .stick_tilt = 0.5f,
    .motion_gain = 1.0f,
    .pointer_speed = 5.0f,
    .rumble = 1,
    .supersample = 2,
    .side_panels = 1,
    .res_w = 1280,
    .res_h = 720,
    .boost = 1,
    .online = 1,
    .level_layout = LAB_LAYOUT_ROTATED_LEFT,
};

const DcrConfig *dcr_config(void) { return &g_cfg; }
void dcr_config_set_tilt(int tilt) {
  if (tilt >= LAB_TILT_STICK && tilt <= LAB_TILT_BOTH)
    g_cfg.tilt = tilt;
}

void dcr_config_set_level_layout(int layout) {
  /* upright, or turned with the board's top at the left (its top at the
   * right showed the board upside down to the player: not offered) */
  if (layout >= LAB_LAYOUT_PORTRAIT && layout <= LAB_LAYOUT_ROTATED_RIGHT)
    g_cfg.level_layout = layout == LAB_LAYOUT_PORTRAIT ? LAB_LAYOUT_PORTRAIT : LAB_LAYOUT_ROTATED_LEFT;
}

enum { K_BOOL, K_CHOICE, K_TEXT };

typedef struct {
  const char *section, *key, *def, *help;
  int kind;
  const char *choices; /* K_CHOICE: comma-separated, index = value */
} Opt;

static const Opt k_opts[] = {
    {"controls", "swap_a_b", "false",
     "Swap A and B (true: B selects, A goes back).", K_BOOL, NULL},
    {"controls", "tilt", "stick",
     "What tilts the board. stick: the left stick (the D-pad tilts all the way).\n"
     "# motion: the controller's motion sensors -- hold it (or the console, in\n"
     "# handheld) flat like the board, and tilt it; Settings > Calibrate sets\n"
     "# the level position. both: motion, and the stick adds to it. The game's\n"
     "# Settings screen changes this too, and ZL in a level (motion on / off);\n"
     "# then it is kept with the saves.",
     K_CHOICE, "stick,motion,both"},
    {"controls", "stick_tilt", "0.5",
     "How far the board tilts with the stick all the way over, in g (the pull\n"
     "# a phone tilted 30 degrees gives the ball): 0.1 to 1.", K_TEXT, NULL},
    {"controls", "motion_sensitivity", "1.0",
     "Motion tilt: 1 = as far as the controller is tilted, 2 = twice as far\n"
     "# (0.25 to 4).", K_TEXT, NULL},
    {"controls", "pointer_speed", "5",
     "Speed of the pointer on the game's own screens (pause, level end), which\n"
     "# are touch screens: 1 to 20.", K_TEXT, NULL},
    {"controls", "rumble", "true",
     "Rumble when the ball drops into a hole, reaches the goal or hits a bumper.", K_BOOL, NULL},
    {"display", "resolution", "720",
     "Rendering resolution: 720, 1080 or auto (1080 if docked when the game\n"
     "# starts).", K_CHOICE, NULL},
    {"display", "layout", "portrait",
     "The game is a portrait (upright) phone game. portrait: upright in the\n"
     "# middle of the screen, the side panels beside it. rotated_left /\n"
     "# rotated_right: turned a quarter to fill the screen, for a console held\n"
     "# upright (its left / right side at the top; touch and tilt turn with it).",
     K_CHOICE, "portrait,rotated_left,rotated_right"},
    {"display", "level_layout", "rotated_left",
     "With layout = portrait: how a level is shown while it is played (the\n"
     "# menus stay upright). rotated_left: the board turned a quarter to fill\n"
     "# the screen, its top at the left (the stick and motion turn with it; the\n"
     "# game's pause and end-of-level screens turn back upright to be read).\n"
     "# portrait: upright, as the menus. ZR in a level changes it (kept with\n"
     "# the saves).",
     K_CHOICE, "portrait,rotated_left"},
    {"display", "supersample", "auto",
     "Draw the game at twice the size and scale it down (smoother edges).\n"
     "# auto: at 720p yes, at 1080p no. true / false.", K_TEXT, NULL},
    {"display", "menus", "ipad",
     "The menus: ipad = Labyrinth 2 HD's (landscape; needs the iPad game's .ipa\n"
     "# in this folder: its pictures, its iPad level packs); android = the phone's\n"
     "# (portrait).", K_CHOICE, "ipad,android"},
    {"display", "side_panels", "true",
     "Portrait layout: the blue background and the controls beside the game\n"
     "# (false: black).", K_BOOL, NULL},
    {"online", "enabled", "true",
     "The Labyrinth 2 level server: Download levels (the community's level\n"
     "# packs, their ratings) and Create (your own packs from the web editor\n"
     "# at labyrinth2.com, and publishing them). false: offline.", K_BOOL, NULL},
    {"online", "device_id", "",
     "Leave empty. The server keeps one account (your ID and PIN) per device;\n"
     "# the port makes this console's id once (from its serial number, else its\n"
     "# user, never shared with another console) and keeps it in\n"
     "# data/device_id. To use the account of another device, put its id here\n"
     "# (8 to 16 hex digits, e.g. a phone's Android ID).", K_TEXT, NULL},
    {"performance", "boost_cpu_when_loading", "true",
     "CPU at 1785 MHz while the game starts (until its first picture) and\n"
     "# inside loading frames (those over 50 ms), normal otherwise.",
     K_BOOL, NULL},
    {"debug", "gl_selftest", "false", "Graphics self-test picture at start-up.", K_BOOL, NULL},
    {"debug", "boot_log_on_screen", "false",
     "Show the start-up log on screen at every launch. Off: the log appears only\n"
     "# while something is being set up (first launch, a new APK or NRO).",
     K_BOOL, NULL},
    {"debug", "log_java_calls", "false",
     "Write every Java method the game calls to debug.log (slow; for bug reports).", K_BOOL,
     NULL},
    {"debug", "log_touches", "false",
     "Write every touch the game gets (its 320x480 coordinates) to debug.log.", K_BOOL, NULL},
    {"config", "version", "1", "Settings file format; leave as it is.", K_TEXT, NULL},
};
#define O_COUNT ((int)(sizeof k_opts / sizeof k_opts[0]))

static char g_val[O_COUNT][48];
static int g_have[O_COUNT];

static int opt_index(const char *section, const char *key) {
  for (int i = 0; i < O_COUNT; i++)
    if (!strcmp(k_opts[i].section, section) && !strcmp(k_opts[i].key, key))
      return i;
  return -1;
}

static void path_of(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s/%s", dcr_game_root(), name);
}

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
    for (int i = 0; i < O_COUNT; i++)
      if (!strcasecmp(section, k_opts[i].section) && !strcasecmp(key, k_opts[i].key)) {
        snprintf(g_val[i], sizeof g_val[i], "%s", val);
        g_have[i] = 1;
      }
  }
}

static void write_opts(FILE *f, int only_missing) {
  const char *last = NULL;
  for (int i = 0; i < O_COUNT; i++) {
    if (only_missing && g_have[i])
      continue;
    if (!last || strcmp(last, k_opts[i].section)) {
      fprintf(f, "\n[%s]\n", k_opts[i].section);
    }
    last = k_opts[i].section;
    if (k_opts[i].help)
      fprintf(f, "# %s\n", k_opts[i].help);
    fprintf(f, "%s = %s\n", k_opts[i].key, g_val[i]);
  }
}

static int as_bool(int i) {
  const char *v = g_val[i];
  if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1"))
    return 1;
  if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"))
    return 0;
  debugPrintf("[config] %s = %s: not true/false, using %s\n", k_opts[i].key, v, k_opts[i].def);
  return !strcmp(k_opts[i].def, "true");
}

/* index of the value in the option's choice list, 0 (the first) if unknown */
static int as_choice(int i) {
  const char *v = g_val[i];
  const char *c = k_opts[i].choices;
  for (int idx = 0; c && *c; idx++) {
    const char *e = strchr(c, ',');
    size_t n = e ? (size_t)(e - c) : strlen(c);
    if (strlen(v) == n && !strncasecmp(v, c, n))
      return idx;
    if (!e)
      break;
    c = e + 1;
  }
  if (strcasecmp(v, k_opts[i].def))
    debugPrintf("[config] %s = %s: not one of %s, using %s\n", k_opts[i].key, v,
                k_opts[i].choices, k_opts[i].def);
  return 0;
}

static float as_float(int i, float lo, float hi) {
  float v = (float)atof(g_val[i]);
  if (!(v >= lo && v <= hi)) {
    debugPrintf("[config] %s = %s: not %g..%g, using %s\n", k_opts[i].key, g_val[i], lo, hi,
                k_opts[i].def);
    v = (float)atof(k_opts[i].def);
  }
  return v;
}

void dcr_config_load(void) {
  for (int i = 0; i < O_COUNT; i++)
    snprintf(g_val[i], sizeof g_val[i], "%s", k_opts[i].def);
  char path[300];
  path_of(path, sizeof path, "config.ini");
  FILE *f = fopen(path, "r");
  if (f) {
    parse(f);
    fclose(f);
    int missing = 0;
    for (int i = 0; i < O_COUNT; i++)
      missing += !g_have[i];
    if (missing && (f = fopen(path, "a"))) {
      fprintf(f, "\n# Added by build %llu (new options, at their defaults):\n",
              (unsigned long long)DCR_BUILD);
      write_opts(f, 1);
      fclose(f);
      debugPrintf("[config] added %d new option%s to config.ini\n", missing, missing > 1 ? "s" : "");
    }
  } else if ((f = fopen(path, "w"))) {
    fputs("# Labyrinth 2 for Switch -- settings.\n"
          "# Changes apply the next time the game starts. Delete this file to get\n"
          "# the defaults back.\n",
          f);
    write_opts(f, 0);
    fclose(f);
    debugPrintf("[config] wrote config.ini with the defaults\n");
  }

  g_cfg.swap_ab = as_bool(opt_index("controls", "swap_a_b"));
  g_cfg.tilt = as_choice(opt_index("controls", "tilt"));
  g_cfg.stick_tilt = as_float(opt_index("controls", "stick_tilt"), 0.1f, 1.0f);
  g_cfg.motion_gain = as_float(opt_index("controls", "motion_sensitivity"), 0.25f, 4.0f);
  g_cfg.pointer_speed = as_float(opt_index("controls", "pointer_speed"), 1.0f, 20.0f);
  g_cfg.rumble = as_bool(opt_index("controls", "rumble"));
  g_cfg.layout = as_choice(opt_index("display", "layout"));
  dcr_config_set_level_layout(as_choice(opt_index("display", "level_layout")));
  g_cfg.side_panels = as_bool(opt_index("display", "side_panels"));
  g_cfg.menus = as_choice(opt_index("display", "menus"));
  g_cfg.online = as_bool(opt_index("online", "enabled"));
  snprintf(g_cfg.device_id, sizeof g_cfg.device_id, "%s", g_val[opt_index("online", "device_id")]);
  g_cfg.boost = as_bool(opt_index("performance", "boost_cpu_when_loading"));
  g_cfg.gl_selftest = as_bool(opt_index("debug", "gl_selftest"));
  g_cfg.boot_log = as_bool(opt_index("debug", "boot_log_on_screen"));
  g_cfg.log_jni = as_bool(opt_index("debug", "log_java_calls"));
  g_cfg.log_touch = as_bool(opt_index("debug", "log_touches"));

  const char *r = g_val[opt_index("display", "resolution")];
  int docked = appletGetOperationMode() == AppletOperationMode_Console;
  int h = !strcmp(r, "720") ? 720 : !strcmp(r, "1080") ? 1080 : !strcasecmp(r, "auto") ? (docked ? 1080 : 720) : 0;
  if (!h) {
    debugPrintf("[config] resolution = %s: not 720, 1080 or auto, using 720\n", r);
    h = 720;
  }
  g_cfg.res_h = h;
  g_cfg.res_w = h * 16 / 9;
  dcr_window_set_size(g_cfg.res_w, g_cfg.res_h);
  {
    int i = opt_index("display", "supersample");
    const char *v = g_val[i];
    if (!strcasecmp(v, "auto"))
      g_cfg.supersample = h <= 720 ? 2 : 1;
    else
      g_cfg.supersample = as_bool(i) ? 2 : 1;
  }

  static const char *const tilts[] = {"stick", "motion", "both"};
  static const char *const layouts[] = {"portrait", "rotated left", "rotated right"};
  debugPrintf("[config] %dx%d (%s, %s), %s (levels %s), supersample %dx, side panels %s; A/B %s; tilt %s "
              "(stick %.2f g, motion x%.2f), pointer %.0f; CPU boost %s\n",
              g_cfg.res_w, g_cfg.res_h, r, docked ? "docked" : "handheld", layouts[g_cfg.layout],
              layouts[g_cfg.level_layout],
              g_cfg.supersample, g_cfg.side_panels ? "on" : "off", g_cfg.swap_ab ? "swapped" : "normal",
              tilts[g_cfg.tilt], (double)g_cfg.stick_tilt, (double)g_cfg.motion_gain,
              (double)g_cfg.pointer_speed, g_cfg.boost ? "on" : "off");
}
