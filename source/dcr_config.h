/* dcr_config.h -- the user's settings, from <game folder>/config.ini (dcr_config.c). */
#ifndef DCR_USER_CONFIG_H
#define DCR_USER_CONFIG_H

/* [controls] tilt: what tilts the board. */
enum { LAB_TILT_STICK,  /* the left stick (and the D-pad) */
       LAB_TILT_MOTION, /* the controller's motion sensors, held flat */
       LAB_TILT_BOTH    /* motion, with the stick adding to it */ };

/* [display] layout: where the portrait picture goes on the landscape screen. */
enum { LAB_LAYOUT_PORTRAIT, /* upright in the middle, side panels beside it */
       LAB_LAYOUT_ROTATED_LEFT, /* turned to fill the screen: hold the console upright, */
       LAB_LAYOUT_ROTATED_RIGHT /* its left (or right) side at the top */ };

typedef struct {
  int swap_ab;           /* [controls] swap_a_b */
  int tilt;              /* [controls] tilt: LAB_TILT_* (the Settings screen changes it) */
  float stick_tilt;      /* [controls] stick_tilt: g of tilt at full stick */
  float motion_gain;     /* [controls] motion_sensitivity: x the controller's own tilt */
  float pointer_speed;   /* [controls] pointer_speed: dp per frame at full stick */
  int rumble;            /* [controls] rumble: holes, goals, bumpers */
  int layout;            /* [display] layout: LAB_LAYOUT_* */
  int level_layout;      /* [display] level_layout: LAB_LAYOUT_*, for a level being played
                          * (portrait layout only; the game's ZR changes it) */
  int supersample;       /* [display] supersample: 1 or 2 (the portrait surface's scale) */
  int side_panels;       /* [display] side_panels: the background and hints beside the picture */
  int menus;             /* [display] menus: 0 the iPad's (when its .ipa is there), 1 the phone's */
  int res_w, res_h;      /* [display] resolution */
  int online;            /* [online] enabled: the level server (downloads, your packs) */
  char device_id[40];    /* [online] device_id: "" = made from the console (lab_online.c) */
  int boost;             /* [performance] boost_cpu_when_loading */
  int gl_selftest;       /* [debug] gl_selftest */
  int boot_log;          /* [debug] boot_log_on_screen */
  int log_jni;           /* [debug] log_java_calls */
  int log_touch;         /* [debug] log_touches */
} DcrConfig;

/* Read config.ini (writing it with the defaults, or adding missing options,
 * first). Early in main(); the defaults hold until then. */
void dcr_config_load(void);
const DcrConfig *dcr_config(void);
/* The Settings screen's tilt choice (saved in the game's registry, which
 * wins over config.ini once set). */
void dcr_config_set_tilt(int tilt);
/* In a level, ZR's choice of level_layout (kept in the registry likewise). */
void dcr_config_set_level_layout(int layout);

#endif
