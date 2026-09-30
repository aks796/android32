# Group C (JNI, NDK, imports): notes

Files: `jni_core.c`, `jni.h`, `android_ndk.c`, `opensles.c`, `imports.h`,
`tools/gen_imports.py`, and three new ones: `rt_window.c`, `rt_window.h`,
`imports_lookup.c`.

Compile status: all five `.c` files compile warning-free with
`tools/check.sh` (default settings). I also compiled them once by hand in the
same container with every setting flipped:

```text
-DRT_OPENSLES=1 -DRT_JNI_UNHANDLED_INSTANCE_SINGLETON=1
-DRT_JNI_UNHANDLED_BUILDER_RETURNS_SELF=1 -DRT_LOOPER_MAIN_IMPLICIT=0
-DRT_ACONFIG_LANG="zh" -DRT_ACONFIG_COUNTRY="CN"
```

A third build force-included the real `bionic.h`, `bionic_io.h`,
`dcr_path.h`, `rt_cfg.h` and `rt_audout.h` alongside my local declarations,
to prove they match. No file includes `config.h`. The game-name grep from
DESIGN.md finds nothing.

What changed, per file:

| file | base | change |
| --- | --- | --- |
| jni_core.c | abs (= sonic) | Header comment made generic. `#include "rt_settings.h"`. Weak `port_jni_invoke` hook, called first in `invoke()`. dcr's unhandled-call fallbacks, behind `RT_JNI_UNHANDLED_INSTANCE_SINGLETON` / `RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF`. 8 `jni_h_*` constant handlers. `jni_init` sets `g_jni_log` from `rt_config()->log_jni`. `jni_str_fmt` uses `b_vsnprintf` (NULL-safe `%s`). The classes.txt message no longer names `tools/stage_sd.py`. Kept: field storage, wildcard entries, `jni_live_objects`, the 233-slot enum with its `_Static_assert`, "unhandled String returns \"\"". |
| jni.h | the 6-port copy | Declares `port_jni_invoke`, the `JNI_H_DECL` macro (token-identical to dcr's) and the 8 `jni_h_*` handlers. Documents the wildcard (`name` NULL) entries. dcr's proxy/looper/`jni_h_*` declarations are **not** here; they move to dcr (below). |
| android_ndk.c | lab2 (= abs = sonic) | The window code moved to rt_window.c; the `b_ANativeWindow_*` shims stay here and call it. `MAX_LOOPERS` 32. Settings: `RT_LOOPER_MAIN_IMPLICIT`, `RT_ACONFIG_LANG/COUNTRY`, `RT_OPENSLES`. dcr's 15 `ASensor*` stubs and 5 `AInputEvent`/`AKeyEvent` getters (struct renamed `RtInputEvent`, same layout). The OpenSL refusal is inside `#if !RT_OPENSLES` and now defines the same 5 `SL_IID_*` symbols as opensles.c. Dropped: the unused `b_read` prototype. |
| rt_window.c/.h | lab2's window code (= flappy's fbf_window.c) | `dcr_window_size`, `dcr_window_set_size`, `dcr_window_prepare`, and new `rt_window_set_geom(NWindow*, w, h)` (was the static `window_set_geom`). Starts at `RT_SCREEN_W` x `RT_SCREEN_H`. |
| opensles.c | a8r | Whole file inside `#if RT_OPENSLES`. The private audout core (~140 lines) is gone; it is now group D's rt_audout (see "opensles.c on rt_audout" below). Game names removed from comments. |
| imports.h | identical x7 | Declares the weak data `port_imports[]`/`port_imports_count`, the callback `port_module_imports`, and the new `rt_import_find`. `dcr_imports` stays non-const. |
| imports_lookup.c | new (was the generated tail of each imports.c) | `dcr_import_lookup`: port_imports, then dcr_imports. `rt_import_find(module, name)`: port_module_imports(module), then port_imports, then dcr_imports; returns the entry. Weak default `port_module_imports`. |
| tools/gen_imports.py | lab2 | Generic (below). |

### opensles.c on rt_audout

This is the integrator's request.

- `ao_open` becomes `rt_audout_open`, `submit` becomes `rt_audout_submit`,
  `g_out_rate` becomes `rt_audout_rate()`, and the periodic stats line reads
  `rt_audout_stats`.
- `FRAMES_PER_BUF` is `RT_AUDOUT_FRAMES`. a8r uses the default 1024, the old
  value. The buffers are still one 0x1000 page, `data_size` 4096, and the
  wait for a free buffer is 2 ms.
