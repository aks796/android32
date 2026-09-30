/* rt_cfg.h -- <game folder>/config.ini: the INI engine every port's settings
 * are built on, and the runtime's own view of them (RtConfig).
 *
 * The port keeps its settings in its own dcr_config.h / dcr_config.c: its
 * DcrConfig struct, a CfgOpt table (the file's options, in the file's order)
 * and dcr_config_load(), which hands that table to rt_config_load(). The
 * engine writes config.ini from the table on the first start, appends the
 * options a newer build adds ("# Added by build ..."), upgrades an older file
 * format through the table's migration rows, fills the port's fields through
 * the rows' dst pointers, then calls the table's apply() for whatever else
 * the port derives. Values are kept and written as the strings the player
 * sees ("auto", "true"), never as indexes, so every port's existing
 * config.ini reads the same as before.
 *
 * Runtime files read only rt_config(): the resolution, the CPU boost and the
 * debug switches, found in the port's table by section and key (the
 * CFG_ROW_* rows below spell them). MIT.
 */
#ifndef RT_CFG_H
#define RT_CFG_H

/* What the runtime itself reads. Filled by rt_config_load() from the rows
 * [display] resolution, [performance] boost_cpu_when_loading, [debug]
 * gl_selftest, boot_log_on_screen and log_java_calls (a row the port's table
 * does not have keeps its default: 1280x720, boost on, the rest off). */
typedef struct RtConfig {
  int res_w, res_h, boost, gl_selftest, boot_log, log_jni;
} RtConfig;

const RtConfig *rt_config(void);

/* ------------------------------------------------------------ the table */
typedef enum {
  CFG_BOOL,   /* true/yes/on/1, false/no/off/0 -> int 1/0 */
  CFG_CHOICE, /* one of `choices` -> int, its index; unknown: the default's */
  CFG_INT,    /* int; `choices` lists the allowed values (a word counts as 0:
               * "system,1020,1224"), else lo..hi when lo < hi, else any */
  CFG_FLOAT,  /* float, lo..hi */
  CFG_TEXT    /* the string itself, into char[dst_cap] */
} CfgKind;

typedef struct CfgOpt {
  const char *section, *key, *def;
  const char *help;    /* "# " + this above the key; more lines as "\n# ..." */
  CfgKind kind;
  const char *choices; /* comma-separated, see CfgKind */
  void *dst;           /* int* (BOOL, CHOICE, INT), float* (FLOAT), char* (TEXT);
                        * NULL: not applied (read it with rt_config_get & co.) */
  float lo, hi;        /* FLOAT, INT without choices */
  unsigned dst_cap;    /* TEXT */
  int tag;             /* the port's own use (a feature id, an index) */
} CfgOpt;

/* A changed default: in a file whose [config] version is below
 * `below_version` (0: below the table's version), the option is moved from
 * `from` to `to`. from NULL: whatever it holds. A file without a version line
 * counts as version 1. The file is then written again whole, through a .part
 * file (the player's values kept, this build's help text). */
typedef struct CfgMigrate {
  const char *section, *key, *from, *to;
  int below_version;
} CfgMigrate;

/* Lines written under a section's [header], whenever the header is written. */
typedef struct CfgSection {
  const char *section, *intro;
} CfgSection;

typedef struct CfgTable {
  const CfgOpt *opts;
  unsigned nopts;
  const CfgSection *sections;
  unsigned nsections;
  const CfgMigrate *migrate;
  unsigned nmigrate;
  /* The file format. The engine writes it last, as
   *   [config]
   *   # Settings file format; leave as it is.
   *   version = <version>
   * (every port's table ended with that row). 0: no version line. */
  int version;
  /* The file's first lines. NULL: "# <PORT_TITLE> for Switch -- settings."
   * and "Changes apply the next time the game starts. ..." */
  const char *header;
  /* rt_config_save() and format upgrades write this one instead (NULL:
   * header): a port whose own screen also edits the settings says so. */
  const char *save_header;
  /* After every load and save, once the dst fields and rt_config() are set:
   * the port's derived fields and its "[config] ..." summary line. */
  void (*apply)(void);
} CfgTable;

#define CFG_COUNT(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

