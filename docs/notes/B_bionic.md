# Group B (bionic): notes

Files: `bionic.h`, `bionic_io.h`, `bionic_pthread.h`, `dcr_time.h`, `dcr_net.h`,
`bionic_core.c`, `bionic_cxx.c` (new), `bionic_dl.c`, `bionic_extra.c`,
`bionic_io.c`, `bionic_math.c`, `bionic_mem.c`, `bionic_net.c`,
`bionic_printf.c`, `bionic_pthread.c`, `bionic_setjmp.S` (unchanged),
`bionic_sha1.c` (unchanged), `bionic_signal.c`, `bionic_stdio.c`,
`bionic_time.c`, `bionic_wchar.c`, `bionic_zlib.c`.

Base: lab2's copy for every file (the seed). No runtime file includes
`config.h`; they read `rt_settings.h` (`PORT_PACKAGE`, `PORT_NAME`) and keep
their own `RT_*` defaults next to the code.

Compile status: all 17 sources compile warning-free with `tools/check.sh`.
The non-default settings (every `RT_*` below at its other value) were also
compiled warning-free with a scratch copy of check.mk's flags plus `-D`s.
`RT_TIME_SHIFT=2` stops with the intended `#error`. `nm` over test/build
shows no duplicate strong symbols. The only overlap is `dcr_cwd`, which is
weak in F's dcr_path.c and strong here. Every undefined symbol of the bionic
objects is defined by a runtime object, libnx, newlib or miniz.

## 1. Settings and callbacks

### Settings (`port_config.h`; the default is in the file named)

| Setting | Default | Per-port value | What it does |
| --- | --- | --- | --- |
| `RT_TIME_SHIFT` (bionic_time.c) | `0` | lab2 `1` (`RT_TIME_SHIFT_REALTIME`); a8r `3` (`RT_TIME_SHIFT_REALTIME \| RT_TIME_SHIFT_TIME`); others 0 | Which wall clocks run on the process's own time (freezes left out). REALTIME covers `clock_gettime(REALTIME/_COARSE)`, gettimeofday and ftime; TIME covers `time()`. TIME without REALTIME is an `#error`. The bit names are in dcr_time.h; port_config.h may also use the numbers. |
| `RT_TIME_FREEZE_WATCH` (bionic_time.c) | `1` | all 1 (flappy gains it) | The freeze watch thread. 0 means no freeze detection at all. |
| `RT_TIME_WATCH_PRIO` | `0x2C` | lab2 `0x2B` | The watch thread's libnx priority. |
| `RT_TIME_WATCH_CORE` / `_STACK` | `-2` / `0x4000` | all | The watch thread's core and stack size. |
| `RT_TIME_WATCH_NS` / `RT_TIME_FREEZE_NS` | 100 ms / 2 s | all | Reading period; the gap that counts as a freeze. |
| `RT_PROC_COMM` (bionic_io.c) | the last 15 characters of `PORT_PACKAGE` | dcr `"disneycrossyro"`. The default gives: lab2 `labs.labyrinth2` (was `se.illusionlabs`); abs `grybirdsspaceHD` (was `com.rovio.angry`); pvz `com.trans.pvztv` (same); sonic `com.sega.ssasr` (was lab2's, a copy bug); flappy `tgears.flapfire` (same); a8r `d.HEP.GloftA8HP` (was pvz's, a copy bug) | comm in /proc/self/stat and /proc/self/status. |
| `RT_EXTRA_ENV` (bionic_core.c) | none | dcr `"MONO_DEBUG=explicit-null-checks"` (load-bearing) | Comma-separated `"NAME=value"` strings placed ahead of the runtime's environment. |
| `RT_IO_READAHEAD` (bionic_io.c) | `0` | a8r `1` | a8r's read-ahead buffers for read-only fds. |
| `RT_IO_PATTERN_STATS` (bionic_io.c) | `0` | a8r `1` | Per-fd read-pattern counters for `dcr_io_report_patterns`. |
| `RT_STDIO_READ_BUF` (bionic_stdio.c) | `0` | a8r `32768` | setvbuf size for FILEs opened only for reading. |
| `RT_NULL_SAFE_PRINTF` (bionic_printf.c) | `1` | all 1 | A NULL `%s` prints "(null)". runtime.mk must always pass `-Wl,--wrap=_svfprintf_r -Wl,--wrap=_vfprintf_r`. With 0 the wraps call `__real_*` straight. |
| `RT_ZLIB_HOLDBACK` (bionic_zlib.c) | `1` | dcr `0` (until tested there) | inflate's hand-back of the last input byte (the libpng fix). |
| `RT_DL_HAS_OPENSLES` (bionic_dl.c) | `1` | dcr `0` | Whether `dlopen("libOpenSLES.so")` succeeds. dcr's FMOD must fall back to its Java output. |
| `RT_LOG_THREAD_CREATE` (bionic_pthread.c) | `0` | a8r `1` | Log the first 48 new threads and the code addresses on their creator's stack. |
| `RT_COOP_SAMPLE` (bionic_pthread.c) | `1` | all | Cooperative sampling for the watchdog: `g_sample_*`, checked in mutex lock and getspecific. |

