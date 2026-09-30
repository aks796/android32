# Group F: files, boot, launcher

## Files

| file | what changed |
| --- | --- |
| `source/dcr_path.c/h` | lab2's rules plus the generic changes. Settings for the rest. Owns `dcr_game_root` / `dcr_set_game_root` / `dcr_data_dir` / `dcr_apk_path` / `dcr_set_apk_path`: every `main.c` defined them before. `dcr_path_traced` is weak. New weak `port_path_fixup`. Weak default `dcr_cwd`; bionic_io.c's strong one wins. `config.h` is replaced by `rt_settings.h`, and `DCR_PKG_NAME` is now `PORT_PACKAGE`. |
| `source/dcr_apkcache.c` + new `.h` | 512-byte path (abs). dcr's open-failure log line. "the APK" wording. `MAX_BLOCKS` and `KEEP_FREE` become `RT_APKCACHE_*`, with a static assert against the hash size. |
| `source/dcr_dircache.c` + new `.h` | `RT_DIRCACHE`. The scope is built from `dcr_data_dir()`. Generic comment. |
| `source/dcr_manifest.c/h` | flappy's `memset` on every load. `PORT_PACKAGE` default. `RT_MANIFEST_LOG_PREFIX` (dcr's `unity*` dump). `DCR_DEFAULT_PACKAGE` is removed; nothing used it. |
| `source/dcr_formats.h` | `nro_build_named(f, stem)`. `nro_build(f)` = `PORT_PAYLOAD_NAME ".build"`, defined only when `PORT_PAYLOAD_NAME` is. Generic comment. |
| `source/dcr_exefs.h` | Comment only. The code is byte-identical. |
| `source/dcr_migrate.h` | **Deleted.** `rt_migrate.h` replaces it. abs's `dcr_migrate_folder()` signature is kept there as a compatibility wrapper. |
| `source/rt_migrate.c/h` (new) | libnx-free folder move. Base: abs's merge. Adds pvz's same-size APK and `.moved` marker, a8r's both-installed guard and `migrated.txt`. Settings below. |
| `source/rt_apkfind.c/h` (new) | libnx-free zip central-directory walk (`rt_zip_walk*`, `rt_zip_has`, `rt_zip_data_offset`). Role-based APK finder `rt_apk_find` with adopt semantics (`.previous` / `.unused`). |
| `source/main.c` | The runtime's `main()`: steps 1-17 of the plan. It calls `port_load` and `port_run`. |
| `source/rt_boot.c/h` (**new, not in the DESIGN table**) | `dcr_report_boot`, `rt_boot_migrate(_report/_result)`, `rt_boot_find_apks`, `dcr_apk_role_path`, `dcr_apk_summary`, and the weak `port_*` boot defaults. **Why it exists:** a port that keeps its own `main.c` replaces the runtime's file, so these helpers cannot live in `main.c`. Please add it to group F in DESIGN.md. |
| `launcher/source/main.c` | The shared launcher. Everything that names the game comes from `port_config.h`, through `rt_settings.h`. Weak `port_launcher_*` hooks. |
| `launcher/source/launcher_bar.c`, `launcher.h` (new) | a8r's green progress screen, generic (`PORT_TITLE`, `PORT_SETUP_NOTE`). Hook declarations. |
| `launcher/source/fwd_mine.h` | Unchanged. |
| `launcher/launcher.mk` (new) | A port's `launcher/Makefile` sets 5 variables and includes it. The template is in its header. |
| `launcher/build.sh` | Shared docker build. It sources the port's `launcher/romfs_extras.sh`. |
| `launcher/Makefile` | lab2's; **deleted**. `launcher.mk` replaces it. |

## Compile status

