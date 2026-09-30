/* dcr_config.c -- Disney Crossy Road's settings: config.ini's options, on
 * the runtime's INI engine (runtime/source/rt_cfg.c).
 *
 * REFERENCE for the pilot port (docs/examples/): it replaces the port's
 * dcr_config.c; the port's dcr_config.h (DcrConfig, dcr_config_load(),
 * dcr_config()) stays as it is. The options, their order, defaults and help
 * text are the ones the port writes today, so players' config.ini files read
 * the same. What changes: the engine keeps the values (64 characters, was
 * 16), a format upgrade rewrites the whole file through config.ini.part
 * instead of one line in place (values kept; the player's own comments are
 * not), and dcr_config_value() is the runtime's (rt_config_value()).
 *
 * The file header is the runtime's default, "# Disney Crossy Road for Switch
 * -- settings." (PORT_TITLE). The flag files of earlier builds (keep_locks,
 * hop_on_release, gltest) are carried into the first config.ini written. MIT.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>

#include "dcr_config.h"
#include "dcr_path.h"
#include "rt_cfg.h"
#include "util.h"

static DcrConfig g_cfg = {
    .unlock_all = 1,  .max_stars = 1,     .hide_top_bar = 1,   .hop_on_press = 1, .res_w = 1280,
    .res_h = 720,     .boost = 1,         .short_wipe = 1,     .free_store = 1,   .log_sounds = 1,
    .sound_priority = 1, .quiet_center = 1, .mix_48k = 1, .menu_controls = 1, .pretend_online = 1,
};

const DcrConfig *dcr_config(void) { return &g_cfg; }

/* dst NULL: read by the port's C# through dcr_config_value("section.key"). */
static const CfgOpt k_opts[] = {
    {"game", "unlock_all_characters", "true",
     "Every character and collection unlocked (the online events that give\n"
     "# some away never come here).",
     CFG_BOOL, NULL, &g_cfg.unlock_all},
    {"game", "max_character_stars", "true",
     "Every character at its maximum star level, with all of that level's boosts.",
     CFG_BOOL, NULL, &g_cfg.max_stars},
    {"game", "hide_top_bar", "true",
     "Hide the thin coloured bar across the top of the screen (the game's mode bar).",
     CFG_BOOL, NULL, &g_cfg.hide_top_bar},
    {"game", "free_purchases", "true",
     "The store is free: ticket packs, pixel packs and bundles are yours for\n"
     "# the asking (the game's own offline store; there is no Google Play here).",
     CFG_BOOL, NULL, &g_cfg.free_store},
    {"game", "ducktales", "true",
     "The DuckTales pack from Disney Crossy Road (the world-wide edition): its\n"
     "# characters, world, music and sounds, as its own section of the character\n"
     "# select. Read at start-up from your own copy of that game: put its APK in\n"
     "# this folder as dcr.apk (without it, this does nothing).",
     CFG_BOOL, NULL, NULL},
    {"game", "sorcerer_mickey", "true",
     "Sorcerer's Apprentice Mickey, an Epic figurine in Mickey & Friends (his\n"
     "# models are in the game's files but he was never released). Like the Genie,\n"
     "# he turns the obstacles ahead of him into Fantasia's marching brooms.",
     CFG_BOOL, NULL, NULL},
    {"game", "hidden_characters", "true",
     "The figurines whose models are in the game's files but not in its character\n"
     "# select: Golden Camel, Dragon Genie, Elf Pleakley, Santa Jumba, Vampire\n"
     "# Stitch, Witch Lilo and The Ocean (released only in the world-wide edition),\n"
     "# Classic Mickey, the Fantasia Broom, Safari Mickey, Max, Oswald, Ortensia,\n"
     "# the Scuba Diver and Human Cadenza (never released), and the hidden Meowana\n"
     "# and Hanging Tree. Each in its own theme, won in the prize machine.",
     CFG_BOOL, NULL, NULL},
    {"game", "profiles", "true",
     "Switch profiles on the leaderboard and in multiplayer. The first time the\n"
     "# leaderboard opens, the Switch's profile picker comes up (B: no profile,\n"
     "# not asked again); X in the leaderboard changes it, - drops it. Your runs\n"
     "# then go on that profile's name and icon. In multiplayer each player can\n"
     "# press X in the waiting room to play as a profile. Scores are kept in\n"
     "# leaderboard.txt next to this file.",
     CFG_BOOL, NULL, NULL},
    {"controls", "hop_on_press", "true",
     "Hop the moment A or a direction is pressed. false: the game's own\n"
     "# crouch on press, hop on release.",
     CFG_BOOL, NULL, &g_cfg.hop_on_press},
    {"controls", "menu_controls", "true",
     "Every menu works with the controller: A presses, B goes back, + pauses,\n"
     "# L/R change theme in the character select, and the screens open with a\n"
     "# button already chosen. Touch still works.",
     CFG_BOOL, NULL, &g_cfg.menu_controls},
    {"online", "pretend_online", "true",
     "The game thinks it is online and signed in, answered by the port itself: no\n"
     "# \"no connection\" popups, and the prize machine, daily missions, weekend\n"
     "# challenges (a different one each week), the ticket machine, free gifts,\n"
     "# leaderboards, the daily login reward and coin drops in runs all work;\n"
     "# \"watch an ad\" rewards are given at once.",
     CFG_BOOL, NULL, &g_cfg.pretend_online},
    {"multiplayer", "local_multiplayer", "true",
     "The multiplayer button starts LOCAL multiplayer: up to 4 players on this\n"
     "# Switch, one controller (or one Joy-Con held sideways) each. Four modes:\n"
     "# Coin Crazy, Snatch and Run, Last One Standing and Hop Non-Stop (the last\n"
     "# two are knock-outs: no timer, one life each). Player 1 picks the world\n"
     "# with L/R in the waiting room; + pauses a match (B then ends it).",
     CFG_BOOL, NULL, NULL},
    {"multiplayer", "match_seconds", "120",
     "How long a Coin Crazy or Snatch and Run match lasts, in seconds (30-900).",
     CFG_TEXT, NULL, NULL},
    CFG_ROW_RESOLUTION("auto",
                       "Rendering resolution: auto (1080 if docked when the game starts, 720 in\n"
                       "# handheld), 1080 or 720. The Switch scales the picture to the screen either\n"
                       "# way. 1080 in handheld drops to 30 fps: the handheld GPU cannot keep up."),
    CFG_ROW_BOOST("CPU at 1785 MHz while the game starts (until its title menu, through\n"
                  "# the logos) and inside loading frames (those over 50 ms), normal otherwise.",
                  &g_cfg.boost),
    {"performance", "shorter_transitions", "true",
     "Start loading as soon as the screen wipe has covered the screen (0.6 s)\n"
     "# instead of 0.8 s after it began: map changes 0.2 s faster.",
     CFG_BOOL, NULL, &g_cfg.short_wipe},
    {"performance", "bundles_in_place", "true",
     "Open the game's asset bundles (characters, worlds, music) where they lie\n"
     "# inside game.apk. Off: the game's own way, which copies each one into a\n"
     "# cache on the SD card first (slow on a first start).",
     CFG_BOOL, NULL, NULL},
    {"audio", "important_sounds_first", "true",
     "Every sound playing is heard: 64 voices instead of the game's 32 (a busy\n"
     "# road plays 40-50 sounds at once, and the extra ones -- hops, car engines,\n"
     "# crowds -- were silent), and hops and clicks come before ambient loops.",
     CFG_BOOL, NULL, &g_cfg.sound_priority},
    {"audio", "one_theme_switch_sound", "true",
     "Changing theme in the character selector plays its sound once (the\n"
     "# carousel's click no longer lands on top of it). The SEA edition already\n"
     "# plays one: nothing to change there.",
     CFG_BOOL, NULL, &g_cfg.quiet_center},
    {"audio", "mix_at_48khz", "true",
     "Mix the game's audio at 48 kHz, as phones do. Off: 24 kHz, the Android\n"
     "# fallback's rate (muffled music, harsh effects).",
     CFG_BOOL, NULL, &g_cfg.mix_48k},
    CFG_ROW_GL_SELFTEST(&g_cfg.gl_selftest),
    {"debug", "profile_long_frames", "false",
     "Write where the time goes in frames over 100 ms (loading) to debug.log.\n"
     "# It slows those frames down a little: leave off unless asked for a log.",
     CFG_BOOL, NULL, &g_cfg.profile},
    CFG_ROW_BOOT_LOG("Show the start-up log on screen at every launch. Off: the screen stays\n"
                     "# dark until the game draws, and the log appears only while something is\n"
                     "# being set up or updated (first launch, a new game.apk or NRO).",
                     &g_cfg.boot_log),
    {"debug", "log_sounds", "false",
     "Write every sound the game plays (and its volume faders) to debug.log\n"
     "# (for a bug report about audio).",
     CFG_BOOL, NULL, &g_cfg.log_sounds},
    {"debug", "port_mod", "true",
     "The port's own additions to the game (menu controls, local multiplayer,\n"
     "# the extra characters, the online spoof). Off only to rule them out.",
     CFG_BOOL, NULL, NULL},
    /* [config] version = 3: the engine's row, last (CfgTable.version) */
};