### Callbacks (weak defaults in group B files)

| Callback | Declared in | Default | Who defines it |
| --- | --- | --- | --- |
| `int port_net_owns(int fd)`, `port_net_close`, `port_net_fcntl`, `port_net_ioctl`, `short port_net_ready(fd, events)` | dcr_net.h (defaults in bionic_net.c) | owns nothing | pvz (was `pvz_net_*`), sonic (was `dcr_net_*`) |
| `int port_net_socket(domain, type, proto)` | dcr_net.h | -1 | sonic (was `ssr_net_socket`; -2 "ours, but full" still means the runtime makes its offline socket) |
| `port_net_bind`, `port_net_sendto`, `port_net_recvfrom` | dcr_net.h | called only for owned fds | sonic (were `ssr_net_*`) |
| `uintptr_t port_thread_tag_for_new(void)`, `void port_thread_tag_enter(uintptr_t)` | bionic_pthread.h (defaults in bionic_pthread.c) | 0 / nothing | sonic (`ssr_engine_current` / `ssr_engine_set_current`) |
| `int port_gc_signal(BThread *, int sig)` | bionic_pthread.h (bionic_signal.c) | 0 | dcr (was `dcr_gc_signal`, mono_rt.c:198) |
| `void port_on_fatal_signal(int sig, BThread *t)` | bionic_pthread.h (bionic_signal.c) | nothing | dcr: `dcr_mono_log_managed_stack("[fatal]")` |
| `uint64_t port_prof_begin(void)`, `void port_prof_end(int counter, uint64_t t0, uint64_t bytes)` | bionic.h (bionic_zlib.c) | 0 / nothing | a8r. The `RT_PROF_*` ids in bionic.h use a8r's `PC_*` order, so it can pass them straight through. Group D may use the same ids for GL. |

Used from other groups: `port_import_interpose` (A, so_util.h) on every
dlsym result; `cs_mmap`/`cs_munmap`/`cs_mprotect`/`cs_write`/`cs_rw_alias`/
`g_cs_armed` (A, codespace.h); `dcr_import_lookup` (C, imports.h) in dlsym's
system lookup.

### OVERRIDE (weak in the runtime)

`b_mmap`, `b_munmap`, `b_mremap`, `b_mprotect`, `b_mmap_bytes`, `b_memcpy`,
`b_memmove` and `b_memset` (bionic_mem.c). The `__aeabi_*` and `__*_chk`
forms call `b_memcpy`/`b_memmove`/`b_memset`, so a strong port copy of those
three covers them.

### New exports