- `tools/check.sh` builds main.c, rt_boot.c, rt_migrate.c, rt_apkfind.c, dcr_path.c, dcr_apkcache.c, dcr_dircache.c and dcr_manifest.c with no warnings. main.c also builds with `GL=0`.
- The same 8 files also build with no warnings against a scratch `port_config.h` that sets **every** optional setting: a8r's list values, `CWD_RELATIVE 0`, `DIRCACHE 0`, `MISMATCH_FATAL 1`, `PORT_APK_ROLES` with compound literals, and so on.
- The other groups' files that include my headers (bionic_io, bionic_stdio, dcr_setup, rt_cfg) also build with no warnings.
- **Launcher:** built with `devkitpro/devkita64` in two scratch ports under the scratchpad (`lport` and `lport2`). No warnings with `-Wall -Wextra`.
  - `lport` is lab2-like: default hooks, text mode.
  - `lport2` is a8r-like: `RT_LAUNCHER_BAR 1`, a hook file with minizip, `LAUNCHER_LIBS`, `romfs_extras.sh`, `PORT_APK_ROLES` with compound literals and a byline.
  - Both produced an NRO with the payload in the romfs.
- **Host tests** (scratchpad `ftest/`):
  - `rt_apk_find` was run with synthetic zips for each port's role table, in wrapper mode and launcher mode.
  - `rt_migrate` was tested for move, merge, NRO stays, skip-self, same-size APK, both-installed, and a second run as a no-op.
- **Path check** (scratchpad `pathtest/`): each port's old `dcr_path.c` against the new one with its settings, over 31 paths. The only differences are the intended ones:
  - dcr, pvz and abs gain the sdcard rule;
  - `/storage/emulated/legacy`, `/mnt/extSdCard` and `/storage/sdcard0` are now aliases everywhere;
  - `/mnt/sdcard/Android/obb/<pkg>` goes to `<root>/obb`.

  dcr's relative and jar paths are unchanged with `RT_PATH_CWD_RELATIVE 0`.

## 1. Settings and callbacks

### Settings (all `#ifndef` defaults sit in the file that reads them)

| name | default | ports that differ | what it does |
| --- | --- | --- | --- |
| `PORT_OLD_ROOT_PATHS` | none | dcr `"/switch/disneycrossyroadsea"`, lab2 `"/switch/labyrinth2"`, abs `"/switch/abspace"`, pvz `"/switch/pvztouch"`, sonic `"/switch/sonicracing"`, flappy `"/switch/flappybirdsfamily"`, a8r `"/switch/a8retry"` | Old folders, as a list, moved in by the wrapper and the launcher. |
| `PORT_APK_ROLES` | `RT_APK_ROLE_DEFAULT` (package = `PORT_PACKAGE`, name `PORT_APK_DEFAULT_NAME`) | see the table below | The APK role table. The launcher reads the same one. |
| `PORT_APK_DESC` | `PORT_TITLE " (" PORT_PACKAGE ")"` | see below | Describes the APK in error texts and in the launcher. |
| `PORT_APK_DEFAULT_NAME` | `"game.apk"` (rt_settings.h) | a8r `"A8R.apk"` | The fallback for base.apk before the search, and the default role's name. |
| `RT_PATH_CWD_RELATIVE` | 1 | dcr 0 | Relative paths resolve against `dcr_cwd()`. |
| `RT_PATH_SDCARD_DIR` | `"sdcard"` | none (`""` = off) | Other shared storage goes to `<root>/sdcard`. |
| `RT_PATH_SD_SUBDIRS` | none | a8r `"gameloft"` | `<shared storage>/<name>` goes to `<root>/<name>`. |
| `RT_PATH_EXTRA_DIRS` | none | a8r `"data/databases", "sdcard", "gameloft", "gameloft/games", "gameloft/games/GloftA8HP"` | Extra `prepare_dirs` folders. |
| `RT_PATH_TRACE_EXTRA` | `"main.pak"` | a8r `".obb"` | What `dcr_path_traced` logs besides `.apk`. |
| `RT_DIRCACHE` | 1 | dcr 0 | Whether the dircache answers misses. |
| `RT_APKCACHE_MAX_BLOCKS` / `RT_APKCACHE_KEEP_FREE` | 1024 / 256 MB | none | Cache size limits. |
| `RT_MANIFEST_LOG_PREFIX` | none | dcr `"unity"` | Meta-data entries logged after a load. |
| `RT_MIGRATE_MERGE` | 1 | none (dcr, lab2, sonic, flappy and a8r were 0 in effect) | A folder both have is merged, never overwriting. |
| `RT_MIGRATE_MOVE_NEWER_NRO` | 0 | abs 1 | An NRO carrying this build or a newer one moves too. |
| `RT_MIGRATE_SAME_SIZE_APK` | 1 | none | An APK with a same-size copy in the new folder stays. |
| `RT_MIGRATE_BOTH_INSTALLED` | none | a8r `"A8R.apk", "gameloft"` | Both folders hold an install: touch nothing. |
| `RT_MIGRATE_ONCE_MARKER` | none | pvz `".moved"` | The marker file says the move is done. |
| `RT_MIGRATE_RECORD` | `"migrated.txt"` | none | Appended with what moved. |
| `RT_EMU_FIXUPS` | 1 | dcr: 0 recommended, see its migration | `dcr_emu_fix_self()` right after `log_init`, under an emulator only. |
| `RT_BOOST_AT_BOOT` | 1 | a8r 0 | `dcr_boost_launch_begin()` at boot. |
| `RT_APK_CACHE` | 1 | none (flappy gains it) | `dcr_apkcache_set_path(apk)`. |
| `RT_PACKAGE_MISMATCH_FATAL` | 0 | flappy 1 | Another package: fatal instead of a warning. |
| `RT_LAUNCHER_BAR` | 0 | a8r 1 | The launcher shows the progress screen from the icon. |
| `PORT_LAUNCHER_START_NOTE` | `"(the first start unpacks the game from the APK)"` | see below | The line under "Starting ...". |
| `PORT_LAUNCHER_BYLINE` | none | flappy `"by aks796 (the Switch port); the game by .GEARS"` | A line under the launcher title. |

