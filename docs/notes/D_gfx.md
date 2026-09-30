# Group D (graphics, boost, audio, pad): notes

Files: `gl_mesa.c`, `gl_null.c`, `gl_layer.h`, `gl_blit.c/h`, `dcr_boost.c` +
new `dcr_boost.h`, new `rt_audout.c/h`, new `rt_pad.c/h`.
All compile warning-free with `tools/check.sh`, and `GL=0 tools/check.sh
gl_null.c gl_mesa.c gl_blit.c`. The non-default variants (`RT_GL_CHECK=1`,
`RT_GL_SWAP_ENDS_FRAME=0`, `RT_CAPTURE_TRIES=300`, `RT_GL_BLIT=1` with GL=0
and GL=1, `RT_BOOST_WATCH_THREAD=1`, `RT_BOOST_LAUNCH_AFTER_PICTURE_MS=25000`,
`RT_MAIN_THREAD_NAME="UnityMain"`, `RT_AUDOUT_FRAMES=512`) were compiled once
by hand in the same container (check.mk with `CC="arm-none-eabi-gcc -D..."`).
None of my files includes `config.h`, and the game-name grep finds nothing.

| file | base | change |
| --- | --- | --- |
| gl_layer.h | lab2 | declares the whole renderer API: `dcr_gl_lookup/frames/selftest`, `dcr_gl_request_capture(_named)`, `dcr_gl_capture_now`, `dcr_frame_hook`, `dcr_present_hook`, `rt_egl_start_glthread`, and the callbacks `port_gl_wrap`, `port_gl_before_swap`, `port_gl_after_swap` |
| gl_mesa.c | lab2 | `rt_settings.h`; `RT_GL_SWAP_ENDS_FRAME`, `RT_CAPTURE_TRIES`, `RT_GL_CHECK` (a8r's check wrappers and `.dump_textures`, without its profiler and card code); `port_gl_wrap` (sonic's `ssr_gl_wrap`); swap callbacks (a8r's perf); `dcr_gl_capture_now` (a8r); `rt_egl_start_glthread` (a8r, plus the old name `b_egl_start_glthread`); `__wrap_nouveau_bo_new` (a8r); `RT_SCREEN_W/H`; the obsolete `__wrap__mesa_is_format_srgb` is not taken |
| gl_null.c | lab2 (= pvz, only the vendor string differs) | `rt_settings.h`; vendor strings = `PORT_NAME`; no-op `dcr_gl_selftest` (returns 1), `dcr_gl_request_capture(_named)`, `dcr_gl_capture_now`, `rt_egl_start_glthread` / `b_egl_start_glthread` (return 0), so the API links with either renderer |
| gl_blit.c/h | sonic (superset: alpha, meshes) | body inside `#if RT_GL_BLIT` (default 0); header comment only |
| dcr_boost.c | lab2/abs/pvz + dcr + sonic + a8r | see below |
| dcr_boost.h | new | every `dcr_boost_*` the ports call, `dcr_launch_ready`, and the four callbacks |
| rt_audout.c/h | the audout core, identical in all 7 | `rt_audout_open/rate/submit/cancel/close/selftest/stats`; a mixer pump (`rt_audout_pump_start/stop`, `rt_audout_pause`, `rt_audout_pump_blocks`); weak `port_audio_selftest` |
| rt_pad.c/h | dcr_input.c (+ lab2/sonic, identical pieces) | `rt_pad_setup`, `rt_pad_slot`, `rt_pad_slot_id`, `rt_pad_is_single`, `rt_pad_single_buttons`, `rt_pad_sideways`, `rt_pad_read`, `rt_pad_style_name` |

dcr_boost.c merge, in detail:
- dcr: `Snap`/`Diff`/`take_busiest` structures, the applet-busy check in
  `dcr_boost_poll`, the launch past the first picture
  (`dcr_boost_first_picture`, `dcr_launch_ready`, `dcr_boost_launch_tick`),
  `dcr_boost_cpu_report` + SoC temperature (only linked if called).
- sonic: `dcr_boost_idle`, the watcher thread (`RT_BOOST_WATCH_THREAD`), a
  hold's time counted apart from long frames, `dcr_boost_report` only when
  something changed.
- a8r: `port_cpu_boost_set`, `port_perf_clocks`, `port_boost_snapshot`,
  `port_boost_format`, the 700-byte frame line, file reads on the start-up
  line.
- lab2/abs/pvz: `dcr_boost_hold` (a8r had dropped it; kept).
- `dcr_config()->boost` became `rt_config()->boost` (group G).
- Small fix: `dcr_boost_idle` now counts the boosted milliseconds and clears
  the "is a hold's" flag (sonic's left it set, so the next long frame's boost
  was counted as the hold's).

## 1. Settings and callbacks

### Settings (each has its `#ifndef` default in the file that reads it)

| name | where | default | ports | what it does |
| --- | --- | --- | --- | --- |
| `RT_GL_SWAP_ENDS_FRAME` | gl_mesa.c | 1 | dcr 0 | 1: `eglSwapBuffers` calls `dcr_boost_frame_end(frame)` and then `dcr_boost_frame_begin()`. 0: the port brackets its frames (dcr: around nativeRender in dcr_boot.c) |
| `RT_CAPTURE_TRIES` | gl_mesa.c | 30 | dcr 300 | under the emulator, a capture skips all-black read-backs for up to this many frames; the last one is saved anyway |
| `RT_GL_CHECK` | gl_mesa.c | 0 | a8r 1 (it had them on); others 0 | wraps glTexImage2D, glCompressedTexImage2D, glRenderbufferStorage, glFramebufferTexture2D, glFramebufferRenderbuffer, glCheckFramebufferStatus: GL errors / incomplete framebuffers are logged with the caller (first 64); `.dump_textures` in the game folder dumps compressed uploads of 1024x1024 or more |
| `RT_GL_BLIT` | gl_blit.c | 0 | pvz 1, sonic 1 | builds the `dcr_blit_*` functions (GLES 1 overlay drawing). lab2 has gl_blit.c but never calls it: 0 |
| `RT_BOOST_WATCH_THREAD` | dcr_boost.c | 0 | sonic 1, flappy 1 | a thread (0x4000 stack, priority 0x2C, core -2) calls `dcr_boost_poll` every 10 ms, started by `dcr_boost_launch_begin` when boost is on |
| `RT_BOOST_LAUNCH_AFTER_PICTURE_MS` | dcr_boost.c | 0 | dcr 25000 | >0: the start-up boost lasts until `dcr_launch_ready()` or this long after `dcr_boost_first_picture()` (checked by `dcr_boost_launch_tick()`); the start-up log line says which |
| `RT_MAIN_THREAD_NAME` | dcr_boost.c | `"main"` | dcr `"UnityMain"` | the main thread's name in the `[frame]` / `[cpu]` lines |
| `RT_AUDOUT_FRAMES` | rt_audout.h (ports read it too) | 1024 | flappy 512 | frames per audout buffer (three queued). Buffers are whole pages (0x1000 for both values), `data_size` = frames x 4. The wait for a free buffer sleeps 1 ms at <= 512 frames, 2 ms above (flappy / everyone today) |
| `RT_PAD_MAX_PLAYERS` | rt_pad.h | 8 | dcr 8, a8r 8, flappy 8, lab2 2, pvz 2, sonic 2, abs 1 | used by `rt_pad_setup(0, ...)`. The default is a tie (8 vs 2 three ports each); 8 lets any controller connect. Ports should set it |

`RT_SCREEN_W/H` (rt_settings.h) replaces `DCR_FORCE_SCREEN_W/H` in the mesa
self-test. All seven ports use 1280x720 today.

### Callbacks (weak; the runtime's default keeps today's behaviour)

| callback | declared in | default | who overrides | what it does |
| --- | --- | --- | --- | --- |
| `uintptr_t port_gl_wrap(const char *name, uintptr_t real)` | gl_layer.h | 0 | sonic (was `ssr_gl_wrap`), a8r (its profiler + card) | every gl* lookup in `dcr_gl_lookup` (mesa): return a wrapper, or 0. With `RT_GL_CHECK`, the check wrapper sits outside it |
| `void port_gl_before_swap(void)` | gl_layer.h | nothing | a8r (`a8r_perf_gpu_end`) | after the present hook and any capture, just before Mesa's eglSwapBuffers |
| `void port_gl_after_swap(uint32_t frame)` | gl_layer.h | nothing | a8r | right after it, before the boost's frame end and the frame hook |
| `int port_cpu_boost_set(int on)` | dcr_boost.h | -1 | sonic, a8r | -1: use appletSetCpuBoostMode(FastLoad); else the port set its own clock: 1 done, 0 no boost now |
| `void port_perf_clocks(void)` | dcr_boost.h | nothing | sonic (`ssr_perf_clocks`), a8r (`a8r_perf_clocks`) | first thing in `dcr_boost_launch_begin`, before the boost-off check |
| `void port_boost_snapshot(void)` | dcr_boost.h | nothing | a8r (`dcr_pc_snapshot(s_pc0)`) | when a long frame passes 50 ms, with the thread and I/O snapshots |
| `int port_boost_format(char *out, size_t cap, int startup)` | dcr_boost.h | 0 | a8r | appends to a long frame's line (startup 0: since the snapshot; out is inside the 700-byte buffer) or to the start-up line (startup 1: 400 bytes, called outside the lock, before the line is printed) |
| `void port_audio_selftest(void)` | rt_audout.h (group F's main.c calls it) | `rt_audout_selftest()` | a port whose sound is not audout's | the start-up audio self-test |

The frame hooks stay function-pointer globals: `dcr_frame_hook` (after each
present, both renderers) and `dcr_present_hook` (before each present, mesa).

### New runtime API

- `int rt_egl_start_glthread(void *display, void *context)` (gl_layer.h): Mesa's
  glthread for a context not yet current. `b_egl_start_glthread` stays as the
  old name.
- `dcr_gl_capture_now()`: the current back buffer, now.
- rt_audout.h: `rt_audout_open() -> 0/-1`, `unsigned rt_audout_rate()`,
  `int rt_audout_submit(const int16_t *frames) -> 0 queued / -1 dropped`,
  `rt_audout_cancel(int on)` (a waiting submit returns -1; buffers already free
  still queue, as `g_stop_thread`/`g_closing` did), `rt_audout_close()` (stop +
  exit; open again works), `rt_audout_selftest()`, `rt_audout_stats(RtAudoutStats *)`
  (submits, underruns, append_fails, dropped). `rt_audout_submit` returns -1
  when audout is not open (it used to write through NULL).
- The pump: `rt_audout_pump_start(fill, ud, prio, core)` runs `fill(out,
  RT_AUDOUT_FRAMES, ud)` then submit, forever, on a 0x10000-stack thread;
  `rt_audout_pause(1)` sleeps 10 ms a turn instead (HOME);
  `rt_audout_pump_stop()` cancels a waiting submit and joins;
  `rt_audout_pump_blocks()` counts buffers filled.
- rt_pad.h: `Result rt_pad_setup(int max_players, int handheld)` =
  `padConfigureInput(n, NpadStandard)` (Handheld style removed when `handheld`
  is 0) then hid command 102 with the u32 list `{0, 1, ..., n-1, 0x20}` (dcr's
  exact bytes for n = 8), logged. `rt_pad_slot(PadState *, slot)` =
  `padInitializeWithMask` for one slot (0-7 players, `RT_PAD_HANDHELD` = 8).
  `rt_pad_is_single`, `rt_pad_single_buttons` (returns buttons unchanged unless
  single), `rt_pad_sideways`, `rt_pad_read(pad, sticks[4])` (buttons + sticks as
  held: lone right Joy-Con's stick as the left, turned; no right stick),
  `rt_pad_style_name` (0 -> "none").

## 2. Migration per port

Everyone:
- Delete `gl_mesa.c`, `gl_null.c`, `gl_layer.h`, `dcr_boost.c` (and
  `gl_blit.c/h` where present) from `source/`.
- Replace local prototypes of `dcr_boost_*` / `dcr_launch_ready` with
  `#include "dcr_boost.h"`, and of `dcr_gl_*` with `#include "gl_layer.h"`.
- Replace the audout core in the audio file (the `AoBuf` typedef through the
  self-test, ~140 lines) with `rt_audout.h`: `ao_open`/`dcr_audio_open` ->
  `rt_audout_open`, `submit` -> `rt_audout_submit`, `g_out_rate` ->
  `rt_audout_rate()`, `FRAMES_PER_BUF`/`FRAMES_PER_BLOCK` -> `RT_AUDOUT_FRAMES`,
  the stop flag in the wait -> `rt_audout_cancel(1)`, stats ->
  `rt_audout_stats`. Delete the port's `*_audio_selftest`; group F's main.c
  calls `port_audio_selftest()`, whose default is `rt_audout_selftest()`.
- Input: `padConfigureInput(...)` (+ dcr/a8r's `set_supported_npad_ids`) ->
  `rt_pad_setup(RT_PAD_MAX_PLAYERS, 1)`; the local `is_single`,
  `single_buttons`, `sideways`, `style_name` -> `rt_pad_*`. For the five
  ports that only called `padConfigureInput`, the explicit u32 list is a new
  (second, identical) hid call.
- Ports that start system applets (keyboard, controller screen) should bracket
  them with `dcr_applet_busy(1/0)` (group E) so the boost stays off there.

| port | settings | files that shrink or go | renames / new definitions |
| --- | --- | --- | --- |
| dcr | `RT_GL_SWAP_ENDS_FRAME 0`, `RT_CAPTURE_TRIES 300`, `RT_BOOST_LAUNCH_AFTER_PICTURE_MS 25000`, `RT_MAIN_THREAD_NAME "UnityMain"`, `RT_PAD_MAX_PLAYERS 8` | dcr_audio.c loses its core (keeps the FMOD pump and resampler, calls `rt_audout_submit`); dcr_input.c loses `set_supported_npad_ids`, `is_single`, `single_buttons`, `style_name`, and the weak `dcr_gl_request_capture` (gl_null.c has it now); `slot_read` can use `rt_pad_read` then its mute | dcr gains `b_eglGetCurrentDisplay`/`b_eglQueryContext`, the frame hooks (NULL), named captures |
| lab2 | `RT_PAD_MAX_PLAYERS 2` | lab_audio.c: core, `lab_audio_selftest`, and `audio_thread` -> `rt_audout_pump_start(mix, NULL, 0x28, 2)`; `g_paused` -> `rt_audout_pause`; `g_mixes` -> `rt_audout_pump_blocks()`; gl_blit.c/h go (unused) | lab_input.c style names change in the log ("the console (handheld)" -> "Joy-Cons, attached"); slot policy (P[0] = padInitializeDefault) stays |
| abs | `RT_PAD_MAX_PLAYERS 1` | abs_audio.c: core + `abs_audio_selftest` (its thread pulls and resamples, so it keeps it, or its fill can move into the pump) | abs_input.c: `rt_pad_setup(1, 1)` |
| pvz | `RT_GL_BLIT 1`, `RT_PAD_MAX_PLAYERS 2` | pvz_audio.c: core + `dcr_audio_selftest` (the AudioOutput resampler stays); gl_blit.c/h go (the runtime's is a superset) | null renderer's vendor is now `PORT_NAME` |
| sonic | `RT_BOOST_WATCH_THREAD 1`, `RT_GL_BLIT 1`, `RT_PAD_MAX_PLAYERS 2` | ssr_audio.c: core + `ssr_audio_selftest` (mixer thread 0x28 core 2 -> pump; the music thread stays); ssr_input.c: `is_single`, `single_buttons`, `sideways`, and `read_pad`'s stick code (`rt_pad_read`, then `swap_ab` and the test pad) | ssr_patch.c: `ssr_gl_wrap` -> `port_gl_wrap`. ssr_perf.c: `int port_cpu_boost_set(int on) { if (!ssr_cpu_managed()) return -1; ssr_cpu_boost(on); return 1; }`, `void port_perf_clocks(void) { ssr_perf_clocks(); }` |
| flappy | `RT_AUDOUT_FRAMES 512`, `RT_BOOST_WATCH_THREAD 1`, `RT_PAD_MAX_PLAYERS 8` | fbf_audio.c: core, `dcr_audio_selftest`, `mixer` -> `rt_audout_pump_start(mix, NULL, 0x2A, -2)` (stack 0x8000 -> 0x10000), shutdown -> `rt_audout_pump_stop(); rt_audout_close();`, `dcr_audio_blocks` -> `rt_audout_pump_blocks`. fbf_window.c is group C's (rt_window.c); gl_mesa/gl_null now expect `dcr_window_prepare/size` from there | fbf_input.c: `rt_pad_setup(8, 1)`; GameCube name kept in `rt_pad_style_name` |
| a8r | `RT_GL_CHECK 1`, `RT_PAD_MAX_PLAYERS 8` | gl_mesa.c goes; its profiler/card wrappers move to a port file as `port_gl_wrap` (glTexImage2D, glCompressedTexImage2D incl. `card_pixels`, glTexSubImage2D, glCompressedTexSubImage2D, glCompileShader, glLinkProgram, glGenerateMipmap, `px_bytes`); dcr_boost.c goes; opensles.c (group C) holds another copy of the audout core + `dcr_audio_out_open/rate/submit` + `dcr_audio_selftest`: those become `rt_audout_*` (a8r_menu_audio.c: `DCR_AUDIO_FRAMES` -> `RT_AUDOUT_FRAMES`) | `port_gl_before_swap() { a8r_perf_gpu_end(); }`, `port_gl_after_swap(f) { a8r_perf_gpu_begin(); dcr_prof_frame_end(f); dcr_prof_frame_begin(); }` (see risks); `port_cpu_boost_set(on) { if (a8r_cpu_hands_off()) return 0; if (a8r_cpu_managed()) { a8r_cpu_boost(on); return 1; } return -1; }`; `port_perf_clocks() { a8r_perf_clocks(); }`; `port_boost_snapshot() { dcr_pc_snapshot(s_pc0); }`; `port_boost_format(out, cap, startup)`: startup 1 -> `dcr_io_report_patterns(); dcr_io_report_readahead(); dcr_pc_format(out, cap, NULL)`, else `dcr_pc_format(out, cap, s_pc0)`, return `strlen(out)`. `b_egl_start_glthread` still links (alias); prefer `rt_egl_start_glthread`. Makefile: drop `-Wl,--wrap=_mesa_is_format_srgb` (no `__wrap_` definition now: link error otherwise); `--wrap=nouveau_bo_new` comes from runtime.mk |

## 3. Open questions and risks

1. **From other groups, declared locally.**
   - `int dcr_applet_is_busy(void)` (group E, rt_applet.h), in dcr_boost.c.
   - `dcr_io_read_stats` (group B, bionic_io.c; it exists in the seed).
   - `dcr_window_prepare/size` (group C, rt_window.c).
   - `dcr_game_root` (group F, dcr_path.h; dcr_path.h still includes config.h
     in the seed, so I didn't include it).
   - `rt_config()` / `RtConfig` (group G): dcr_boost.c includes `rt_cfg.h` if
     it exists (`__has_include`), else declares the DESIGN.md typedef itself.
     DESIGN says `rt_cfg.h`, G's report says `rt_config.h`. If G uses another
     name, the local fallback still links as long as `rt_config()` exists
     with that layout. Remove the fallback once G lands.
2. **runtime.mk (group E)** must pass `-Wl,--wrap=nouveau_bo_new` for the mesa
   renderer. Without it, `__wrap_nouveau_bo_new` is dead code whose
   `__real_` reference sits in a gc'd section. That should link, but it isn't
   verified.
3. **The applet-busy check is new for six ports.** A long frame under a system
   applet is no longer boosted (dcr's hardware finding). A hold
   (`dcr_boost_hold`) is left on during an applet; dcr's code turned off any
   boost, but dcr never holds.
4. **flappy's watch thread** fixes its never-boosted long frames. It needs a
   hardware check: the boost now actually turns on in flappy's loads.
5. **Capture on hardware.** a8r skipped all-black frames on hardware too, and
   gave up after 30 without saving. The runtime keeps lab2/dcr's rule: skip
   only under the emulator, and save the last try. The plan's "always skip
   black" read as "every port gets skip-black". Flag it if a8r wanted
   hardware skipping.
6. **a8r's frame-profiler order.** `dcr_prof_frame_begin()` used to run after
   the boost's frame_end/begin; in `port_gl_after_swap` it runs before them.
   A long frame's boost log (inside frame_end) is then counted in the next
   profiled frame. If that matters, a8r can call `dcr_prof_frame_begin` from
   `dcr_frame_hook` instead, which runs after the boost.
7. **a8r's GL checks now wrap its own wrapper** (check outside,
   `port_gl_wrap` inside). This matches today's order (timing around the real
   call, check after) and the `.dump_textures` dump still sees the original
   upload. One difference: a card-replaced upload is now also error-checked.
8. **Swap callbacks, `port_gl_wrap` and the present hook are mesa only.**
   gl_null calls only `dcr_frame_hook`, as today.
9. **Log text changes** (not behaviour):
   - `[boost] so far: ...` is printed only when something changed, and adds a
     hold's milliseconds.
   - The start-up line gains file reads (a8r's).
   - `rt_pad_style_name` wording is one set for every port.
   - `[audio] audout open` names the buffer count and size.
10. **Pump stack** is 0x10000 for all ports (flappy had 0x8000). Priority and
    core are the port's arguments.
11. **Skipped for now:**
    - the shared resampler: pvz linear, dcr FMOD linear, abs Hermite, pvz's
      per-block `pvz_video_mix`;
    - the controller-applet helper `rt_controller_applet(min, max, explain[],
      colors)` with the applet-busy bracket and held-button mute (dcr_input.c
      ~422-500, ssr_input.c ~348-420, lab_input.c, a8r_menu.c).
12. **`rt_audout` is not thread-safe.** Like the old core, one thread submits.
    The self-test runs before the game's threads.
13. **docs/SETTINGS.md** (integrator) needs the nine settings above.
    `rt_settings.h` needs nothing from me.