- `b_pthread_create_on` (from a8r).
- `b_lock_words`, `b_thread_foreach_nolock` and
  `g_sample_want/sp/lr/n` (from dcr; E's watchdog uses them).
- `BThread.gc_paused` and `BThread.port_tag`.
- `__thread int dcr_io_tagged_thread` and `dcr_io_gl_stats` (from a8r).
- `dcr_io_report_readahead` and `dcr_io_report_patterns`, always defined
  (they do nothing when their setting is off).
- `b_socket_close` in bionic_io.h.
- The fd-activity API (`dcr_fd_activity/wait/seq/readable`) and `dcr_cwd`,
  declared in bionic_io.h.
- `b_safe_format` and `b_vsnprintf`, declared in bionic.h.
- The `RT_PROF_*` ids and the `RT_TIME_SHIFT_*` bits.

New `b_*` shims:
- From lab2's lab_bionic.c: `__cxa_*` guards, pure virtual, `_Znwj`/`_Znaj`,
  `_ZdlPv`/`_ZdaPv`, and `get_crc_table`.
- From a8r's bionic_a8r.c: the nothrow new/delete (plus `_ZdaPv` nothrow),
  `_ZSt7nothrow`, `__srget`, `tmpnam`, `pread`, `times`, `getgid`/`getegid`
  and `gethostbyaddr`; from a8r's zlib: `deflateInit_`, `compressBound` and
  `zlibVersion`.
- From sonic: `__isfinitef`.
- From abs: `pthread_condattr_init/destroy`,
  `pthread_cond_timedwait_monotonic(_np)`, `pthread_rwlock_*`, `readdir_r`,
  `memrchr`, `dup` and `getauxval`.

### How the time core works (bionic_time.c)

The core is lab2/a8r's run clock. One lock guards everything:
- `run_ns_locked()` reads the tick. If the watch thread runs and more than
  `RT_TIME_FREEZE_NS` passed since the last reading, the gap minus
  `RT_TIME_WATCH_NS` goes into `g_frozen_ns`. Then it updates `g_last_read`.
- Every path calls it first: readings, the watch thread, and
  `dcr_time_suspend`/`dcr_time_resume`. This is the equivalent of dcr's
  ffa6292 fix. A focus message handled right after a thaw now removes the
  freeze before it stamps or ends a suspension.
- Suspensions are measured in run time, so a freeze inside one is not
  removed twice.
- `dcr_monotonic_ns` takes a reading even while suspended, so the freeze
  check still runs.
- Only the watch thread logs, outside the lock. That covers both the freeze
  line and the "resumed after" line. There is no watch-thread callback:
  E's `rt_applet_poll` polls `dcr_time_freezes()`.
- Freeze detection is off until the watch thread runs. `start_watch()` sets
  the last reading when it starts, so boot time before `dcr_time_init` never
  counts as a freeze.

## 2. Migration per port

Every port deletes its copies of all group B files: `bionic*.c/.h/.S`,
`dcr_time.h` and `dcr_net.h`. It keeps none of them unless listed below.

**dcr**
- Settings: `RT_PROC_COMM "disneycrossyro"`,
  `RT_EXTRA_ENV "MONO_DEBUG=explicit-null-checks"`, `RT_DL_HAS_OPENSLES 0`,
  `RT_ZLIB_HOLDBACK 0`.
- mono_rt.c: rename `dcr_gc_signal` to `port_gc_signal` (same signature).
  Add `void port_on_fatal_signal(int sig, BThread *t)` that calls
  `dcr_mono_log_managed_stack("[fatal]")`. The signal.c hook for that call
  is gone.
- dcr_icall_hooks.c: `dcr_icall_interpose` becomes `port_import_interpose`
  (A's note too). dlsym now calls it on every result, as dcr's did.
- **Keep as overrides:** its JIT-arena `b_mmap`, `b_munmap`, `b_mremap`,
  `b_mprotect` and `b_memcpy`/`b_memmove`/`b_memset` (and `b_mmap_bytes` if
  it wants its own count).
  - Put them in a port file with another name (e.g. `dcr_jit_mem.c`) and
    drop the rest of its bionic_mem.c. The runtime's `__aeabi_*`/`__*_chk`
    and `b_madvise` then call dcr's.
  - Or keep the whole file as `bionic_mem.c`, as a file override.
  - To drop the overrides later, dcr can become a codespace provider (A's
    note, §5). It would also need a way for `MAP_FIXED` into arena memory:
    the runtime refuses addresses outside its own mapping table. dcr logs
    that case ("MAP_FIXED over JIT memory"). The runtime already asks
    `cs_mmap` for PROT_EXEC file mappings and fills them through
    `cs_rw_alias`.
- dlopen now finds modules by name in the so list; dcr's hard-wired
  `main_mod`/`unity_mod`/`mono_mod` are gone. libmono must be in the list
  only once loaded (A's shared so_util). `dlopen("libvulkan.so")` now fails
  with a "not found" log instead of silently.
- Behaviour changes for dcr (GENERIC in the plan):
  - `exit()`/`_exit()` now end the process with `svcExitProcess` after a
    flush. It was newlib `exit()`. Check Unity's Quit path on hardware.
  - poll/select sleep on the fd-activity futex (was: 1 ms polling).
  - dircache lookups in open, stat, fopen and opendir.
  - `/proc/self/cmdline`.
  - The repeated-line collapser on stdout/stderr.
  - A read-only fopen of the APK is served from the RAM cache.
  - bionic_extra.c's shims.
  - `gmtime_r`/`localtime_r`/`asctime`/`ftime`/`strftime_l`.
  - mbtowc and friends.
  - `__android_log_vprint` uses vsnprintf. That is NULL-safe through the
    wrap, as before.

**lab2**
- Delete `lab_bionic.c`: everything moved.
  - The guards are a8r's pending-bit version: no lock is held while a
    constructor runs. lab2's held a recursive lock.
  - Pure virtual now goes through `fatal_error` (was `abort()`).
- Delete the `dcr_net_*` stubs in lab_loader.c:205-210.
- Settings: `RT_TIME_SHIFT 1`, `RT_TIME_WATCH_PRIO 0x2B`. Optional:
  `RT_PROC_COMM "se.illusionlabs"` keeps the old comm byte for byte.
- lab_boot.c:96's polling of `dcr_time_freezes()` becomes E's
  `port_process_frozen`.

**abs**
- Delete `abs_bionic.c`: everything moved.
- Delete the `dcr_net_*` stubs in abs_loader.c:87-92.
- Optional: `RT_PROC_COMM "com.rovio.angry"` to keep the old comm.

**pvz**
- In pvz_net.c and pvz_net.h, rename `pvz_net_owns/close/fcntl/ioctl/ready`
  to `port_net_*` (same signatures). Its per-module socket table is C's
  `port_module_imports`.
- New for pvz: `b_ferror`/`b_freopen`/`b_tmpfile` (its stdio lacked them).

**sonic**
- Delete `bionic_ssr.c`.
- ssr_net.c: rename `dcr_net_owns/close/fcntl/ioctl/ready` to
  `port_net_*`, and `ssr_net_socket/bind/sendto/recvfrom` to
  `port_net_socket/bind/sendto/recvfrom`, or add `port_*` wrappers around
  them.
- ssr_loader.c: define
  `uintptr_t port_thread_tag_for_new(void) { return (uintptr_t)ssr_engine_current(); }`
  and
  `void port_thread_tag_enter(uintptr_t t) { ssr_engine_set_current((int)t); }`.
  `BThread.engine` is now `BThread.port_tag`.
- The comm becomes `com.sega.ssasr`, which fixes the copy bug.

**flappy**
- Nothing to rename. Its inline `dcr_net_*` stubs go with its bionic_io.c.
- It gains the freeze watch. E's note turns its 2 s frame-gap check into
  `port_process_frozen`.
- It gains `b_ferror`/`b_freopen`/`b_tmpfile`.

**a8r**
- Delete `bionic_a8r.c`.
  - Its freopen/tmpfile give way to the runtime's. `freopen` now reopens
    the same stream at the translated path, instead of fclose + a new FILE.
  - `pread` no longer takes a lock (see risks).
- a8r_perf.c / a8r.h: use the runtime's `dcr_io_tagged_thread` instead of
  `g_a8r_on_gl_thread`. For example, set it on the GL thread, or
  `#define g_a8r_on_gl_thread dcr_io_tagged_thread` in a8r.h and drop the
  definition at a8r_perf.c:219.
- a8r_prof: define `port_prof_begin() { return dcr_pc_begin(); }` and
  `port_prof_end(k, t0, b) { dcr_pc_end(k, t0, b); }`. `RT_PROF_*` equals
  `PC_*`.
- Settings: `RT_TIME_SHIFT 3`, `RT_IO_READAHEAD 1`,
  `RT_IO_PATTERN_STATS 1`, `RT_STDIO_READ_BUF 32768`,
  `RT_LOG_THREAD_CREATE 1`.
- a8r_boot.c's `b_pthread_create_on` call is unchanged, and so is its
  local prototype.
- The comm becomes `d.HEP.GloftA8HP`, which fixes the copy bug.

## 3. Open questions and risks

1. **Private read handle per tracked fd** (the integrator's follow-up to
   889f12c). `b_pread_all`, which serves pread and mmap of files, reads a
   non-APK file through that fd's private handle.
   - Each `g_open[]` entry now carries `own` (the handle, or
     `OWN_NONE`/`OWN_FAILED`/`OWN_DEAD`), `pins` and a Mutex.
   - The handle opens lazily on the fd's first pread, then reads are seek
     + read under the entry mutex. `g_open_lock` is held only to find and
     pin the entry.
   - Every untrack closes it after any read in flight (it takes the entry
     mutex first). That covers `b_close`, `b_fclose`, `b_dup2` onto the fd,
     `b_freopen`, and the stale entry `b_track_open` finds.
   - A read that lost that race returns 0 and never reopens. A slot is
     reused only when unpinned.
   - When the open fails (the file is open for writing: 0xE02), that is
     remembered and every pread falls back to borrowing the fd's position,
     as before. The open is not retried.
   - Risks:
     - While an fd has been pread, a second handle on its file stays open
       until the fd closes. It never outlives the game's own handle.
     - The whole scheme is new code with no hardware run. Check a8r's load
       time and the `[io] pread ... through its own position` lines.
     - An fd made by `dup()`/`F_DUPFD` is untracked, so its pread borrows
       the position, as before.
2. **Behaviour changes not in the plan:**
   - `b_close` of an offline socket now frees its slot and returns 0. Before,
     newlib's `close()` returned -1 EBADF and the slot leaked; after 128
     sockets, `socket()` failed.
   - Read-ahead state is forgotten on `dup2` onto an fd (a8r only).
   - The "(null)" rewrite skips a `%s` with an explicit zero precision.
     Before, dcr's b_ shims rewrote and then the wrap rewrote again, which
     printed "(null)(null)".
   - The b_ printf shims no longer walk the format themselves. The wrap
     walks it once.
3. **Time.**
   - A watch thread starved for 2 s looks like a freeze. With the default
     0x2C it shares its priority with lab2's main thread and sound decoder;
     lab2 sets 0x2B.
   - MONOTONIC stands still while focus-suspended, so E's exit path must
     call `dcr_time_resume()`.
   - The "resumed after" log line now comes from the watch thread, up to
     100 ms later.
   - lab2/a8r created the watch thread before the time-service read; it now
     starts right after it.
4. **pthread default layout.** The arbiter's WaitForAddress layout now
   defaults to int32 everywhere, as in a8r. lab2's lineage chose it with
   `dcr_is_emulator()`, which picked int64 on hardware until the self-test
   switched. `dcr_pthread_selftest()` still checks it at boot.
5. **Fingerprint.** `ro.build.fingerprint` now carries `PORT_NAME`. The
   old per-port strings were "labyrinth2_nx", "abspace_nx", "pvz_nx",
   "sonicracing_nx", "fbf_nx", "a8r_nx" and "dcrsea_nx". This matters only
   if a game keys data on it.
6. **Integrator's files.** `RT_TIME_SHIFT_*` is defined in dcr_time.h, so a
   port_config.h that uses the names only works in bionic_time.c. That is
   the only reader, but numbers (1, 3) are the safe spelling.
   `RT_NULL_SAFE_PRINTF` is read only by bionic_printf.c; the `--wrap`
   flags must stay in runtime.mk (E) regardless of its value.
7. **Duplicate `dcr_cwd`.** F's dcr_path.c has a weak `dcr_cwd`
   (returning `DCR_ANDROID_FILES`); bionic_io.c's strong one (the chdir'd
   directory) wins. That is intended, but F's weak copy is then dead code.
8. **Terraria.** `~/thirtytwo/terrarialegacy` (not one of the seven) has a
   newer bionic_net.c: offline sockets keep F_GETFL/F_SETFL flags, and a
   blocking recvfrom waits 100 ms instead of spinning. This is worth
   merging if that port joins.
9. **DESIGN's name grep** hits only `b_abs_realtime_to_timeout_ns`
   (bionic_pthread.c, "abs_" inside an existing export name): a false
   positive, left as is.
10. **Declared locally from other groups:** nothing remains. `cs_rw_alias`
   and `port_import_interpose` come from A's headers, which were updated
   during this session. My files call `cs_rw_alias(void *, size_t)` as A
   defined it.
