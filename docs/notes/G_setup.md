# Group G: setup and config

Files:
- `source/rt_cfg.c`, `source/rt_cfg.h`: the INI engine. They replace the
  seeded `dcr_config.c/h`, which are deleted.
- `source/dcr_setup.c`, `source/dcr_setup.h`: the first-launch setup, the NRO
  self-update and the hand-over to the launcher.
- `docs/examples/dcr_setup_plan.c`, `docs/examples/dcr_config_table.c`: the
  pilot port's plan and option table.

All four source files compile warning-free with `tools/check.sh`. The
examples compile warning-free with the port's flags in a scratch dir, next to
`test/port` and DCR's own `dcr_config.h`.

Verified on the host with DCR's original `dcr_config.c` / `dcr_setup.c`
against the new engine, driver and examples (a miniz-over-zlib shim, and
synthetic APKs and NROs):

- **Config.** `config.ini` is byte-identical for:
  - a fresh file, docked and handheld;
  - the flag-file carry-over;
  - append-missing, with the user's comments and edits kept;
  - invalid values;
  - a v2 file.

  In a v1 file the values match, but the file is rewritten whole (see risk 3).
- **Setup.** Every file written is byte-identical:
  - the stored `game.apk` rewrite;
  - the three libraries;
  - `classes.txt` and `.setup`;
  - the DuckTales pack, from dcr.apk and from the NRO;
  - a second run, a newer NRO, and a changed library.

## 1. Settings and callbacks

### Settings

| name | file | default | per port | what it does |
| --- | --- | --- | --- | --- |
| `RT_SETUP_UPDATE_PERMILLE` | dcr_setup.c | 1000 | sonic 500, a8r 500; others 1000 | Where the bar stands while an NRO update (or a hand-over to the launcher) restarts. |
| `RT_SETUP_APK_REQUIREMENT` | dcr_setup.c | "This port needs PORT_TITLE (PORT_PACKAGE) for 32-bit ARM (PORT_ABI_DIR): use the APK of your own copy." | none; ports set `plan.apk_requirement` instead | The fatal text when a library is missing from the APK. |
| `RT_CFG_VALUE_MAX` | rt_cfg.c | 64 | was dcr 16, abs/pvz/flappy 24, a8r 40, lab2/sonic 48 | The longest value kept. |

Shared settings read:
- `PORT_TITLE`:
  - the config header, `"# " PORT_TITLE " for Switch -- settings."`;
  - `[setup] setting up ...`.
- `PORT_PACKAGE` and `PORT_ABI_DIR`: where the libraries are, and the default
  requirement text.
- `PORT_NSP_NAME`: the update's romfs file.
- `PORT_PAYLOAD_NAME`, through `nro_build()`.
- `PORT_APK_DEFAULT_NAME`: the example only.
- `RT_SCREEN_W/H`: the RtConfig default.

### Callbacks (weak; the runtime's default keeps today's behaviour)

| name | default | who defines it |
| --- | --- | --- |
| `void port_setup_progress_draw(const char *what, int permille)` | `log_console_progress` + `log_console_update` | a8r: `log_console_active() ? log_console_progress(...) : a8r_menu_progress(...)`, then `log_console_update()` |
| `int port_setup_lib_override(const char *lib, unsigned long apk_crc, RtSetupCtx *ctx)` | 0 (unpack it from the APK) | pvz: its built-in libHomura when the APK's CRC is a known 1.1.5 build (`ensure_port_mod`) |
| `void port_config_new_file(void)` | nothing | dcr: flag files `keep_locks`, `hop_on_release`, `gltest`, applied with `rt_config_set` (in the example) |

### Port-provided data

- `const RtSetupPlan port_setup_plan` (weak reference). With no plan,
  `dcr_setup_from_apk` makes only classes.txt, at 750-950. The plan holds:
  - `libs`, `nlibs`, `libs_what`, `apk_requirement`;
  - `libs_p0/p1` (bar by bytes) and `classes_p0/p1` (bar by dex file);
  - `store_apk_prefix` and `store_p0/p1`;
  - `steps[]` of `{name, when, p0, p1, run}`, where `when` is
    `RT_STEP_BEFORE_LIBS`, `RT_STEP_WITH_ZIP` or `RT_STEP_AFTER_ZIP`;
  - `finish_in_port`.
