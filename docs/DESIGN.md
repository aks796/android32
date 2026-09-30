# android32: design

android32 is the runtime shared by the 32-bit Android-game wrappers for the
Switch. It holds the .so loader, bionic libc shims, JNI core, NDK shims, GL
glue, clocks, paths, setup, config, crash handler, watchdog and launcher. Each
port keeps only its game-specific code. A runtime fix is made once, and every
port gets it on its next build.

Ports: dcr (`~/thirtytwo/disneycrossroad_sea/dcrsea_nx`), lab2
(`~/thirtytwo/labyrinth2/labyrinth2_nx`), abs (`~/thirtytwo/abspace/abspace_nx`),
pvz (`~/thirtytwo/pvztouch/pvztouch_nx`), sonic (`~/sonicracing/sonicracing_nx`),
flappy (`~/flappybirdsfamily/flappybirdsfamily_nx`), a8r
(`~/thirtytwo/a8retry/a8retry_nx`). Each has its own diverged copy of these
files in `source/`. The merge plans per group are in the session scratchpad,
`runtime_plan/report_*.md`.

## Layout

```text
android32/
├── source/            runtime sources (C, asm, headers)
├── launcher/          the 64-bit launcher NRO, shared (a port adds hooks + icon)
├── tools/             gen_imports.py, docker_build.sh, export_github.sh, check.sh
├── runtime.mk         included by each port's Makefile
├── npdm.json.in       the NPDM template
├── dcr32.ld, dcr32.specs
├── test/              compile check: a test port (Disney Crossy Road's values)
└── docs/              DESIGN.md, SETTINGS.md, MIGRATING.md
```

In a port, the runtime sits at `<port>/runtime`: a git submodule on GitHub, a
symlink while developing. The port's Makefile sets a few variables and includes
`runtime/runtime.mk`. The port's `source/` holds `port_config.h`, the game's
files, and any runtime file it overrides.

## How a port changes the runtime's behaviour

In order of preference:

1. **SETTING**: a macro in the port's `source/port_config.h`.
   - `PORT_*` names the game and where it lives: `PORT_TITLE`, `PORT_NAME`,
     `PORT_PACKAGE`, `PORT_ROOT_PATH`, ...
   - `RT_*` changes how the runtime behaves, e.g. `RT_TIME_WATCH_PRIO`.
   - Settings several files share have their defaults in `source/rt_settings.h`.
   - A setting only one file reads has its `#ifndef` default in that file, next
     to the code, with a comment giving what it does and each port's value.
2. **CALLBACK**: a weak function the runtime calls. The runtime has a weak
   default definition that keeps today's behaviour. The port defines a strong
   one. Every callback is named `port_*` and declared in the header of the
   runtime module that calls it.
3. **OVERRIDE**: the port defines the whole function (the runtime's copy is
   weak), or the whole file. A port file with the same name as a runtime file
   replaces it at build time, which lets a port migrate one file at a time.

Required data a port provides (not callbacks):
- the JNI tables `jni_method_defs[]`, `jni_field_defs[]`, `jni_class_supers[][2]`
  and `jni_missing_classes[]`;
- the generated `imports.c`;
- `port_load()` and `port_run()`;
- `dcr_boot_frames()`.

## Rules for runtime code

- No game names, game strings or game macros. That covers `PVZ_*`, `LAB_*`,
  `FBF_*`, `A8R_*`, `SSR_*`, `ABS_*`, `DCR_ROOT_PATH`, `DCR_PACKAGE`,
  "Plants vs. Zombies", package names and so on. Use the `PORT_*` settings.
  Check with:
  `grep -nE "(PVZ|LAB|FBF|A8R|SSR|ABS)_|DCR_(ROOT_PATH|PACKAGE|APK_NAME)|pvz_|lab_|fbf_|a8r_|ssr_|abs_" source/*`
- Runtime files include `"rt_settings.h"`, never a port's `"config.h"`.
  `test/port/config.h` is a temporary copy of lab2's, only for files that have
  not been converted yet. The end state is that no runtime file includes it.
- Keep every exported name ports call today (`dcr_*`, `b_*`, `jni_*`, `log_*`,
  `so_*`, ...). New runtime API is `rt_*`. New callbacks are `port_*`.
  Renaming an existing export is allowed only where this document says so.
- Keep hardware-proven behaviour byte-for-byte unless the merge plan calls the
  change a fix. When the plans disagree with the code, the code wins; note it.
- Match the ports' style:
  - a block comment at the top saying what the file is and why;
  - comments that explain why, not what;
  - two-space indent, `snake_case`;
  - "MIT." at the end of the header comment.