- The audio thread is unchanged: priority 0x2A, core 2, the same linear
  resampler and the same callback order.
- The exported names stay: `dcr_audio_out_open/rate/submit` are wrappers.
  `dcr_audio_selftest` calls `rt_audout_selftest`. `dcr_audio_writes`,
  `dcr_audio_pause` and `dcr_audio_close` are unchanged.
- One new line: `dcr_audio_close` calls `rt_audout_cancel(1)`, so an audio
  thread waiting for a buffer at exit returns. The plan calls cancel GENERIC.
- Why not keep a private copy: rt_audout.c is linked into every port, and
  its weak `port_audio_selftest` runs at boot. Two cores on one AudioOut
  session would each reap the other's released buffers, which can hang
  `free_buffer`. The second `audoutStartAudioOut` could also fail.

## 1. Settings and callbacks

### Settings

Each default sits in the file that reads it.

| name | file | default | ports | what it does |
| --- | --- | --- | --- | --- |
| `RT_JNI_UNHANDLED_INSTANCE_SINGLETON` | jni_core.c | 0 | dcr 1 | An unhandled static `instance()`/`getInstance()`/`sharedInstance()` with `()` args answers `jni_singleton(cls)` instead of null. |
| `RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF` | jni_core.c | 0 | dcr 1 | An unhandled instance method on a `...Builder`, or one declared to return the receiver's class, answers the receiver (`build`/`create` excepted). |
| `RT_LOOPER_MAIN_IMPLICIT` | android_ndk.c | 1 | dcr 0 | `ALooper_forThread` on the main thread makes its looper. With 0, only threads that called `ALooper_prepare` have one. |
| `RT_ACONFIG_LANG` | android_ndk.c | `"en"` | pvz `"zh"` | AConfiguration language until `dcr_config_locale()` sets one. |
| `RT_ACONFIG_COUNTRY` | android_ndk.c | `"US"` | pvz `"CN"` | AConfiguration country, likewise. |
| `RT_OPENSLES` | android_ndk.c **and** opensles.c (same default in both) | 0 | a8r 1 | 1: opensles.c implements OpenSL ES over rt_audout. 0: `slCreateEngine` is refused, and opensles.c compiles to nothing. |

Shared settings used: `RT_SCREEN_W/H` (rt_settings.h). Every port has
1280x720 (`DCR_FORCE_SCREEN_W/H` today), the default. `RT_AUDOUT_FRAMES`
(rt_audout.h) is used by opensles.c.

### Callbacks and optional data

| name | declared in | default | who defines it | what it does |
| --- | --- | --- | --- | --- |
| `int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out)` | jni.h | weak, returns 0 | dcr | Sees every call through a method ID first. Return 1 with `*out` set if handled. |
| `const DynLibFunction port_imports[]` + `const int port_imports_count` | imports.h (weak refs) | absent (NULL) | lab2 (its GL overrides) | Searched before `dcr_imports` by `dcr_import_lookup`, so by so_resolve and dlsym alike. |
| `const DynLibFunction *port_module_imports(const char *module, int *count)` | imports.h | weak, NULL / 0 | nobody yet | A per-module table, searched first by `rt_import_find(module, name)`. See open question 2. |

Undefined weak data in this link (`-pie --no-dynamic-linker`, binutils
2.46): I linked a test the same way. The GOT slots stay 0 and no
relocation is emitted, so `port_imports == NULL` holds when a port
defines none.

### New API

- `jni_h_void`, `jni_h_false`, `jni_h_true`, `jni_h_zero`, `jni_h_minus1`,
  `jni_h_null`, `jni_h_empty_string` (a new `""`), and `jni_h_self` (the
  receiver, retained). Also the `JNI_H_DECL(fn)` macro. Ports may use them
  in their tables instead of their own `h_void`/`h_false`/... copies.
- `void rt_window_set_geom(NWindow *w, u32 width, u32 height)` (rt_window.h).
- `const DynLibFunction *rt_import_find(const char *module, const char *name)`
  (imports.h).
- rt_window.h also declares `dcr_window_size/set_size/prepare`. Today every
  caller declares them locally, and those declarations stay compatible.