The launcher also reads `PORT_TITLE`, `PORT_NAME`, `PORT_ROOT_PATH`, `PORT_NSP_NAME`, `PORT_PAYLOAD_NAME` and `PORT_SETUP_NOTE`. `launcher.mk` passes `-DPORT_PAYLOAD_NAME` from `PORT_PAYLOAD`, and `LAUNCHER_APP_TITLE` from the Makefile's `APP_TITLE` (the name sphaira lists).

`PORT_ROOT_PATH` = `"/switch/" PORT_NAME` is right for all 7, so no port needs to set it.

`PORT_PAYLOAD` / `TARGET`: dcrsea_nx, labyrinth2_nx, abspace_nx, pvz_nx, sonicracing_nx, fbf_nx, a8retry_nx.

`PORT_BANNER` (today's report_boot line):

| port | `PORT_BANNER` |
| --- | --- |
| dcr | `"dcrsea_nx: Disney Crossy Road: SEA (Unity 5.6.4f1 / Mono, armeabi-v7a)"` |
| lab2 | `"labyrinth2_nx: Labyrinth 2 (Illusion Labs engine, armeabi)"` |
| abs | `"abspace_nx: Angry Birds Space (Rovio Fusion engine, armeabi-v7a)"` |
| pvz | `"pvz_nx: Plants vs. Zombies Touch (PvZ TV Touch: Transmension engine + Homura mod, armeabi-v7a)"` |
| sonic | `"sonicracing_nx: Sonic & SEGA All-Stars Racing (Sumo Digital engine, armeabi)"` |
| flappy | `"fbf_nx: Flappy Birds Family (dotGears engine, armeabi-v7a)"` |
| a8r | `"a8retry_nx: Asphalt 8: Airborne Retry (Gameloft jet/glf engine + the A8R mod, armeabi-v7a)"` |

`PORT_APK_DESC` (keep it to one line; the launcher indents it):

| port | `PORT_APK_DESC` |
| --- | --- |
| dcr | `"Disney Crossy Road, Southeast Asia edition 1.5.4 (armeabi-v7a)"` |
| lab2 | `"Labyrinth 2 1.29 (se.illusionlabs.labyrinth2)"` |
| abs | `"Angry Birds Space HD 2.2.14 (com.rovio.angrybirdsspaceHD, armeabi-v7a)"` |
| pvz | `"PvZ TV Touch 1.1.5 (com.trans.pvztv, armeabi-v7a)"` |
| sonic | `"Sonic & SEGA All-Stars Racing 1.0.1 (com.sega.ssasr, armeabi)"` |
| flappy | `"Flappy Birds Family (com.dotgears.flapfire, armeabi-v7a)"` |
| a8r | overrides `port_apk_help` and `port_launcher_instructions` instead |

`PORT_LAUNCHER_START_NOTE`, from today's texts:

| port | `PORT_LAUNCHER_START_NOTE` |
| --- | --- |
| dcr, pvz | `"(the first start unpacks the game's libraries from the APK)"` |
| lab2 | `"(the first start unpacks the game's library and levels from the APK)"` |
| abs, sonic | `"(the first start unpacks the game's library from the APK)"` |
| flappy | `"(the first start unpacks the game's engine from the APK)"` |
| a8r | not shown: bar mode |

### APK role tables (`PORT_APK_ROLES` in port_config.h)

Flags are `RT_APK_ADOPT`, `_OPTIONAL`, `_PACKAGE_PREFIX`, `_PACKAGE_BONUS`, `_HIGHEST_VERSION` and `_ANY_FILE`. Need lists are compound literals: `.need = (const char *const[]){"...", NULL}`.

| port | roles |
| --- | --- |
| dcr | `{.what = "Disney Crossy Road: SEA", .name = "game.apk", .package = "net.gogame.disney.crossyroad", .flags = RT_APK_ADOPT}, {.what = "the world-wide Disney Crossy Road (DuckTales)", .name = "dcr.apk", .package = "com.disney.disneycrossyroad", .flags = RT_APK_ADOPT \| RT_APK_OPTIONAL \| RT_APK_PACKAGE_PREFIX}` |
| lab2 | the default. Adding `.need = {"lib/armeabi/liblabyrinthii.so"}` is recommended: the launcher has no package check. |
| abs | `{.what = "the game", .need = {"lib/armeabi-v7a/libAngryBirdsSpace.so"}, .package = "com.rovio.angrybirdsspaceHD", .version_code = 221400, .flags = RT_APK_PACKAGE_BONUS}`; name NULL, as today. |
| pvz | `{.what = "the game", .name = "game.apk", .need = {"lib/armeabi-v7a/libGameMain.so"}, .reject = {"assets/paks/2.ChangeGameChina.zip"}}, {.what = "the English source", .name = "english.apk", .need = {"assets/paks/2.ChangeGameChina.zip"}, .flags = RT_APK_OPTIONAL}` |
| sonic | `{.what = "the game", .name = "game.apk", .need = {"lib/armeabi/libssasr.so"}, .flags = RT_APK_ANY_FILE}` |
| flappy | `{.what = "the game", .need = {"lib/armeabi-v7a/libflapfire.so", "res/raw/atlas.png"}, .flags = RT_APK_HIGHEST_VERSION}` |
| a8r | `{.what = "the A8R APK", .name = "A8R.apk", .need = {"AndroidManifest.xml", "assets/m_bgm_menu_halloween_djgontran_i_see_you.mp3", "assets/m_breton_the_commission.mp3", "assets/m_celldweller_pulsar.mp3", "assets/m_celldweller_through_the_gates.mp3", "assets/m_dj_gontran_down_to_earth.mp3"}, .flags = RT_APK_ADOPT}` |

A port may instead define `const RtApkRole *port_apk_roles(int *count)`. If it does, it must compile that file into the launcher too; `#include "../../source/x.c"` from a launcher source works.

### Callbacks

**Wrapper** (weak defaults are in `rt_boot.c` and `dcr_path.c`):

| callback | default | ports that define it |
| --- | --- | --- |
| `int port_load(const char *apk)` | **required** (with the runtime `main.c`); non-zero = quiet exit | all |
| `void port_run(void)` | **required** | all |
| `void port_report_boot(void)` | nothing | dcr: the `__dcr_reloc_diag` dump |
| `void port_before_update(void)` | nothing | a8r: `dcr_setup_zips_to_launcher()` |
| `int port_after_apk_find(void)` | 0 | pvz: English from the NRO; 1 = search again |
| `const char *port_apk_help(void)` | generic from `PORT_APK_DESC` | all may keep their exact texts; a8r should (zips) |
| `const RtApkRole *port_apk_roles(int *count)` | `PORT_APK_ROLES` | none |
| `const char *port_path_fixup(const char *real, char *out, size_t cap)` | `real` | lab2: the iPad overlay (move `overlay()` from its old `dcr_path.c`; `real` may be `out`) |
| `int dcr_path_traced(const char *p)` (weak OVERRIDE) | `.apk` or `RT_PATH_TRACE_EXTRA` | dcr: its Unity rules |
| `void port_audio_selftest(void)` | group D's weak default | lab2 `lab_audio_selftest`, abs `abs_audio_selftest`, sonic `ssr_audio_selftest` |

**Launcher** (weak defaults are in `launcher/source/main.c`):

| hook | default | ports that define it |
| --- | --- | --- |
| `int port_launcher_prepare(void)` | 0 | a8r: `zips_install()` |
| `int port_launcher_check(char *status, size_t, char *help, size_t)` | 0 | sonic: `ssr_find_data` gives "game data: X (how)" or MISSING, plus the OBB help. a8r: the OBB at `gameloft/games/GloftA8HP/main.sa2.Asphalt8.obb`, plus the zips help. |
| `void port_launcher_apk_note(const char *apk)` | nothing | flappy: the engine build from the lib's CRC, via `rt_zip_walk_path`; print only if `!launcher_bar_on()` |
| `void port_launcher_instructions(void)` | "1. put the APK of your own `PORT_APK_DESC` ..." | sonic: the APK + OBB lines; a8r: the three zips |
| `port_apk_roles` | `PORT_APK_ROLES` | none |

Shared helpers the hooks use: `launcher_bar/_off/_on`, `launcher_header`, `rt_zip_*`.

**Exports for ports:** `dcr_game_root`, `dcr_set_game_root`, `dcr_data_dir`, `dcr_apk_path`, `dcr_set_apk_path`, `dcr_apk_role_path(i)`, `dcr_apk_summary()`, `dcr_report_boot`, `rt_boot_*`, `rt_migrate_*`, `dcr_migrate_folder` (compat), `rt_apk_find`, `rt_zip_*`, and every existing `dcr_manifest_*`, `dcr_apkcache_*` and `dcr_dircache_*` function.

### Launcher per port

Each port adds:
- `launcher/Makefile`: `APP_*`, `TARGET` = `PORT_NAME`, `PORT_PAYLOAD`, the include;
- `launcher/build.sh`: 3 lines, see the `launcher/build.sh` header;
- `icon.jpg`.

| port | extras |
| --- | --- |
| dcr | `romfs_extras.sh`: the DuckTales block of today's build.sh, unchanged (uses `$HERE`) |
| lab2 | `romfs_extras.sh`: the iPad block (`python3 "$HERE/../tools/make_ipad_assets.py"`) |
| abs | none; delete `launcher/source/migrate.c` |
| pvz | `romfs_extras.sh`: the English-pack block |
| sonic | hooks file: check and instructions; `ssr_pack.h` is found in `../source` |
| flappy | hooks file: the apk note; `PORT_LAUNCHER_BYLINE`; `APP_VERSION 0.1.0` |
| a8r | Keep `zips.c`/`zips.h`. Delete their `launcher_bar*` (now shared) and include `launcher.h`. Set `GAME_DIR` to `"sdmc:" PORT_ROOT_PATH`. Hooks: prepare, check, instructions. Add `LAUNCHER_LIBS := -lminizip -lz` and `RT_LAUNCHER_BAR 1`. |

## 2. Migration per port

**All ports:**
- Delete `g_root`, `dcr_game_root()`, `g_apk` and `dcr_apk_path()` from `main.c`, or delete `main.c` itself: the runtime has them.
- Set `PORT_*` in `port_config.h`.
- Move the tail of today's `main()` into `port_load` / `port_run`, as in the table below.
- Delete the port copies of the group F files: `dcr_path.c/h`, `dcr_apkcache.c`, `dcr_dircache.c`, `dcr_manifest.c/h`, `dcr_formats.h`, `dcr_exefs.h`, `dcr_migrate.h/.c`, `dcr_apkfind.*`, `launcher/source/main.c`, `launcher/source/fwd_mine.h`, `launcher/Makefile`.
- Replace hand-declared cache prototypes with `dcr_apkcache.h` / `dcr_dircache.h` (optional).

| port | port_load(apk) | port_run() | also |
| --- | --- | --- | --- |
| dcr | `dcr_setup_from_apk`; 3× `load_module` (EMU_POOL for unity), else fatal; 3× `resolve_module`; emulator: `dcr_emu_fix_module(&unity_mod, pool)` and its own `dcr_emu_fix_self()`; 3× `so_finalize` + `so_flush_caches`; `dcr_patches_apply`; the FMOD patches per config; `dcr_time_install`; `dcr_vsync_init`; `dcr_mono_install`; `jit_arena_init` (fatal); `dcr_mono_hook_exceptions`; `dcr_mod_jitlog_install` | 3× `so_execute_init_array`; `dcr_boot_run()` (never returns) | `RT_PATH_CWD_RELATIVE 0`, `RT_DIRCACHE 0`, `RT_MANIFEST_LOG_PREFIX "unity"`, `RT_EMU_FIXUPS 0` (keeps its self-fix after the module fix, as today); `dcr_path_traced` in a port file; `port_report_boot`; drop the `dcr_setup_adopt_apks()` call (the roles adopt now); APK is `dcr_apk_path()` (still `<root>/game.apk` after adoption) |
| lab2 | `dcr_setup_from_apk`; `lab_apk_init` (fatal); the .ipa lookup (game.ipa, else any *.ipa, else from the NRO: G's `rt_setup_copy_from_nro`); `lab_ipa_init`; `lab_reg_load`; `lab_levels_load`; `lab_files_setup`; `rt_setup_finish()`; `lab_load_module` (fatal) | `lab_run_constructors`; `lab_boot_run` | `port_path_fixup` = `overlay()`; `port_audio_selftest` = `lab_audio_selftest`; its `find_apk` / `move_old_folder` go |
| abs | `dcr_setup_from_apk`; `abs_assets_init` (fatal); `abs_load_module` (fatal) | `abs_run_constructors`; `abs_boot_run` | `RT_MIGRATE_MOVE_NEWER_NRO 1`; drop `dcr_setup_move_old_folder`; delete `dcr_apkfind.h`, `dcr_migrate.c/h`; the migration now runs before `log_init` (it ran after report_boot) |
| pvz | `dcr_setup_from_apk`; `pvz_load_modules` (fatal) | `pvz_run_constructors`; `pvz_boot_run` | `port_after_apk_find`: `dcr_config()->english && !dcr_apk_role_path(1)[0] && <English from NRO> == 1`. Delete `pvz_apks.c`. Aliases: `pvz_game_apk()` = `dcr_apk_path()`; `pvz_english_apk()` = role 1 or `"<root>/english.apk"`; `pvz_apks_have_english()` = `dcr_apk_role_path(1)[0]`; `pvz_apks_summary()` = `dcr_apk_summary()`. `RT_MIGRATE_ONCE_MARKER ".moved"`. |
| sonic | `dcr_setup_from_apk`; `ssr_apk_init` (fatal); `ssr_data_find` (fatal, OBB text); `ssr_load_module` (fatal) | `ssr_run_constructors`; `ssr_boot_run` | `port_audio_selftest` = `ssr_audio_selftest`; delete its `dcr_migrate.h`; `ssr_find_apk` is replaced by the role table (`ssr_find_data` stays) |
| flappy | `dcr_setup_from_apk`; `fbf_load_engine` (fatal) | `fbf_run_constructors`; `fbf_game_run` | `RT_PACKAGE_MISMATCH_FATAL 1`; delete `dcr_apkfind.c/h`; the migration now runs before `log_init`; the APK cache is on now |
| a8r | `if (!dcr_config()->skip_start \|\| a8r_input_minus_held()) if (a8r_menu_run(apk) == 0) return 1;`; `dcr_boost_launch_begin`; `a8r_setup`; `a8r_menu_done`; `a8r_load_engine` (fatal) | `dcr_prof_start`; `a8r_run_constructors`; `a8r_boot_run` | `RT_BOOST_AT_BOOT 0`, `PORT_APK_DEFAULT_NAME "A8R.apk"`, `RT_PATH_*` as above, `RT_MIGRATE_BOTH_INSTALLED`; `port_before_update`; `port_apk_help` (the zips text); delete `a8r_folder.h` (migrate + adopt), unless `a8r_is_mod_apk` has other users |

## 3. Open questions and risks

**Requests for the integrator:**
1. **`rt_boot.c/h` is a new group F file.** Add it to DESIGN.md.
2. **Add `rt_boot` to `RT_BUILD_H_USERS` in runtime.mk** (group E). It includes `dcr_build.h`, which exists early (order-only), but only listed users rebuild when the build number changes. A stale `DCR_BUILD` only affects abs's `RT_MIGRATE_MOVE_NEWER_NRO` comparison, and not harmfully.
3. **Move the `PORT_APK_DESC` and `PORT_APK_ROLES` defaults to rt_settings.h.** They are identical `#ifndef`s in `rt_boot.c` and `launcher/source/main.c`, because two files read them.
4. **test/port/port_config.h** (dcr) could set `RT_PATH_CWD_RELATIVE 0`, `RT_DIRCACHE 0` and dcr's `PORT_APK_ROLES`, so the check covers them.
5. **The launcher is not covered by check.sh.** A `tools/check_launcher.sh` could build `launcher/` against `test/port` in devkita64. The scratch ports under `scratchpad/lport*` show how.

**Behaviour changes:**
6. **dcr: the generic sdcard rule.** `/storage/emulated/0/...`, `/sdcard/...` and friends that Unity or Mono write outside the package folders now go to `<root>/sdcard` instead of the SD root. Before switching, check a player's SD card for `sdmc:/storage` or `sdmc:/sdcard` leftovers. The same applies to pvz and abs, which had no rule; flappy and sonic are already lab2-like.
7. **Adoption keeps `<name>.previous` instead of `remove()`.** dcr's old `game.apk` / `dcr.apk` stay as `*.previous`, which can be hundreds of MB on the SD card. dcr's world-wide APK is now matched by the parsed manifest package prefix `com.disney.disneycrossyroad`, not a byte scan.
8. **APK ranking.**
   - sonic: .apk before other files, then the newest (was readdir order).
   - flappy: the newest before by-name, at equal versionCode.
   - a8r: `A8R.apk` itself must now match the need list, or "no APK" (it was used unchecked).
9. **Migration merge is now on for every port.** A subfolder both folders have (such as `data/`) is merged, never overwriting; before, dcr, lab2, sonic, flappy and a8r left it whole. Set `RT_MIGRATE_MERGE 0` in a port to keep the old behaviour.
10. **Every port now writes `migrated.txt` when something moves.**
11. **a8r's both-installed guard:** when only the new folder has an install, a8r's leftovers (config.ini and the like) now move; a8r used to leave the old folder alone.
12. **The launcher has no manifest reader.** Package-only roles (dcr, lab2's default) accept any readable zip there and are never adopted in the launcher. This is the same as today's "any *.apk", but no better.
13. **`RT_EMU_FIXUPS` default 1:** abs, pvz and flappy get the early self-fix under an emulator. It is a no-op on hardware.
14. **Launcher:** + is accepted from any controller (flappy's `padInitializeAny`), for all ports. The instruction texts are generic unless a port overrides the hooks.
15. **a8r: `dcr_self_process()` now runs before the start screen** (it ran after). It is only a handle fetch.

**Other risks:**
16. **Payload stems stay as they are** (the `nro_build` / romfs contract). `nro_build()` now needs `PORT_PAYLOAD_NAME`; host tools should use `nro_build_named()`.
17. **The dircache scope is fixed on the first call.** `dcr_set_game_root()` must come before any file operation.
18. **No declarations of other groups' names remain.** `main.c` includes `emu_fixups.h`, `dcr_boost.h`, `rt_applet.h`, `dcr_setup.h`, `rt_cfg.h` and `rt_audout.h`. It still declares `dcr_config_load`, `dcr_pthread_selftest`, `dcr_io_selftest` and `dcr_gl_selftest` locally, as the ports' `main.c` files did.