- Every runtime file must compile with `tools/check.sh <file>`, and with
  `GL=0` for the renderer files. Warnings count: fix them.

## Groups (who owns which files)

Each group owns its files outright. Nobody edits another group's files.
If you need something from another group that doesn't exist yet:
- declare it `extern` locally in your own `.c` with a `/* from <group> */`
  comment;
- list it in your notes file.

The interfaces section below fixes the cross-group names up front.

| Group | Merge plan | Owns |
| --- | --- | --- |
| A loader | report_loader.md | `so_util.c/h`, `crt0_reloc.c`, `exc_handler.c`, `exc32.S`, `code_flush.c/h`, `codespace.h`, `selfproc.c/h`, `nx_init.c/h`, `nx32_virtmem.c`, `emu_fixups.c` (+ new `emu_fixups.h`), `kuser.S`, `dcr32.ld`, `dcr32.specs` |
| B bionic | report_bionic.md | `bionic.h`, `bionic_*.c/h/S` (all), `dcr_time.h`, `dcr_net.h`, new `bionic_cxx.c` |
| C jni | report_jni_gl.md §1-3, §10 | `jni_core.c`, `jni.h`, `android_ndk.c`, new `rt_window.c`, `opensles.c`, `imports.h`, `tools/gen_imports.py` |
| D gfx | report_jni_gl.md §4-8, §12-13 | `gl_mesa.c`, `gl_null.c`, `gl_layer.h`, `gl_blit.c/h`, `dcr_boost.c` (+ new `dcr_boost.h`), new `rt_audout.c/h`, new `rt_pad.c/h` |
| E system | report_system_build.md | `dcr_sched.c/h`, `watchdog.c` (+ new `watchdog.h`), `util.c/h`, `error.c/h`, `host_compat.c`, new `rt_applet.c/h`, `runtime.mk`, `npdm.json.in`, `tools/docker_build.sh`, `tools/export_github.sh` |
| F files | report_files_launcher.md | `dcr_path.c/h`, `dcr_apkcache.c` (+ new `.h`), `dcr_dircache.c` (+ new `.h`), `dcr_manifest.c/h`, `dcr_formats.h`, `dcr_exefs.h`, `dcr_migrate.h` → new `rt_migrate.c/h`, new `rt_apkfind.c/h`, `main.c`, `launcher/` |
| G setup | report_setup_config.md | `dcr_setup.c` (+ new `dcr_setup.h`), `dcr_config.c/h` → new `rt_cfg.c/h` |

`rt_settings.h`, `test/`, `tools/check.sh` and the docs other than your own
notes file belong to the integrator (the main session). Ask in your notes if
something there needs changing.

## Interfaces between groups (fixed names)

Settings shared across files are in `rt_settings.h`:
- `PORT_TITLE`, `PORT_NAME`, `PORT_PACKAGE`, `PORT_PAYLOAD_NAME` (set by
  runtime.mk from `TARGET`)
- `PORT_ROOT_PATH`, `PORT_OLD_ROOT_PATHS`, `PORT_BANNER`, `PORT_SETUP_NOTE`,
  `PORT_NSP_NAME`, `PORT_ABI_DIR`, `PORT_APK_DEFAULT_NAME`,
  `PORT_SO_REGION_BYTES`
- `RT_GFX_RESERVE_MB`, `RT_SCREEN_W/H`, `DCR_GL_MESA`

Where each group's reports use `A32_*`, `DCR_*` (new ones) or `JNI_*` for a new
setting, name it `RT_*` instead (`A32_TIME_SHIFT` becomes `RT_TIME_SHIFT`, and
so on). Where they use `RT_ROOT_PATH`, `DCR_ROOT_PATH`, `RT_NRO_NAME`,
`RT_PORT_NAME`, `A32_PORT_NAME` or `A32_PACKAGE`, use the `PORT_*` names above.

**B, time** (`dcr_time.h`):
- `dcr_time_init`, `dcr_time_suspend`, `dcr_time_resume`
- `u64 dcr_monotonic_ns(void)`: clock with suspension and freezes removed
- `u64 dcr_run_ns(void)`: tick minus freezes
- `unsigned dcr_time_freezes(void)`: count of freezes found

**E, lifecycle** (`rt_applet.h`):
- `rt_applet_start()`, which sets `SuspendHomeSleepNotify`, then `appletHook`
- `rt_applet_poll()`
- `rt_applet_stop()`
- `int rt_focused(void)`
- `int rt_exit_requested(void)`
- `void dcr_applet_busy(int on)`
- `int dcr_applet_is_busy(void)`
- `int dcr_boot_in_focus(void)`