Kept exports: every `jni_*`, `g_jni_env`, `g_jni_vm`, `g_jni_log`, `jv_*`,
`dcr_looper_run_main`, `dcr_config_locale`, `dcr_window_*`, every `b_A*`,
`b_sl*` and `b_SL_IID_*`, `dcr_audio_*` (with RT_OPENSLES),
`dcr_imports`, `dcr_imports_count` and `dcr_import_lookup`.

### gen_imports.py

The PASSTHROUGH list is the union of the 7 ports: lab2's, plus sonic `fmax`,
plus a8r `strlcat strptime iscntrl islower ispunct isupper toupper`, plus dcr
`strcasestr exp2f` (`strnlen` was there already). DATA and WEAK_NULL are the
common lists. The `gl[A-Z]*` filter is kept.

It scans exactly the files runtime.mk builds:
- the port's `source/`;
- `runtime/source`, minus the files the port replaces (same base name, `.c`
  or `.S`), minus the ones it lists in `EXCLUDE`, which mirrors the
  Makefile's `RT_EXCLUDE`;
- never the output file itself.

The output no longer contains `dcr_import_lookup`; imports_lookup.c has it.

The per-port inputs sit in the port's `tools/`:
- `imports.cfg`: one directive per line, repeatable, `#` comments.
  - `MODULES`: required.
  - `PASSTHROUGH`, `WEAK_NULL name reason`, `DATA bionic host`, `EXCLUDE file`.
- `imports_needed.txt`: the cache, same format as today.

How a port runs it, from the port's folder (the runtime at `runtime/`):

```sh
python3 runtime/tools/gen_imports.py --libs <apk>/lib/armeabi-v7a  # refresh the cache (needs pyelftools)
python3 runtime/tools/gen_imports.py          # write source/imports.c
python3 runtime/tools/gen_imports.py --check  # verify only
```

`--port DIR`, `--cfg`, `--needed`, `--source` and `--out` override the
defaults. I used them to test without touching any port.

**Host test.** I ran it against every port's cached `imports_needed.txt`
into the scratchpad in two setups:
- (A) the port's `source/` as it is today;
- (B) only the port's own files plus the runtime, which is the post-migration
  state.

I then compared the tables row by row with each port's current `imports.c`.
Both setups give the same result:

| port | imports (gl* left to GL) | rows | differences from today's imports.c |
| --- | --- | --- | --- |
| dcr | 412 (0) | 407 | none (identical; dcr's old script had no gl filter, and it imports no gl*) |
| lab2 | 89 (53) | 86 | `snprintf sprintf` now `b_*` |
| abs | 246 (68) | 246 | `snprintf sprintf vsnprintf vsprintf` now `b_*` |
| pvz | 411 (70) | 411 | the same 4 plus `vasprintf` now `b_*` |
| sonic | 129 (58) | 126 | `snprintf sprintf vsnprintf vsprintf` now `b_*` |
| flappy | 93 (37) | 93 | `sprintf vsprintf` now `b_*` |
| a8r | 317 (144) | 317 | `snprintf sprintf vsnprintf vsprintf` now `b_*` |

The only difference is expected. The runtime now has bionic_printf.c, so
the printf family binds to its NULL-safe `b_*` shims instead of newlib
directly. That is DESIGN's `RT_NULL_SAFE_PRINTF=1`; with 0, group B's shims
pass straight through. No weak import changed binding: the new PASSTHROUGH
entries and the new runtime shims bind nothing that was NULL before. The
"weak imports bound to NULL" header line now says "none" instead of being
empty. The generated file compiles.

An earlier run found a8r's `compressBound deflateInit_ zlibVersion` MISSING
in setup B. Group B's bionic_zlib.c has had them since.

## 2. Migration per port

**Every port**

- Delete the port's `source/jni_core.c`, `jni.h`, `android_ndk.c` and
  `imports.h`, and `tools/gen_imports.py`.
- Add `tools/imports.cfg` (per port, below). Regenerate `source/imports.c`
  with `python3 runtime/tools/gen_imports.py`.
  - An old imports.c still defines `dcr_import_lookup`, so it will not link
    next to imports_lookup.c (duplicate definition). Regenerate it; don't
    hand-edit it.
- `g_jni_log = dcr_config()->log_jni;` after `jni_init()` is now redundant,
  because `jni_init` sets it from `rt_config()->log_jni`, which group G reads
  from `[debug] log_java_calls`. It is harmless to keep. The lines to remove:
  - lab_java.c:254
  - abs_java.c:603
  - pvz_java.c:456
  - ssr_java.c:426
  - fbf_java.c:70
  - a8r_java.c:828
- Optional: replace local constant handlers with `jni_h_*`. Candidates:
  - lab_java.c `h_void`;
  - abs_java.c `h_void h_false h_null h_self`;
  - pvz_java.c `h_empty_string h_null h_false h_zero h_void`;
  - ssr_java.c `h_void h_false h_true h_zero h_minus1 h_null`;
  - a8r_java.c `h_void h_false h_true h_zero h_empty`;
  - dcr jni_android.c `noop ret_null ret_true ret_false ret_zero ...`.

  They are static, so nothing collides.
- `DCR_FORCE_SCREEN_W/H` in config.h: nothing to set (`RT_SCREEN_W/H` default
  1280x720). Local `void dcr_window_size(int *, int *);` declarations may
  become `#include "rt_window.h"`.
- Tables with a NULL `name` (wildcard) now work in every port. pvz, a8r,
  flappy and dcr had `strcmp(d->name, ...)`, which would crash on one. None
  of them has such an entry, so nothing changes.

**dcr** (own lineage)

- `port_config.h`:
  - `#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON 1`
  - `#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 1`
  - `#define RT_LOOPER_MAIN_IMPLICIT 0`
- Move dcr's extra `jni.h` declarations into a dcr header (for example
  `source/dcr_jni_unity.h`, which includes "jni.h"):
  - `jni_is_proxy`, `jni_proxy_call`;
  - `jni_looper_bind_engine`, `jni_looper_run_engine`;
  - the 23 `JNI_H_DECL(jni_h_newInterfaceProxy ... jni_h_rh_newProxyInstance)` lines;
  - `jni_release_obtained`.

  Drop its `#define JNI_H_DECL`: jni.h has the same macro, and jni_www.c /
  jni_android.c's `JNI_H_DECL(jni_h_www_*)` keep working. Include the new
  header from dcr_boot.c, dcr_input.c, jni_android.c, jni_proxy.c and
  jni_www.c.