/* Reads config.ini through `t` (kept: pass a static table). The port's
 * dcr_config_load() calls it early in main(); until then the dst fields keep
 * their initial values and rt_config() its defaults. */
void rt_config_load(const CfgTable *t);

/* The current value of an option as written ("" for one the table does not
 * have), its choices (NULL: none), and the value as its row's kind reads it:
 * BOOL 1/0, CHOICE the index, INT the number, FLOAT truncated, TEXT atoi. */
const char *rt_config_get(const char *section, const char *key);
const char *rt_config_choices(const char *section, const char *key);
int rt_config_bool(const char *section, const char *key);
int rt_config_int(const char *section, const char *key);
float rt_config_float(const char *section, const char *key);
/* "section.key" (the Crossy Road port's C# asks this way): true/yes/on 1,
 * false/no/off 0, a number as itself; `dflt` for anything else or an option
 * this build does not have. */
int rt_config_value(const char *section_dot_key, int dflt);
int dcr_config_value(const char *section_dot_key, int dflt); /* the same */

/* A settings screen's edits: set checks the value against the row (BOOL
 * takes true/false; CHOICE and INT one of their choices), 0 when taken;
 * save writes the whole file through config.ini.part, then applies it (dst
 * fields, rt_config(), apply()); 0 on success. */
int rt_config_set(const char *section, const char *key, const char *value);
int rt_config_save(void);
/* The names Asphalt 8's start screen calls them by. */
const char *dcr_config_get(const char *section, const char *key);
const char *dcr_config_choices(const char *section, const char *key);
int dcr_config_set(const char *section, const char *key, const char *value);
int dcr_config_save(void);

/* Weak callback, before the first config.ini is written (no file yet): a
 * port carries earlier builds' flag files into it with rt_config_set(). */
void port_config_new_file(void);

/* ------------------------------------------------ the runtime's own rows
 * Placed by the port wherever its file has them (the file's order is the
 * table's). The engine finds them by section and key, so a port may also
 * spell one out itself (Asphalt 8 lists resolution's choices as
 * "auto,720,1080" for its start screen). `dst`: the port's own copy of the
 * value, or NULL. The help text is the port's; the common wording is below. */
#define CFG_ROW_RESOLUTION(def, help) \
  {"display", "resolution", def, help, CFG_CHOICE, "720,1080,auto", NULL, 0, 0, 0, 0}
#define CFG_ROW_BOOST(help, dst) \
  {"performance", "boost_cpu_when_loading", "true", help, CFG_BOOL, NULL, dst, 0, 0, 0, 0}
#define CFG_ROW_GL_SELFTEST(dst) \
  {"debug", "gl_selftest", "false", "Graphics self-test picture at start-up.", CFG_BOOL, NULL, dst, 0, 0, 0, 0}
#define CFG_ROW_BOOT_LOG(help, dst) \
  {"debug", "boot_log_on_screen", "false", help, CFG_BOOL, NULL, dst, 0, 0, 0, 0}
#define CFG_ROW_LOG_JNI(help, dst) \
  {"debug", "log_java_calls", "false", help, CFG_BOOL, NULL, dst, 0, 0, 0, 0}
/* Not read by the runtime (each port's input code does), but every port with
 * a controller has it: [controls] swap_a_b, default false. */
#define CFG_ROW_SWAP_AB(help, dst) \
  {"controls", "swap_a_b", "false", help, CFG_BOOL, NULL, dst, 0, 0, 0, 0}

#define CFG_HELP_RESOLUTION \
  "Rendering resolution: 720, 1080 or auto (1080 if docked when the game\n" \
  "# starts)."
#define CFG_HELP_BOOST \
  "CPU at 1785 MHz while the game starts (until its first picture) and\n" \
  "# inside loading frames (those over 50 ms), normal otherwise."
#define CFG_HELP_BOOT_LOG \
  "Show the start-up log on screen at every launch. Off: the log appears only\n" \
  "# while something is being set up (first launch, a new APK or NRO)."
#define CFG_HELP_LOG_JNI \
  "Write every Java method the game calls to debug.log (slow; for bug reports)."

#endif /* RT_CFG_H */