Its callbacks are `port_focus_lost()`, `port_focus_gaining()`,
`port_focus_gained()`, `port_process_frozen(unsigned count)` and
`port_watchdog_hold()`.

The watchdog reads `dcr_boot_frames()`, which each port provides.

**E, util** (`util.h`): today's `log_*` functions, plus:
- `debugPrintf`
- `dcr_is_emulator`
- `log_lock_word`
- `dcr_log_tap`
- `typedef void (*rt_progress_fn)(const char *title, const char *note, const char *what, int permille); void log_progress_set_renderer(rt_progress_fn);`

**F, paths** (`dcr_path.h`): these move out of every `main.c`:
- `const char *dcr_game_root(void)`
- `void dcr_set_game_root(const char *)`
- `const char *dcr_apk_path(void)`
- `void dcr_set_apk_path(const char *)`

Also exported from here: `dcr_translate_path`, `dcr_path_prepare_dirs`, `dcr_cwd`, and `const char *dcr_data_dir(void)`.

**G, config** (`rt_cfg.h`):
- `typedef struct RtConfig { int res_w, res_h, boost, gl_selftest, boot_log, log_jni; } RtConfig;`
- `const RtConfig *rt_config(void)`
- the INI engine

This is the only config runtime files read. Each port keeps `dcr_config.h` with its own `DcrConfig` and option table. `dcr_config_load()` and `dcr_config()` stay the port-facing names.

**G, setup** (`dcr_setup.h`):
- `dcr_setup_from_apk`
- `dcr_setup_update_from_nro`
- `dcr_setup_progress`
- `dcr_setup_did_work`
- `rt_setup_copy_from_nro`
- the port's setup plan (named `port_*`)

**D, boost** (`dcr_boost.h`): every `dcr_boost_*` in today's ports, plus `dcr_launch_ready()`.

**D, audio** (`rt_audout.h`): `rt_audout_open/rate/submit/cancel/close/selftest`.

**C, imports** (`imports.h`):
- `dcr_imports`, `dcr_imports_count`, `dcr_import_lookup`
- weak `const DynLibFunction port_imports[]` and `const int port_imports_count`, searched before `dcr_imports` (A's `so_resolve` and B's `dlsym` both go through `dcr_import_lookup`, or search `port_imports` first the same way)
- weak `const DynLibFunction *port_module_imports(const char *module, int *count)`

**A, loader**:
- `void *port_import_interpose(const char *sym, void *real)`: B's `dlsym` uses it too
- `const char *port_code_region(uint32_t addr)`
- `int port_exception_hook(...)`
- `int port_code_flush(void *, size_t)`
- the codespace hooks `cs_mmap`, `cs_munmap`, `cs_mprotect`, `cs_write`, `cs_rw_alias`, `g_cs_armed`. These are weak defaults in A's files; B's `bionic_mem.c` calls them.

**B, callbacks**:
- `port_net_owns/close/fcntl/ioctl/ready/socket/bind/sendto/recvfrom` (renamed from `dcr_net_*`)
- `port_thread_tag_for_new`, `port_thread_tag_enter`
- `port_gc_signal`, `port_on_fatal_signal`
- `port_prof_begin/end`

**C/D, callbacks**:
- `port_jni_invoke` (the report's `jni_hook_invoke`)
- `port_gl_wrap`, `port_gl_before_swap`, `port_gl_after_swap`
- `port_cpu_boost_set`, `port_perf_clocks`, `port_boost_snapshot`, `port_boost_format`

**F, boot** (runtime `main.c`):
- ports provide `int port_load(const char *apk)` and `void port_run(void)`
- optional `port_report_boot`, `port_before_update`, `port_after_apk_find`, `port_audio_selftest`, `port_apk_help`, and the APK role table

## Settings that change behaviour for a port

Where the plan offers a setting to keep one port's old behaviour, the default
is the behaviour most ports have. That port's value goes in the table. Where a
report says "GENERIC", every port gets the change. The null-safe printf
(`RT_NULL_SAFE_PRINTF`) defaults to 1: it is what bionic does, and dcr runs it
on hardware. With 0, the `b_` printf shims pass straight through to newlib.

## Notes files (each group writes one)

`docs/notes/<group>.md`, in three parts:
1. **Settings and callbacks** you added: name, default, each port's value, one
   line on what it does.
2. **Migration** per port: what the port must change, such as renames, files
   to delete, new definitions it must provide, and settings it needs.
3. **Open questions and risks** for the integrator: anything you couldn't
   settle, and anything you declared locally from another group.

## Process

- Don't run `git commit`. The integrator commits each group.
- Don't modify anything under a port's folder. They are read-only references.
- Compile with `tools/check.sh <your files>` (about 10 s). The container is
  shared, so check only your files.
