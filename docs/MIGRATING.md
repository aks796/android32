# Moving a port onto android32

This is how a port's own copies of the shared files are replaced by the
runtime. Disney Crossy Road went first. Its branch is the worked example:
`~/thirtytwo/disneycrossroad_sea/dcrsea_nx_a32`, branch `android32`, commit
`3e7abc2`. Read [DESIGN.md](DESIGN.md) first. The per-port details are in
[notes/](notes): every group's section 2 has a paragraph or a table row for
each port.

## Before starting

- Work in a separate copy so the current build stays testable: a git
  worktree (`git worktree add ../<port>_a32 -b android32`), or a copied
  folder if the port has no git repository.
- Link the runtime in: `ln -s <relative path>/libraries/android32 runtime`.
  `docker_build.sh` mounts the link's real path. Link `portlibs32/` (and
  anything else ignored that the build needs) from the main copy.
- Add `/runtime` (and the other links, without a trailing slash) to
  `.gitignore`. On GitHub the runtime becomes a submodule, which
  `runtime/tools/export_github.sh` writes.
- Don't edit the runtime. If the port needs a runtime change (a setting, a
  callback, a fix), say so to the Disney Crossy Road session, which keeps the
  runtime. Until then, use a port file that overrides the runtime's (same
  file name) or a weak function.

## Steps

1. **`source/port_config.h`**, macros only (the assembler and the launcher
   read it too):
   - `PORT_TITLE`, `PORT_NAME`, `PORT_PACKAGE`;
   - the port's other `PORT_*` and `RT_*` values. Each group's notes,
     section 1, has a table with every port's value. Disney Crossy Road's
     file shows the layout.
2. **Delete the port's copies of the runtime's files.** Every file in the port's `source/` that has the
   same name as one in `runtime/source/` goes. So do `dcr32.ld`,
   `dcr32.specs`, `<TARGET>.json`, `tools/gen_imports.py`,
   `launcher/source/main.c`, `launcher/source/fwd_mine.h` and
   `launcher/Makefile`. Keep a file under the same name only to override the
   runtime's on purpose.
3. **`config.h`** keeps only the game's own macros. The shared ones (root
   path, `SO_REGION_BYTES`, `GFX_RESERVE_MB`, screen size, `DEBUG_LOG`,
   `DCR_GL_MESA`) are settings now.
4. **`main.c` becomes `port_load(apk)` and `port_run()`**, in a port file
   (the runtime's `main.c` runs the rest). F's notes have a table of what
   goes into each, per port. Drop the port's own `g_root`/`dcr_game_root`,
   APK search, old-folder move and focus-mode call. The runtime does those.
5. **`dcr_setup.c` becomes `port_setup_plan`, and `dcr_config.c` becomes an
   option table.** See G's notes and `docs/examples/`. Keep `.setup` key
   names, config.ini keys, their order, defaults and help text exactly as
   they are.
6. **Focus and HOME/sleep.** The frame loop calls `rt_applet_poll()` and
   checks `rt_focused()`, then `rt_applet_stop()` at the end. What the old
   focus handler did goes into `port_focus_lost` / `port_focus_gained`. The
   port's `dcr_boot_in_focus`, hook cookie and `appletSetFocusHandlingMode`
   go. E's notes have the table.
7. **Renamed callbacks:**
   - `dcr_net_*` → `port_net_*`
   - `dcr_gc_signal` → `port_gc_signal`
   - `dcr_icall_interpose` → `port_import_interpose`
   - `ssr_gl_wrap` → `port_gl_wrap`
   - `pvz_looper_run_main` / `pvz_config_locale` → `dcr_*`
   - the thread tags → `port_thread_tag_*`

   Each group's notes list them.
8. **Watchdog counters:** `rt_watchdog_add_counter(label, fn)` after
   `dcr_watchdog_start()`.
9. **The import table:**
   - put the port's modules in `tools/imports.cfg`
     (`MODULES libfoo.so ...`);
   - run `python3 runtime/tools/gen_imports.py`;
   - compare the symbol list with the old `imports.c`. Only the printf family
     may move to `b_` shims.

   An old `imports.c` doesn't link: the runtime defines `dcr_import_lookup`
   now.
10. **Makefile:** set `TARGET` and `PORT_NPDM_PROGRAM_ID` (plus the other
    `PORT_NPDM_*`, `PORT_CFLAGS`/`LIBS`/`LDFLAGS` the port needs), then
    `include runtime/runtime.mk`, then the port's own rules. E's notes have
    templates. `build.sh` execs `runtime/tools/docker_build.sh`.
    `./build.sh rt-files` prints what is built from where.
11. **Launcher:**
    - `launcher/Makefile` sets `APP_TITLE`, `APP_AUTHOR`, `APP_VERSION`,
      `TARGET` (`PORT_NAME`) and `PORT_PAYLOAD` (the wrapper's `TARGET`), then
      includes `runtime/launcher/launcher.mk`.
    - `launcher/build.sh` is three lines.
    - Romfs extras go in `launcher/romfs_extras.sh`.
    - Hooks go in a `launcher/source/*.c`, with `LAUNCHER_SOURCES := source`.

    F's notes cover each port.
12. **Build:** `./build.sh`, then `launcher/build.sh`. Fix what the compiler
    and linker say. The usual cause is a multiple definition: a port file
    still defines what the runtime now has. Delete the port's copy, unless it
    is a deliberate override, in which case make it the whole function and
    check that the runtime's one is weak.
13. **Test** in Ryujinx, if the game runs there.
    - Only one Ryujinx at a time across all sessions: check
      `pgrep -x Ryujinx` first.
    - Compare a run of the old build and the new one with the same input:
      frame rates, the boot lines, a clean quit.
    - For first-launch setup, delete `.setup` and the unpacked libraries,
      then check that the new `.setup` and files are byte-identical to the
      old ones.
14. **Package** an `SD_CARD.zip` in the branch's folder for a hardware test.
    Its build number is newer than the main build's, so the user tests the
    main build first, then this one.

## What the pilot ran into

- **A port memory override:**
  - Disney Crossy Road keeps its JIT-aware `b_mmap`, `b_munmap`,
    `b_mremap`, `b_mprotect`, `b_mmap_bytes` and `b_memcpy`/`memmove`/`memset`
    in `dcr_jit_mem.c`. They are strong and replace the runtime's weak ones.
  - Its old `b_madvise` had to go; the runtime's isn't weak, and they're
    the same.
- **Exception handling:** port code goes behind `port_exception_hook`
  (`ThreadExceptionInfo32`/`ThreadExceptionFrame32`, `rt_exc_reg` for
  registers).
- **Cache flushes:** the JIT arena's flush became `port_code_flush`. The
  runtime's `code_flush.c` has the flip page.
- **Emulator captures:** they came out all black in Ryujinx for both the old
  and new builds, at 1080p. That's not a regression.
- **The APK search:** it now finds the APK by its manifest. The log shows
  `[apk] <name>: <role>`.

## After hardware testing

When the user has played the branch's build on hardware:
- merge the branch into the port's main line;
- refresh `github_repo/` with `runtime/tools/export_github.sh`, which never
  touches `github_repo/.git`;
- note the runtime commit in the port's NOTES.md.

Don't push anything.