- Define the proxy hook, in jni_proxy.c. This replaces the two lines dcr had
  at the top of `invoke()`:
  ```c
  int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out) {
    if (m->is_static || !jni_is_proxy(self))
      return 0;
    *out = jni_proxy_call(self, m, args);
    return 1;
  }
  ```
- Field storage is new to dcr. `SetXxxField` / `SetStaticXxxField` on a JObj
  (a class object for statics) is now stored. `GetXxxField` reads the stored
  value back when the field has no `JFieldDef` (a def still wins).
  - Before: "ignored write to field" and zero.
  - Check old dcr debug.logs for `ignored write to field` to see which
    fields change behaviour.
- The looper is new to dcr. It moves from the condvar looper to the fd/futex
  looper (the other 6 ports'). `ALooper_wake` bumps bionic_io's activity
  futex, and `pollOnce` sleeps on it. `forThread` never creates a looper
  (setting 0).
  - `pollOnce` returns `ALOOPER_POLL_ERROR` instead of `TIMEOUT` if the table
    of 32 is full.
  - HW test: Unity's waits on its loopers.
- ASensor stubs and AInputEvent getters are unchanged. `DcrInputEvent` is now
  `RtInputEvent`, with the same layout. Nothing in dcr builds these events.
- `jni_live_objects()` is new (one atomic add per object).
- `tools/imports.cfg`:
  ```text
  MODULES libmain.so libunity.so libmono.so
  ```

**lab2**
- `tools/imports.cfg`:
  ```text
  MODULES liblabyrinthii.so
  ```
- Optional, per group A's notes: move `lab_gl_overrides` to
  `const DynLibFunction port_imports[]` and `const int port_imports_count`
  (in lab_gfx.c:356-361 and lab.h:294-295). Then lab_loader.c:125-131 can
  pass `dcr_imports` straight to so_resolve. The overrides still win over
  the GL layer.

**abs**
- `tools/imports.cfg`:
  ```text
  MODULES libAngryBirdsSpace.so
  ```
- Its `b_AAssetManager_*` stay in abs's own source; the scan finds them there.

**pvz**
- `port_config.h`:
  - `#define RT_ACONFIG_LANG "zh"`
  - `#define RT_ACONFIG_COUNTRY "CN"`
- pvz_boot.c: rename to the runtime's names.
  - `pvz_looper_run_main` becomes `dcr_looper_run_main` (lines 425, 428).
  - `pvz_config_locale` becomes `dcr_config_locale` (the prototype at line
    55 and the call at 363).
- libHomura's socket table: keep passing it to so_resolve as its own table.
  Group A's so_resolve searches it first. Do not move it to `port_imports`:
  that would apply to every module and to dlsym.
- `tools/imports.cfg`:
  ```text
  MODULES libnative_code.so libGameMain.so libHomura.so
  ```

**sonic**
- `tools/imports.cfg`:
  ```text
  MODULES libssasr.so
  ```
- Its old extra `fmax` is in the union now.

**flappy**
- Delete `source/fbf_window.c`. rt_window.c defines the same three functions,
  and keeping the file would cause duplicate symbols. fbf.h's declarations
  stay valid.
- flappy gains android_ndk.c. It imports nothing from libandroid, so
  `--gc-sections` drops the shims.
- `tools/imports.cfg`:
  ```text
  MODULES libflapfire.so
  ```

**a8r**
- `port_config.h`: `#define RT_OPENSLES 1`.
- Delete its `opensles.c`.
- main.c's `dcr_audio_selftest()` call becomes group F's `port_audio_selftest()`
  (default `rt_audout_selftest`, the same test). `dcr_audio_selftest` still
  exists for now.
- a8r.h's `dcr_audio_*` declarations are unchanged. `DCR_AUDIO_FRAMES` 1024
  equals `RT_AUDOUT_FRAMES`, and a8r_menu_audio.c may use rt_audout directly.
- `tools/imports.cfg`:
  ```text
  MODULES libasphalt8.so
  ```
- Its extras (`strlcat strptime iscntrl islower ispunct isupper toupper`)
  are in the union now.

## 3. Open questions and risks

1. **Cross-group declarations kept local**, so group C compiles in any commit
   order. A build that force-includes the real headers proves they match.
   Once B, F and G are committed they can become includes:
   - jni_core.c: `b_vsnprintf` (group B, bionic.h), `dcr_game_root`
     (group F, dcr_path.h), and `RtConfig`/`rt_config`. The last one is
     `#if __has_include("rt_cfg.h")` (group G), with the agreed struct as the
     fallback. It picks up the real header now.
   - android_ndk.c: `dcr_fd_activity/wait/seq/readable` (group B, bionic_io.h).
   - opensles.c includes group D's `rt_audout.h`, but only inside
     `#if RT_OPENSLES`, so it needs D only when enabled.
2. **`port_module_imports` has no caller.** Group A's so_resolve takes
   per-module tables through its `funcs` argument (pvz's libHomura) and calls
   only `dcr_import_lookup`. Two options:
   - A calls `rt_import_find(mod->base_name, name)`, making the callback live;
   - drop `port_module_imports` from the fixed interface.

   It is harmless as it stands.
3. **`RT_OPENSLES` is read by two files.** DESIGN puts such defaults in
   rt_settings.h, which is the integrator's file. Both files carry the same
   `#ifndef` default. Please move it to rt_settings.h, then drop mine.
4. **dcr's looper change** (condvar to futex) and **dcr's field storage** are
   the two behaviour changes for dcr. Both need a hardware run. The plan
   flags both.
5. **printf family**: 6 ports now bind `snprintf`/`sprintf`/`vsnprintf`/
   `vsprintf`/`vasprintf` to the NULL-safe `b_*` shims (table above). This is
   intended by DESIGN (`RT_NULL_SAFE_PRINTF`, group B). HW-proven only on dcr.
6. **The scanner is textual.** A shim under `#if` counts as defined:
   - `b_slCreateEngine` in both android_ndk.c and opensles.c;
   - `b_egl*` in gl_mesa.c and gl_null.c.

   So the android_ndk.c stub now defines all five `SL_IID_*`, as opensles.c
   does, and the table doesn't depend on `RT_OPENSLES`. A port that excludes
   a runtime file must list it in `EXCLUDE` as well as `RT_EXCLUDE`.
   Otherwise the link, not the generator, reports the gap.
7. **opensles.c on rt_audout** has not run on hardware. The audout calls are
   the same, and rt_audout's buffers and wait match a8r's old ones (see
   "opensles.c on rt_audout" above).
8. The `slCreateEngine` refusal log says "sound goes through the game's Java
   audio" instead of pvz's "Java AudioOutput". The text changed; the
   behaviour did not.
9. runtime.mk globs `source/*.c`, so it builds the new `rt_window.c` and
   `imports_lookup.c`. Nothing to add.