- A `CfgTable`, passed by the port's `dcr_config_load()` to
  `rt_config_load(&table)`. It holds:
  - `opts`, `nopts`;
  - `sections` (intro lines under a `[section]` header);
  - `migrate` (`{section, key, from|NULL, to, below_version}`);
  - `version` (the engine writes the `[config] version` row, last);
  - `header` and `save_header` (NULL: the default);
  - `apply` (derived fields and the summary log line, after every load and
    save).

### Runtime API

`rt_cfg.h`:
- `RtConfig` and `rt_config()`, exactly as DESIGN.md fixes them. They are
  filled by section/key from `[display] resolution`,
  `[performance] boost_cpu_when_loading` and `[debug] gl_selftest`,
  `boot_log_on_screen` and `log_java_calls`, whatever the rows' `dst`. A
  missing row keeps the default: 1280x720, boost 1, the rest 0.
- `rt_config_load`, `rt_config_get`, `rt_config_choices`, `rt_config_bool`,
  `rt_config_int` (the value as its row's kind reads it), `rt_config_float`,
  `rt_config_value("sec.key", dflt)`, `rt_config_set` and `rt_config_save`.
- Weak wrappers under today's names: `dcr_config_value`, `dcr_config_get`,
  `dcr_config_choices`, `dcr_config_set` and `dcr_config_save`. They are
  wrappers, not aliases, so the files also build on macOS for host tests.
- Row macros:
  - `CFG_ROW_RESOLUTION(def, help)` (CHOICE `720,1080,auto`, no dst);
  - `CFG_ROW_BOOST(help, dst)`;
  - `CFG_ROW_GL_SELFTEST(dst)`;
  - `CFG_ROW_BOOT_LOG(help, dst)`;
  - `CFG_ROW_LOG_JNI(help, dst)`;
  - `CFG_ROW_SWAP_AB(help, dst)`.
- The common help texts: `CFG_HELP_RESOLUTION`, `CFG_HELP_BOOST`,
  `CFG_HELP_BOOT_LOG` and `CFG_HELP_LOG_JNI`. Each is the wording most ports
  use, byte-for-byte.
- Invalid resolution: falls back to the row's default, which reproduces every
  port (720 in lab2/abs/pvz, auto in the others).
- The resolution always ends in `dcr_window_set_size()` (rt_window.h).

`dcr_setup.h`:
- Kept exports: `dcr_setup_from_apk`, `dcr_setup_update_from_nro`,
  `dcr_setup_progress` and `dcr_setup_did_work`.
- New: `rt_setup_finish`, `rt_setup_progress_in`,
  `rt_setup_stamp_get/set/save`, `rt_root_path`, `rt_file_size`,
  `rt_read_whole` (a8r's: NUL-terminated), `rt_write_atomic`,
  `rt_write_if_changed`, `rt_mkdirs`, `rt_mkdirs_for` and `rt_crc_of_file`.
- Zip helpers:
  - `rt_setup_extract_entry(zip, idx, dst, what, p0, p1)`: through .part, the
    bar by bytes;
  - `rt_setup_store_apk`;
  - `rt_apk_has_package`.
- NRO helpers:
  - `rt_find_nro` (skips `._*`);
  - `rt_setup_copy_from_nro(romfs_name, dst_rel, stamp_key, what, p0, p1)`.
    - Stamp modes: NULL is `<dst>.from` (lab2); `""` is no stamp (pvz); a key
      goes in `.setup`, with crc = build (dcr).
    - Returns 2 copied, 1 current, 0 none, -1 failed.
- `rt_setup_restart_into_launcher(what)`.

## 2. Migration per port

For every port:
- Delete the port's `dcr_setup.c`. a8r deletes only its
  `dcr_setup_update_from_nro` from `a8r_setup.c`: the symbol would clash.
- The port's `dcr_config.c` becomes its option table plus
  `dcr_config_load(){ rt_config_load(&t); }` and `dcr_config()`. See
  `docs/examples/dcr_config_table.c`.
- Keep each port's rows in exactly today's order and with today's
  keys/defaults/help. Drop the `[config] version` row: set
  `CfgTable.version` instead.
- The summary `debugPrintf` and the derived fields move into `apply`.
- Runtime files already read `rt_config()`: D `dcr_boost.c`, F `main.c`,
  C `jni_core.c`.
- `.setup` keys must not change, or every player unpacks again once. They are
  the library file names, `classes.txt`, dcr's `dcr0`..`dcr4`, pvz's
  `levels.xml` and `LawnStrings`, and a8r's `libasphalt8.so`.

| port | setup plan | config table |
| --- | --- | --- |
| **dcr** (pilot) | See `docs/examples/dcr_setup_plan.c`. See below. | `docs/examples/dcr_config_table.c`. version 3, migrate `{display,resolution,"1080","auto",2}` and `{debug,profile_long_frames,"true","false",3}`, `port_config_new_file` for the flag files, no `log_java_calls` row. |
| **lab2** | libs `{"liblabyrinthii.so"}` (PORT_ABI_DIR `"lib/armeabi/"`), libs 0-100, classes 100-150, `finish_in_port = 1`: lab_files.c continues the bar 400-1000; F's main.c or `port_run` calls `rt_setup_finish()`. `apk_requirement` = today's "This port needs Labyrinth 2 1.29 ..." text. `dcr_setup_ipad_from_nro(out, cap)` becomes a port function, see below. | version 1, no migrations. `stick_tilt`, `motion_sensitivity` and `pointer_speed` are CFG_FLOAT (0.1-1, 0.25-4, 1-20). `supersample` and `device_id` are TEXT, derived in `apply`. `dcr_config_set_tilt/level_layout` stay in the port. |
| **abs** | libs `{"libAngryBirdsSpace.so"}` 100-500 "Unpacking the game", classes 550-950 (its per-dex bar is now everyone's). `dcr_setup_move_old_folder` goes to F's rt_migrate. | version 1. `fine_aim_speed` (0.25-4), `pointer_speed` (4-40) and `cursor_size` (0.5-3) are CFG_FLOAT. `texture_memory_mb` (0 or 16..1024) is handled in `apply`. |
| **pvz** | See below. | See below. |
| **sonic** | libs `{"libssasr.so"}` (PORT_ABI_DIR `"lib/armeabi/"`), 50-750 "Unpacking the game's library", classes 750-950. `RT_SETUP_UPDATE_PERMILLE 500`. Sonic's text "Listing the game's Java classes" becomes "Reading ..." (cosmetic). | version 2, migrate `{display,frame_rate,"30","60"}`, `{display,resolution,"720","auto"}` and `{performance,optimised_renderer,"false","true"}` (below 0). The k_mhz, k_lang and k_af mappings go in `apply`. Its format-2 rewrite now goes through .part. |
| **flappy** | libs `{"libflapfire.so"}` 200-800 "Unpacking the game's engine", classes 800-950. The 0-200 APK check stays in the port. | version 1. `opt_bool`/`opt_text` become `rt_config_bool`/`rt_config_get`. volume 0..100 and min_score 1..9999 in `apply`, or CFG_INT with lo/hi (same fallback: the default). |
| **a8r** | Keeps `a8r_setup.c` as its own flow; no plan needed. See below. | See below. |

**dcr setup plan.** The example holds:
- store `"assets/"` 0-800 (check at 800);
- libs main/unity/mono 820-940 by bytes (was 820/860/900 per library);
- classes 940-960;
- DuckTales `RT_STEP_AFTER_ZIP` 960-990, one slice per file;
- `dcr_setup_adopt_apks()`, on `rt_apk_has_package`, until F's APK roles
  replace it.

Delete DCR's `dcr_setup.c` and `dcr_config.c`. Move the two example files in
(renaming the table to `dcr_config.c` is fine).

**lab2: `dcr_setup_ipad_from_nro`.**

```c
int r = rt_setup_copy_from_nro("ipad.ipa", "data/ipad.ipa", NULL,
    "Copying the iPad game's menus and levels", 150, 400);
if (r <= 0) return 0;
rt_root_path(out, cap, "data/ipad.ipa");
return 1;
```

The `.from` stamp file is the same, so there is no re-copy.

**pvz setup plan.**
- libs `{"libnative_code.so","libGameMain.so","libHomura.so"}` 20-140
  "Unpacking the game".
- `port_setup_lib_override` wraps `ensure_port_mod` for libHomura. It now runs
  in the first pass, before any library is unpacked, so its "Installing the
  mod" bar should sit at or below `libs_p0` (20), not at 150, or the bar
  steps back.
- classes 170-200.
- `RT_STEP_WITH_ZIP` steps, in this order: english 200-950, logo, hud,
  levels, gamepad_mode. english must stay before logo; gamepad_mode sets
  `LAWN_GAMEPAD_MODE` before the engine loads.
- `dcr_setup_english_from_nro()` becomes
  `rt_setup_copy_from_nro(romfs, "PvZ Touch English.apk", "", "Unpacking the English files", 0, 150)`,
  with 2 mapped to 1.
- `dcr_setup_font_failed` moves to a pvz file.
- It uses `rt_write_atomic`, `rt_setup_stamp_*` and `rt_setup_extract_entry`
  (with `what` NULL).

**pvz config table.**
- version 2, migrate `{debug,profile_startup_seconds,"90","0",0}`.
- sections `{"cheats", intro}`.
- The homura/cheat columns go in `CfgOpt.tag`. `apply` loops over its own
  table with `rt_config_int`.

**a8r setup.**
- Delete its `dcr_setup_update_from_nro`; set `RT_SETUP_UPDATE_PERMILLE 500`.
- Define `port_setup_progress_draw`.
- `dcr_setup_zips_to_launcher` keeps its `.zips` marker and `is_mod_zip`,
  then calls
  `rt_setup_restart_into_launcher("New game files: restarting to install them")`.
- It may swap its static `read_whole`, `write_atomic`, `mkdirs` and stamps
  for the `rt_*` ones. They share `.setup`: use `rt_setup_stamp_*`, not a
  second MAX_STAMP 8 copy.

**a8r config table.**
- K_INT becomes CFG_INT.
- version 2, migrate `{debug,profile_loading,NULL,"false",0}`.
- sections `mod` and `quick_race` intros.
- `save_header` = its "(also set on the start screen: OPTIONS)" header.
- Spell its resolution row out with choices `"auto,720,1080"`: the start
  screen cycles in that order.
- Delete its `dcr_config_get/choices/set/save`: the runtime's weak wrappers
  cover them.
- `dcr_config_signature` stays in the port.

## 3. Open questions and risks

1. **Missing `[config] version` line counts as format 1** (dcr's rule; sonic
   treated it as old too). pvz and a8r treated it as current. For them, a
   hand-edited file without the line is now upgraded:
   - the whole file is rewritten;
   - pvz: profile 90 becomes 0; a8r: profile_loading becomes false.

   This is harmless, but it is a change.
2. **Migration matching is case-insensitive** (sonic's). dcr's `strcmp` did
   not move "TRUE". Test E in the host run showed only that difference.
3. **dcr pre-v3 files are rewritten whole**, instead of dcr's in-place
   `migrate_line()`. Values are kept. The player's own comments and unknown
   keys are dropped once. Only files from builds 202609241315-1403 are
   affected.
4. **as_choice fallback is the default's index** (a8r's fix). This changes
   behaviour only for invalid values: lab2 `level_layout` garbage now gives
   rotated_left, not portrait.
5. **The library bar is by bytes across all libraries** (dcr 820-940, pvz
   20-140). This replaces the fixed steps. The pvz override bar order is
   described in the pvz setup plan above.
6. **`rt_setup_copy_from_nro` picks the newest NRO that carries the file**
   (lab2's rule). dcr's copy took the newest NRO, whatever it carried. `._*`
   files are now skipped everywhere; dcr/pvz/sonic/flappy/abs's `find_nro`
   did not skip them.
7. **Weak `port_setup_plan`** is reached through the GOT. It is 0 when
   absent, the same mechanism as C's `port_imports`. `crt0_reloc` must leave
   an unrelocated GOT slot at 0.
8. **`dcr_setup.h` includes `<miniz/miniz.h>`**, defining
   `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` if unset. A file that includes it after
   zlib-named miniz, or wants zlib names, must include miniz itself first.
   F's `main.c`, D's `dcr_boost.c` and C's `jni_core.c` compile with it.
9. **The update adds a8r's `log_console_update()`** after the "updating ..."
   log line. It is a no-op unless the log is on screen.
10. **Include order: `rt_settings.h` must come before `dcr_formats.h`**
    wherever `nro_build()` is used. F guards `nro_build` with
    `#ifdef PORT_PAYLOAD_NAME`, and runtime.mk passes it with `-D`, so only a
    host build would notice.
11. **a8r keeps its own setup flow.** Nothing here runs its engine assembly.
    If a8r later moves to the driver: its engine step is `RT_STEP_BEFORE_LIBS`
    0-700, `nlibs` 0, classes 700-850, and its data step `RT_STEP_AFTER_ZIP`
    850-950. Its APK must stay open for play (`a8r_apk_asset`), so the
    engine step opens its own zip.

Nothing from other groups is declared locally. `dcr_game_root` comes from
F's `dcr_path.h`, `dcr_window_set_size` from C's `rt_window.h`, `nro_build`
from F's `dcr_formats.h`, and `log_*` and `fatal_error` from E. All exist now.