/* Defaults that changed ([config] version records the file's format; a file
 * without the line is format 1). */
static const CfgMigrate k_migrate[] = {
    /* version 1 (build 202609241315) defaulted to 1080; 1080 in handheld runs
     * at 30 fps, so that default becomes auto */
    {"display", "resolution", "1080", "auto", 2},
    /* version 2 (builds 202609241343-1403) had the profiler on by default */
    {"debug", "profile_long_frames", "true", "false", 3},
};

static void apply(void) {
  const RtConfig *rt = rt_config();
  g_cfg.res_w = rt->res_w;
  g_cfg.res_h = rt->res_h;
  int docked = appletGetOperationMode() == AppletOperationMode_Console;
  debugPrintf("[config] unlocks %s, max stars %s, top bar %s, free purchases %s, hop on %s, %dx%d (%s, %s), "
              "CPU boost %s, short transitions %s, important sounds first %s, one theme-switch sound %s, 48 kHz %s, "
              "profiler %s, sound log %s\n",
              g_cfg.unlock_all ? "on" : "off", g_cfg.max_stars ? "on" : "off",
              g_cfg.hide_top_bar ? "hidden" : "shown", g_cfg.free_store ? "on" : "off",
              g_cfg.hop_on_press ? "press" : "release", g_cfg.res_w, g_cfg.res_h,
              rt_config_get("display", "resolution"), docked ? "docked" : "handheld", g_cfg.boost ? "on" : "off",
              g_cfg.short_wipe ? "on" : "off", g_cfg.sound_priority ? "on" : "off",
              g_cfg.quiet_center ? "on" : "off", g_cfg.mix_48k ? "on" : "off", g_cfg.profile ? "on" : "off",
              g_cfg.log_sounds ? "on" : "off");
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .migrate = k_migrate,
    .nmigrate = CFG_COUNT(k_migrate),
    .version = 3,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }

/* First start: carry over the flag files of earlier builds. */
static int exists(const char *name) {
  char p[300];
  struct stat st;
  snprintf(p, sizeof p, "%s/%s", dcr_game_root(), name);
  return stat(p, &st) == 0;
}

void port_config_new_file(void) {
  if (exists("keep_locks")) {
    rt_config_set("game", "unlock_all_characters", "false");
    rt_config_set("game", "max_character_stars", "false");
  }
  if (exists("hop_on_release"))
    rt_config_set("controls", "hop_on_press", "false");
  if (exists("gltest"))
    rt_config_set("debug", "gl_selftest", "true");
}
