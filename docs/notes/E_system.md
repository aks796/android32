# Group E (system and build): notes

Files: `dcr_sched.c/h`, `watchdog.c` + new `watchdog.h`, `util.c/h`,
`error.c/h`, `host_compat.c`, new `rt_applet.c/h`, `runtime.mk`,
`npdm.json.in`, `tools/docker_build.sh`, `tools/export_github.sh`.

All C files compile warning-free with `tools/check.sh` (GL=1 and GL=0). No
runtime file of mine includes `config.h`; the DESIGN.md game-name grep finds
nothing in them.

| file | base | change |
| --- | --- | --- |
| dcr_sched.c | a8r (= common x5 + the wrapper) | `__wrap_svcSetThreadCoreMask` (GENERIC; runtime.mk passes the flag); `RT_SCHED_SELFTEST`; `rt_settings.h` |
| dcr_sched.h, error.h, host_compat.c | identical x7 | verbatim |
| error.c | identical x7 but the title | `PORT_TITLE ": fatal error"` |
| util.h/.c | sonic | a8r's 1536-byte line; dcr's `log_lock_word()`; `dcr_log_tap` declared in util.h; `RT_LOG_FSYNC` (sonic's fsync + debug.prev.log, now every port); title and note from `PORT_TITLE` / `PORT_SETUP_NOTE`; `log_progress_set_renderer()` (called outside the log lock); `log_console_update` text-only fix; plain `vsnprintf` (NULL-safe through the runtime-wide `--wrap=_svfprintf_r`) |
| watchdog.c / .h | lab2 + dcr's GENERIC parts | `dcr_run_ns()` clock; per-thread CPU ms; GC-paused threads not paused; emulator: no pause, whole-stack `is_return_addr` scan; `MAX_RET` 40; lock-free emergency report; 10 s `svcOutputDebugString` proof of life; `rt_watchdog_add_counter`; weak `port_watchdog_emulator_report`; `RT_WATCHDOG_PRIO/CORE`. `g_raw_fp` dropped (OBSOLETE). `dcr_applet_busy` moved to rt_applet.c. dcr's `dump_main_stack` / main-thread sample / `dcr_emu_stray` left for dcr |
| rt_applet.c / .h | new, from the seven boot files | the lifecycle below |
| runtime.mk, npdm.json.in | lab2's Makefile + NPDM json | see "Build" |
| tools/docker_build.sh | lab2's build.sh + the candidate loop | symlinked runtime/portlibs32 mounted read-only; args passed to make intact |
| tools/export_github.sh | new (lab2 + abs + pvz scripts) | see "Export" |

## 1. Settings and callbacks

### Settings (C)

| name | where | default | ports | what it does |
| --- | --- | --- | --- | --- |
| `PORT_TITLE` | rt_settings.h (required) | none | dcr "Disney Crossy Road", lab2 "Labyrinth 2", abs "Angry Birds Space", pvz "Plants vs. Zombies Touch", sonic "Sonic & SEGA All-Stars Racing", flappy "Flappy Birds Family", a8r "Asphalt 8: Airborne Retry" | progress screen title, `"<title> for Switch"` console banner, error applet `"<title>: fatal error"` |
| `PORT_SETUP_NOTE` | rt_settings.h | "Getting the game ready (after an install or an update)" | all the default | the progress screen's second line |
| `RT_LOG_FSYNC` | util.c | 1 | all 1 (only sonic had it) | fsync debug.log at every ring flush and every 16 boot lines; 0 = fflush only |
| `RT_SCHED_SELFTEST` | dcr_sched.c | 1 | all 1 | the start-up core/time-slice self-test on hardware (always skipped under an emulator) |
| `RT_WATCHDOG_PRIO` | watchdog.c | 0x2B | dcr 0x1C, others 0x2B | the watchdog thread's priority |
| `RT_WATCHDOG_CORE` | watchdog.c | -2 | dcr 2, others -2 | its core |
| `RT_WATCHDOG_BOOT` | rt_applet.c | 0 | dcr 1, others 0 | 1 = hang reports before the frame loop too (dcr's `dcr_boot_in_focus` had no "started" gate) |

debug.prev.log is kept for every port (GENERIC, not a setting).

### Settings (make, before `include runtime/runtime.mk` unless noted)

| name | default | ports | what |
| --- | --- | --- | --- |
| `TARGET` | required | dcrsea_nx, labyrinth2_nx, abspace_nx, pvz_nx, sonicracing_nx, fbf_nx, a8retry_nx | program name; `-DPORT_PAYLOAD_NAME`; NPDM name; `.nsp` / `.build` names. Never rename (launcher romfs contract) |
| `PORT_NPDM_PROGRAM_ID` | required | lab2 0x0100000000001010, dcr ...1013, abs ...100F, pvz ...100E, sonic ...1012, flappy ...1F1A, a8r ...100F | NPDM program id |
| `PORT_NPDM_VERSION` | 0.0.3 | flappy 0.1.0 | |
| `PORT_NPDM_MAIN_STACK` | 0x800000 | flappy 0x400000 | |
| `PORT_NPDM_ADDRSPACE` | 0 | a8r 2 | address_space_type |
| `PORT_NPDM_NAME` / `PORT_NPDM_JSON` | `$(TARGET)` / unset | none | name override / a whole json of the port's own |
| `RT_EXCLUDE` | empty | none yet | runtime files not built (`opensles.c` or `opensles`) |
| `PORT_CFLAGS` `PORT_ASFLAGS` `PORT_LDFLAGS` `PORT_LIBS` | empty | see template | added; `CFLAGS += ...` / `LDFLAGS += ...` after the include work too |
| `PORT_BUILD_H_USERS` | empty | a8r `a8r_setup` | port files that include dcr_build.h (dcr_setup, dcr_config, rt_cfg are built in, in either folder) |
| `PORT_STAMP` | empty | sonic `-$(DCR_VIDEO)` | more of the renderer stamp (a change rebuilds everything) |
| `PORT_CLEAN` | empty | | more for `make clean` |
| `PORTLIBS` | `$(CURDIR)/portlibs32` | | mesa32 lib/ + include/ |
| `DCR_GL_MESA` | 1 when `$(PORTLIBS)/lib/libEGL.a` exists | | as lab2 |
| `CRT0_EXTRA` | empty | | extra crt0_reloc.c flags |
| `RT_SPECS` / `RT_LDSCRIPT` | `runtime/dcr32.specs` / `.ld` | | override only if a port must |

Environment: `PORT_DIR` (both scripts), `DCR_LIBNX32` (exclusive when set),
`DCR_TOOLCHAIN_IMAGE`, `EXPORT_OUT` (another folder named github_repo),
`RT_GITHUB_URL` (the submodule URL).

### Callbacks and API

rt_applet.h (all weak, default no-op / 0):
- `void port_focus_lost(void)`: before the runtime's `log_flush_ring()` + `dcr_time_suspend()`.
- `void port_focus_gaining(void)`: before `dcr_time_resume()`.
- `void port_focus_gained(void)`: after it.
- `void port_process_frozen(unsigned count)`: `dcr_time_freezes()` changed (checked before the focus, as lab2).
- `int port_watchdog_hold(void)`: 1 = frames not expected (called on the watchdog thread).

watchdog.h: weak `void port_watchdog_emulator_report(unsigned secs)` (every
report under an emulator, before the threads are read); required
`uint64_t dcr_boot_frames(void)`.

New API: `rt_applet_start/poll/stop`, `rt_focused` (the focus the game was
last told), `rt_exit_requested`, `rt_request_exit`, `dcr_applet_busy`,
`dcr_applet_is_busy`, `dcr_boot_in_focus` (focused && loop running (first
poll .. stop; or boot too with `RT_WATCHDOG_BOOT`) && !exit && !busy &&
!hold); `rt_watchdog_add_counter(label, read)` (8 slots, "presented" always
first, not counted); `log_progress_set_renderer(rt_progress_fn)`;
`log_lock_word()`; `dcr_log_tap` now declared in util.h.

The system's exit request is recorded by the hook (`rt_exit_requested()`);
every hook event is logged (pvz/abs/a8r's diagnostic, now GENERIC).

## 2. Migration per port

All ports:
- Delete `dcr_sched.c/h`, `util.c/h`, `error.c/h`, `watchdog.c`,
  `host_compat.c`, `dcr32.ld`, `dcr32.specs`, `<TARGET>.json`.
- Frame loop: delete `on_applet`, `apply_focus`, `g_focused`,
  `g_focus_changed`, `g_started`, the hook cookie, the
  `appletSetFocusHandlingMode` + `appletHook` call, `appletUnhook`, the
  `if (!g_focused) dcr_time_resume()` on the way out, and the port's
  `dcr_boot_in_focus()` (the runtime's is strong: a second one fails the
  link). The runtime's main.c (group F) already calls `rt_applet_start()`
  early in boot, before setup and the engine; a port's own call is a no-op
  (idempotent). Use `rt_applet_poll()` where `apply_focus()` was, `rt_focused()` for `g_focused`,
  `rt_exit_requested()` in the loop condition, `rt_applet_stop()` where
  `appletUnhook` was. Port `*_request_exit()` can call `rt_request_exit()`.
- Move the bodies of `apply_focus` into the callbacks (below). Drop the
  "[boot] focus lost/regained" lines (the runtime logs `[applet] ...`).
- Register the audio counter: `rt_watchdog_add_counter("audio writes", fn)`
  after `dcr_watchdog_start()` (fn returns uint32_t).
- System screens (software keyboard, controller applet): wrap in
  `dcr_applet_busy(1)` / `(0)`.
- a8r's progress on its start screen: `log_progress_set_renderer(fn)` where
  `fn(title, note, what, permille)` draws; `a8r_setup.c` then just calls
  `log_console_progress()` (the runtime chooses the console or the renderer).
- Makefile / build.sh / export: see the templates below.

| port | port_focus_lost | port_focus_gaining | port_focus_gained | other |
| --- | --- | --- | --- | --- |
| dcr | `focusChanged(0)`, `pause`, `dcr_vsync_set_paused(1)` | - | `dcr_vsync_set_paused(0)`, `resume`, `focusChanged(1)` | `RT_WATCHDOG_PRIO 0x1C`, `RT_WATCHDOG_CORE 2`, `RT_WATCHDOG_BOOT 1`; counters "JIT flushes", "emulated JIT stores", "signals", "GC suspends" (the total of JIT flushes is no longer printed); `port_watchdog_emulator_report(secs)` = its `secs < 20` main-thread sample + `dump_main_stack` (needs `g_main_thread`: use `envGetMainThreadHandle()`); `dcr_emu_stray` moves to jit_arena.c; `dcr_applet_busy` callers unchanged; loop `while (appletMainLoop() && !g_dcr_quit_requested)` unchanged; `g_running` gate goes |
| lab2 | `pause_play()`, `lab_audio_pause(1)`, `save_all()` | - | `lab_audio_pause(0)` | `port_process_frozen`: `if (lab_game_active() \|\| lab_versus_active()) pause_play()`; counter `lab_audio_mixes`; lab_applet.c: `dcr_applet_busy`; `lab_test.c` keeps `dcr_log_tap` |
| abs | `abs_audio_pause(1)`, `on_update_thread(do_pause, 1)` | - | `on_update_thread(do_resume, 0)`, `abs_audio_pause(0)` | counter `abs_audio_mixes`; the "thawed" check goes (OBSOLETE); hook set before nativeInit is fine (the watchdog starts at the first poll, as `g_started` did) |
| pvz | `window_focus(0)`, `onPauseNative`, `pvz_audio_pause(1)` | - | `pvz_audio_pause(0)`, `onResumeNative`, `window_focus(1)` | way out: `rt_applet_stop(); if (rt_focused()) { window_focus(0); onPauseNative }`; counter `pvz_audio_writes`; pvz_text.c: `dcr_applet_busy` |
| sonic | `dcr_boost_idle`, `ssr_perf_focus(0)`, mp disconnect, `save_state`, `set_pause`, `ssr_split_focus(0)`, input release, video skip, music/audio pause | `ssr_perf_focus(1)`, `dcr_boost_frame_begin()`, `ssr_clock_resync()` | audio/music, `N.resume`, `ssr_split_focus(1)` | `ssr_boot_system_dialog(on)` -> `dcr_applet_busy(on)` (ssr_input.c, ssr_menu.c, ssr_split.c); counter: wrap `ssr_audio_mixes` (unsigned long) in a uint32_t function; keep `dcr_log_tap` in ssr_test.c; `--wrap=svcSetThreadCoreMask` leaves its Makefile |
| flappy | `fbf_input_reset()`, gate reset, engine pause (if up, not user-paused), `fbf_audio_pause(1)` | - | `fbf_audio_pause(0)`, engine resume | the 2 s frame-gap check becomes `port_process_frozen` (lost + gained actions), once flappy uses the runtime's bionic_time.c (freeze watch); `fbf_controller_screen`: `dcr_applet_busy`; counter `dcr_audio_blocks`; NPDM version 0.1.0, stack 0x400000, id 0x...1F1A |
| a8r | `a8r_perf_focus(0)`, `post_wait(EV_PAUSE, 3000)`, `dcr_audio_pause(1)` | `a8r_perf_focus(1)` | `dcr_audio_pause(0)`, `onResume`, `post_wait(EV_RESUME, 3000)` | `port_watchdog_hold()` returns `g_paused`; loop `if (!rt_focused() \|\| !g_gl_up)`; counter `dcr_audio_writes`; `PORT_BUILD_H_USERS := a8r_setup`; `PORT_NPDM_ADDRSPACE := 2`; its mesa wraps (below); renderer for the progress screen |

### Port Makefile template (lab2)

```make
#---------------------------------------------------------------------------------
# Labyrinth 2 -- Nintendo Switch wrapper (32-bit / AArch32)
# (the port's own header comment: what it ships, what it reads)
# The build is the android32 runtime's (runtime/runtime.mk); ./build.sh runs it
# in the toolchain container.
#---------------------------------------------------------------------------------
TARGET               := labyrinth2_nx
PORT_NPDM_PROGRAM_ID := 0x0100000000001010
include runtime/runtime.mk

# stb_vorbis / stb_image (public domain) are compiled into lab_audio.c and
# lab_draw.c; they warn about things that are theirs, not ours.
$(BUILD)/lab_audio.o $(BUILD)/lab_draw.o: CFLAGS += -Wno-sign-compare -Wno-unused-function \
  -Wno-unused-value -Wno-misleading-indentation -Wno-shadow -Wno-implicit-fallthrough \
  -Wno-type-limits -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-array-bounds
$(BUILD)/lab_audio.o: $(SOURCES)/stb_vorbis.inc

.PHONY: check
check:
	@echo "run on the host: python3 tools/gen_imports.py --libs <apk>/lib/armeabi"
```

Sonic (the most involved):

```make
TARGET               := sonicracing_nx
PORT_NPDM_PROGRAM_ID := 0x0100000000001012
# FFmpeg (intro movie, music), when ffmpeg32's libraries are in portlibs32/
DCR_VIDEO  := $(if $(wildcard portlibs32/lib/libavcodec.a),1,0)
PORT_STAMP := -$(DCR_VIDEO)
ifeq ($(DCR_VIDEO),1)
PORT_LIBS  := -L$(CURDIR)/portlibs32/lib -lavformat -lavcodec -lavutil
endif
ifneq ($(wildcard resources/xbox360/splitscreen_card.png),)
PORT_CFLAGS  := -DDCR_XCARD_BAKED=1
PORT_ASFLAGS := -DDCR_XCARD_BAKED=1
endif
include runtime/runtime.mk

ifeq ($(DCR_GL_MESA),1)
LDFLAGS += -Wl,--wrap=nwindowDequeueBuffer   # ssr_perf.c
endif
FFMPEG_USERS := $(BUILD)/ssr_media.o $(BUILD)/ssr_video.o
$(FFMPEG_USERS): $(BUILD)/%.o: $(SOURCES)/%.c $(RENDERER_STAMP) | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -fno-short-enums -DDCR_VIDEO=$(DCR_VIDEO) -c $< -o $@
$(BUILD)/ssr_gfx.o: CFLAGS += -Wno-sign-compare -Wno-unused-function ...
```

Others: abs `PORT_LIBS` = video + `-lmbedtls -lmbedx509 -lmbedcrypto`,
`PORT_CFLAGS := -DMBEDTLS_USER_CONFIG_FILE="<abs_mbedtls_user_config.h>"`,
its `-fno-short-enums` rule and incbin prerequisite after the include; pvz
video libs + `pvz_video.o` rule + `pvz_res.o` incbin prerequisites; dcr
`$(BUILD)/mod_blob.o: mod/dcrmod.dll` (drop its own `--wrap=_svfprintf_r`,
now in runtime.mk); flappy the `fbf_assets.o` rule; a8r after the include:
`ifeq ($(DCR_GL_MESA),1)` `LDFLAGS += -Wl,--wrap=_mesa_is_format_srgb
-Wl,--wrap=nwindowDequeueBuffer -Wl,--wrap=_mesa_glthread_finish_before`
(not `nouveau_bo_new`: runtime.mk has it, and gl_mesa.c the wrapper).
`make rt-files` prints what is built from where.

### Port build.sh

```sh
#!/bin/sh
# Build <TARGET>.nsp in the AArch32 toolchain container: the runtime's
# docker_build.sh (arguments go to make: ./build.sh clean, DCR_GL_MESA=0,
# rt-files). libnx32: ../libnx32/prefix and friends, or DCR_LIBNX32.
exec "$(dirname "$0")/runtime/tools/docker_build.sh" "$@"
```

Ports whose build.sh does more first (dcr, lab2 and pvz make launcher romfs
extras) keep that part, then `exec` the line above.

### Export

`runtime/tools/export_github.sh` replaces make_github_repo.sh (lab2),
export_github.sh (abs) and export_github_repo.sh (pvz). A port lists what it
publishes in `github_export.list` (else the default list: README.md NOTES.md
LICENSE Makefile build.sh .gitignore icon.* source tools launcher). lab2 needs
`portlibs32/README.md` there (it wrote one); pvz `mod` (its build/out/.git are
dropped anyway), `resources`, `logo.png`, `icon.jpg`; abs `sd`; sonic
`resources` would include the disc-made card only if the port's .gitignore does
not ignore it: sonic has no .git at its root, so list `resources/` sub-paths
explicitly and never `resources/xbox360`. `github_export.gitignore`, if
present, becomes github_repo/.gitignore. Tested on scratch copies of lab2
(no root .git) and abs (root .git: its .gitignore honoured): `.git` byte-for-
byte unchanged, gitlink `160000 <runtime HEAD> runtime` staged, rerun
idempotent; OUT=/, $HOME, a symlink, a file, the runtime, a folder holding the
port, PORT_DIR=/ and $HOME are all refused before anything is touched.

## 3. Open questions and risks

- **Docker mount caching.** The first check.sh run after rewriting a file
  saw it truncated at its old size ("unterminated comment", "expected ')' at
  end of input"); the rerun was clean. If a group sees impossible syntax
  errors, rerun before debugging.
- **Symlinked runtime in Docker.** docker_build.sh mounts the real runtime
  path on top of the `/work/runtime` symlink. Docker resolves the link inside
  the container and mounts there; tested with an absolute and a relative
  link (Docker Desktop, macOS). The runtime is read-only in the container,
  so nothing may write under runtime/ during a build (nothing does).
- **Header overrides don't reach runtime .c files**: `#include "x.h"` looks
  in the including file's folder first. A port overrides `.c`/`.S` files,
  not headers. port_config.h works because the runtime has none.
- **RT_WATCHDOG_BOOT** is new (not in the plan): dcr's watchdog reported hangs
  during engine start-up; every other port gated on `g_started`. The
  "started" moment is now the first `rt_applet_poll()`, which every port
  reaches right after its old `g_started = 1`.
- **Watchdog flush skip**: the 5 s flush is skipped while the log lock is
  held (`log_lock_word()`), so a hang holding the log lock can no longer
  block the watchdog before its emergency report (dcr's order could). A fix,
  not hardware-proven.
- **dcr's report line** changes: the watchdog now prints `name` and `CPU ms`
  on the paused-thread line and counters as `+delta` only.
- **flappy** gets freeze handling only through B's always-on freeze watch;
  `port_process_frozen` cannot tell whether a focus message is also pending
  (its old check skipped the pause/resume then). Harmless double pause.
- **Not expressible with the callbacks**: nothing found that a port does in
  its focus handling. Operation-mode changes (dock/undock) only re-read the
  focus, as before; a `port_operation_mode` callback would be new.
- **a8r and abs share program id 0x010000000000100F.** The launcher retargets
  the NPDM to its forwarder's title (dcr_exefs.h `exefs_build_override`), so
  it matters only for emulator/hbl runs. Integrator: pick a8r a free id.
- **Suggestion for group C**: a8r's watchdog counter is `dcr_audio_writes()`
  from opensles.c, now a runtime file; opensles.c could register it itself
  (`rt_watchdog_add_counter("audio writes", dcr_audio_writes)` when a player
  starts), so OpenSL ES ports get it for free.
- **The submodule URL** defaults to `https://github.com/aks796/android32.git`
  when the runtime has no `origin` remote (RT_GITHUB_URL overrides).
- **dcr_build.h** is made before any compile (order-only) and rebuilds
  dcr_setup, dcr_config, rt_cfg (and `PORT_BUILD_H_USERS`) when the minute
  changes; any other includer is rebuilt through its -MMD dependency.
- Nothing declared locally from other groups: `bionic_pthread.h` (B: `gc_paused`,
  `b_lock_words`, `b_thread_foreach_nolock`), `exc_handler.h` (A),
  `gl_layer.h` (D: `dcr_gl_frames`) and `dcr_time.h` (B) already had them.
  The link needs B's `__wrap__svfprintf_r/_vfprintf_r` (present) and D's
  `__wrap_nouveau_bo_new` (present in gl_mesa.c under `DCR_GL_MESA`).
